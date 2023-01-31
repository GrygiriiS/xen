/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SCI SCMI multi-agent driver, using SMC/HVC shmem as transport.
 *
 * Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 * Copyright (c) 2025 EPAM Systems
 */

#include <asm/byteorder.h>
#include <asm/io.h>
#include <asm/sci/sci.h>
#include <asm/smccc.h>

#include <xen/bitops.h>
#include <xen/config.h>
#include <xen/sched.h>
#include <xen/device_tree.h>
#include <xen/iocap.h>
#include <xen/init.h>
#include <xen/err.h>
#include <xen/libfdt/libfdt.h>
#include <xen/list.h>
#include <xen/mm.h>
#include <xen/param.h>
#include <xen/string.h>
#include <xen/time.h>
#include <xen/vmap.h>

#include "scmi_proto.h"

#define SCMI_AGENT_ID_INVALID 0xFF

static uint8_t __initdata opt_dom0_scmi_agent_id = SCMI_AGENT_ID_INVALID;
integer_param("dom0_scmi_agent_id", opt_dom0_scmi_agent_id);

#define SCMI_SECONDARY_AGENTS  "xen,scmi-secondary-agents"
#define SCMI_SHMEM_MAPPED_SIZE PAGE_SIZE

#define HYP_CHANNEL 0x0

#define SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE  BIT(0, UL)
#define SCMI_SHMEM_CHAN_STAT_CHANNEL_ERROR BIT(1, UL)

struct scmi_shared_mem {
    uint32_t reserved;
    uint32_t channel_status;
    uint32_t reserved1[2];
    uint32_t flags;
    uint32_t length;
    uint32_t msg_header;
    uint8_t msg_payload[];
};

struct scmi_channel {
    uint32_t agent_id;
    uint32_t func_id;
    domid_t domain_id;
    uint64_t paddr;
    uint64_t len;
    struct scmi_shared_mem *shmem;
    spinlock_t lock;
    struct list_head list;
};

struct scmi_data {
    struct list_head channel_list;
    spinlock_t channel_list_lock;
    uint32_t func_id;
    bool initialized;
    uint32_t shmem_phandle;
};

static struct scmi_data scmi_data;

static inline int channel_is_free(struct scmi_channel *chan_info)
{
    return ( chan_info->shmem->channel_status
            & SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE ) ? 0 : -EBUSY;
}

/*
 * Copy data from IO memory space to "real" memory space.
 */
void __memcpy_fromio(void *to, const volatile void __iomem *from, size_t count)
{
    while (count && !IS_ALIGNED((unsigned long)from, 4)) {
        *(u8 *)to = __raw_readb(from);
        from++;
        to++;
        count--;
    }

    while (count >= 4) {
        *(u32 *)to = __raw_readl(from);
        from += 4;
        to += 4;
        count -= 4;
    }

    while (count) {
        *(u8 *)to = __raw_readb(from);
        from++;
        to++;
        count--;
    }
}

/*
 * Copy data from "real" memory space to IO memory space.
 */
void __memcpy_toio(volatile void __iomem *to, const void *from, size_t count)
{
    while (count && !IS_ALIGNED((unsigned long)to, 4)) {
        __raw_writeb(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }

    while (count >= 4) {
        __raw_writel(*(u32 *)from, to);
        from += 4;
        to += 4;
        count -= 4;
    }

    while (count) {
        __raw_writeb(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }
}

static int send_smc_message(struct scmi_channel *chan_info,
                            scmi_msg_header_t *hdr, void *data, int len)
{
    struct arm_smccc_res resp;
    int ret;

    if ( (len + sizeof(chan_info->shmem->msg_header)) >
                         SCMI_SHMEM_MAPPED_SIZE )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of smc message. Data is invalid\n");
        return -EINVAL;
    }

    printk(XENLOG_DEBUG "scmi: status=%d len=%d\n",
           chan_info->shmem->channel_status, len);
    printk(XENLOG_DEBUG "scmi: header id = %d type = %d, proto = %d\n",
           hdr->id, hdr->type, hdr->protocol);

    ret = channel_is_free(chan_info);
    if ( IS_ERR_VALUE(ret) )
        return ret;

    chan_info->shmem->channel_status = 0x0;
    /* Writing 0x0 right now, but "shmem"_FLAG_INTR_ENABLED can be set */
    chan_info->shmem->flags = 0x0;
    chan_info->shmem->length = sizeof(chan_info->shmem->msg_header) + len;
    chan_info->shmem->msg_header = pack_scmi_header(hdr);

    printk(XENLOG_DEBUG "scmi: Writing to shmem address %p\n",
           chan_info->shmem);
    if ( len > 0 && data )
        __memcpy_toio((void *)(chan_info->shmem->msg_payload), data, len);

    arm_smccc_smc(chan_info->func_id, 0, 0, 0, 0, 0, 0, chan_info->agent_id,
                  &resp);

    printk(XENLOG_DEBUG "scmi: scmccc_smc response %d\n", (int)(resp.a0));

    if ( resp.a0 )
        return -EOPNOTSUPP;

    return 0;
}

static int check_scmi_status(int scmi_status)
{
    if ( scmi_status == SCMI_SUCCESS )
        return 0;

    printk(XENLOG_DEBUG "scmi: Error received: %d\n", scmi_status);

    switch ( scmi_status )
    {
    case SCMI_NOT_SUPPORTED:
        return -EOPNOTSUPP;
    case SCMI_INVALID_PARAMETERS:
        return -EINVAL;
    case SCMI_DENIED:
        return -EACCES;
    case SCMI_NOT_FOUND:
        return -ENOENT;
    case SCMI_OUT_OF_RANGE:
        return -ERANGE;
    case SCMI_BUSY:
        return -EBUSY;
    case SCMI_COMMS_ERROR:
        return -ENOTCONN;
    case SCMI_GENERIC_ERROR:
        return -EIO;
    case SCMI_HARDWARE_ERROR:
        return -ENXIO;
    case SCMI_PROTOCOL_ERROR:
        return -EBADMSG;
    default:
        return -EINVAL;
    }
}

static int get_smc_response(struct scmi_channel *chan_info,
                            scmi_msg_header_t *hdr, void *data, int len)
{
    int recv_len;
    int ret;
    int pad = sizeof(hdr->status);

    printk(XENLOG_DEBUG "scmi: get smc response msgid %d\n", hdr->id);

    if ( len >= SCMI_SHMEM_MAPPED_SIZE - sizeof(chan_info->shmem) )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of input smc message. Data may be invalid\n");
        return -EINVAL;
    }

    ret = channel_is_free(chan_info);
    if ( IS_ERR_VALUE(ret) )
        return ret;

    recv_len = chan_info->shmem->length - sizeof(chan_info->shmem->msg_header);

    if ( recv_len < 0 )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of smc message. Data may be invalid\n");
        return -EINVAL;
    }

    unpack_scmi_header(chan_info->shmem->msg_header, hdr);

    hdr->status = le32_to_cpup((const __le32 *)chan_info->shmem->msg_payload);
    recv_len = recv_len > pad ? recv_len - pad : 0;

    ret = check_scmi_status(hdr->status);
    if ( ret )
        return ret;

    if ( recv_len > len )
    {
        printk(XENLOG_ERR
               "scmi: Not enough buffer for message %d, expecting %d\n",
               recv_len, len);
        return -EINVAL;
    }

    if ( recv_len > 0 )
    {
        __memcpy_fromio(data, chan_info->shmem->msg_payload + pad, recv_len);
    }

    return 0;
}

static int do_smc_xfer(struct scmi_channel *channel, scmi_msg_header_t *hdr, void *tx_data, int tx_size,
                       void *rx_data, int rx_size)
{
    int ret = 0;

    ASSERT(channel && channel->shmem);

    if ( !hdr )
        return -EINVAL;

    spin_lock(&channel->lock);

    ret = send_smc_message(channel, hdr, tx_data, tx_size);
    if ( ret )
        goto clean;

    ret = get_smc_response(channel, hdr, rx_data, rx_size);
clean:
    spin_unlock(&channel->lock);

    return ret;
}

static struct scmi_channel *get_channel_by_id(uint32_t agent_id)
{
    struct scmi_channel *curr;
    bool found = false;

    spin_lock(&scmi_data.channel_list_lock);
    list_for_each_entry(curr, &scmi_data.channel_list, list)
    {
        if ( curr->agent_id == agent_id )
        {
            found = true;
            break;
        }
    }

    spin_unlock(&scmi_data.channel_list_lock);
    if ( found )
        return curr;

    return NULL;
}

static struct scmi_channel *aquire_scmi_channel(struct domain *d,
                                                uint32_t agent_id)
{
    struct scmi_channel *curr;
    struct scmi_channel *ret = ERR_PTR(-ENOENT);

    spin_lock(&scmi_data.channel_list_lock);
    list_for_each_entry(curr, &scmi_data.channel_list, list)
    {
        if ( curr->agent_id == agent_id )
        {
            if ( curr->domain_id != DOMID_INVALID )
            {
                ret = ERR_PTR(-EEXIST);
                break;
            }

            curr->domain_id = d->domain_id;
            ret = curr;
            break;
        }
    }

    spin_unlock(&scmi_data.channel_list_lock);

    return ret;
}

static void relinquish_scmi_channel(struct scmi_channel *channel)
{
    ASSERT(channel != NULL);

    spin_lock(&scmi_data.channel_list_lock);
    channel->domain_id = DOMID_INVALID;
    spin_unlock(&scmi_data.channel_list_lock);
}

static int map_channel_memory(struct scmi_channel *channel)
{
    ASSERT( channel && channel->paddr );
    channel->shmem = ioremap_nocache(channel->paddr, SCMI_SHMEM_MAPPED_SIZE);
    if ( !channel->shmem )
        return -ENOMEM;

    channel->shmem->channel_status = SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE;
    printk(XENLOG_DEBUG "scmi: Got shmem %lx after vmap %p\n", channel->paddr,
           channel->shmem);

    return 0;
}

static void unmap_channel_memory(struct scmi_channel *channel)
{
    ASSERT( channel && channel->shmem );
    iounmap(channel->shmem);
    channel->shmem = NULL;
}

static struct scmi_channel *smc_create_channel(uint32_t agent_id,
                                               uint32_t func_id, uint64_t addr)
{
    struct scmi_channel *channel;

    channel = get_channel_by_id(agent_id);
    if ( channel )
        return ERR_PTR(EEXIST);

    channel = xmalloc(struct scmi_channel);
    if ( !channel )
        return ERR_PTR(ENOMEM);

    spin_lock_init(&channel->lock);
    channel->agent_id = agent_id;
    channel->func_id = func_id;
    channel->domain_id = DOMID_INVALID;
    channel->shmem = NULL;
    channel->paddr = addr;
    list_add_tail(&channel->list, &scmi_data.channel_list);
    return channel;
}

static void free_channel_list(void)
{
    struct scmi_channel *curr, *_curr;

    list_for_each_entry_safe (curr, _curr, &scmi_data.channel_list, list)
    {
        list_del(&curr->list);
        xfree(curr);
    }
}

static int scmi_dt_read_hyp_channel_addr(struct dt_device_node *scmi_node,
                                         u64 *addr, u64 *size)
{
    struct dt_device_node *shmem_node;
    const __be32 *prop;

    prop = dt_get_property(scmi_node, "shmem", NULL);
    if ( !prop )
        return -EINVAL;

    shmem_node = dt_find_node_by_phandle(be32_to_cpup(prop));
    if ( IS_ERR_OR_NULL(shmem_node) )
    {
        printk(XENLOG_ERR
               "scmi: Device tree error, can't parse reserved memory %ld\n",
               PTR_ERR(shmem_node));
        return PTR_ERR(shmem_node);
    }

    return dt_device_get_address(shmem_node, 0, addr, size);
}

/*
 * Handle Dom0 SCMI specific DT nodes
 *
 * Make a decision on copying SCMI specific nodes into Dom0 device tree.
 * For SCMI multi-agent case:
 * - shmem nodes will not be copied and generated instead if SCMI
 *   is enabled for Dom0
 * - scmi node will be copied if SCMI is enabled for Dom0
 */
static bool scmi_dt_handle_node(struct domain *d, struct dt_device_node *node)
{
    static const struct dt_device_match skip_matches[] __initconst = {
        DT_MATCH_COMPATIBLE("arm,scmi-shmem"),
        { /* sentinel */ },
    };
    static const struct dt_device_match scmi_matches[] __initconst = {
        DT_MATCH_PATH("/firmware/scmi"),
        { /* sentinel */ },
    };

    if ( !scmi_data.initialized )
        return false;

    /* always drop shmem */
    if ( dt_match_node(skip_matches, node) )
    {
        dt_dprintk("  Skip scmi shmem\n");
        return true;
    }

    /* drop scmi if not enabled */
    if ( dt_match_node(scmi_matches, node) && !sci_domain_is_enabled(d) )
    {
        dt_dprintk("  Skip scmi node\n");
        return true;
    }

    return false;
}

/*
 * Finalize Dom0 SCMI specific DT nodes
 *
 * if SCMI is enabled for Dom0:
 * - generate shmem node
 * - map SCMI shmem MMIO into Dom0
 */
static int scmi_dt_finalize(struct domain *d, void *fdt)
{
    __be32 reg[GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS];
    struct scmi_channel *channel;
    __be32 *cells;
    char buf[64];
    int res, rc;

    if ( !sci_domain_is_enabled(d) )
        return 0;

    channel = d->arch.sci_data;

    /*
     * All SCMI shmem nodes should be removed from Dom0 DT at this point, so
     * the shmem node for Dom0 need to be generated from SCMI channel assigned
     * to Dom0.
     * The original SCMI shmem node from platform DT is used by Xen SCMI driver
     * itself as privileged channel (agent_id=0) to manage other SCMI
     * agents (domains).
     */
    snprintf(buf, sizeof(buf), "scmi-shmem@%lx", channel->paddr);

    res = fdt_begin_node(fdt, buf);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,scmi-shmem");
    if ( res )
        return res;

    cells = &reg[0];

    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       channel->paddr, SCMI_SHMEM_MAPPED_SIZE);

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "phandle", scmi_data.shmem_phandle);
    if ( res )
        return res;

    res = fdt_end_node(fdt);
    if ( res )
        return res;

    /*
     * Map SCMI shmem into Dom0 here as shmem nodes are excluded from
     * generic Dom0 DT processing
     */
    res = iomem_permit_access(d, paddr_to_pfn(channel->paddr),
                              paddr_to_pfn(channel->paddr +
                                           SCMI_SHMEM_MAPPED_SIZE - 1));
    if ( res )
        return res;

    res = map_regions_p2mt(d, gaddr_to_gfn(channel->paddr),
                           PFN_UP(SCMI_SHMEM_MAPPED_SIZE),
                           maddr_to_mfn(channel->paddr), p2m_mmio_direct_nc);
    if ( res )
    {
        rc = iomem_deny_access(d, paddr_to_pfn(channel->paddr),
                               paddr_to_pfn(channel->paddr +
                                            SCMI_SHMEM_MAPPED_SIZE - 1));
        if ( rc )
            printk(XENLOG_ERR "scmi: Unable to deny iomem access , err = %d\n",
                   rc);
    }

    return res;
}

static __init int collect_agents(struct dt_device_node *scmi_node)
{
    const struct dt_device_node *chosen_node;
    const __be32 *prop;
    u32 len, i;

    chosen_node = dt_find_node_by_path("/chosen");
    if ( !chosen_node )
    {
        printk(XENLOG_ERR "scmi: chosen node not found\n");
        return -ENOENT;
    }

    prop = dt_get_property(chosen_node, SCMI_SECONDARY_AGENTS, &len);
    if ( !prop )
    {
        printk(XENLOG_WARNING "scmi: No %s property found\n",
               SCMI_SECONDARY_AGENTS);
        return -ENODEV;
    }

    if ( len % (3 * sizeof(u32)) )
    {
        printk(XENLOG_ERR "scmi: Invalid length of %s property: %d\n",
               SCMI_SECONDARY_AGENTS, len);
        return -EINVAL;
    }

    for ( i = 0 ; i < len / (3 * sizeof(u32)) ; i++ )
    {
        u32 agent_id = be32_to_cpu(*prop++);
        u32 smc_id = be32_to_cpu(*prop++);
        u32 shmem_phandle = be32_to_cpu(*prop++);
        struct dt_device_node *node = dt_find_node_by_phandle(shmem_phandle);
        u64 addr, size;
        int ret;

        if ( !node )
        {
            printk(XENLOG_ERR"scmi: Could not find shmem node for agent %d\n",
                   agent_id);
            return -EINVAL;
        }

        ret = dt_device_get_address(node, 0, &addr, &size);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "scmi: Could not read shmem address for agent %d: %d",
                   agent_id, ret);
            return ret;
        }

        ret = PTR_RET(smc_create_channel(agent_id, smc_id, addr));
        if ( ret )
        {
            printk(XENLOG_ERR "scmi: Could not create channel for agent %d: %d",
                   agent_id, ret);
            return ret;
        }

        printk(XENLOG_DEBUG "scmi: Agent %d SMC %X addr %lx\n", agent_id,
               smc_id, addr);
    }

    return 0;
}

static int scmi_domain_init(struct domain *d,
                            struct xen_domctl_createdomain *config)
{
    struct scmi_channel *channel;
    int ret;

    if ( !scmi_data.initialized )
        return 0;

    /*
     * Special case for Dom0 - the SCMI support is enabled basing on
     * "dom0_sci_agent_id" Xen command line parameter
     */
    if ( is_hardware_domain(d) ) {
        if ( opt_dom0_scmi_agent_id != SCMI_AGENT_ID_INVALID )
        {
            config->arch.arm_sci_type = XEN_DOMCTL_CONFIG_ARM_SCI_SCMI_SMC;
            config->arch.arm_sci_agent_id = opt_dom0_scmi_agent_id;
        } else
            config->arch.arm_sci_type = XEN_DOMCTL_CONFIG_ARM_SCI_NONE;
    }

    if ( config->arch.arm_sci_type == XEN_DOMCTL_CONFIG_ARM_SCI_NONE )
        return 0;

    channel = aquire_scmi_channel(d, config->arch.arm_sci_agent_id);
    if ( IS_ERR(channel) )
    {
        printk(XENLOG_ERR"scmi: Failed to acquire SCMI channel for agent_id %u: %ld\n",
               config->arch.arm_sci_agent_id, PTR_ERR(channel));
        return PTR_ERR(channel);
    }

    printk(XENLOG_INFO"scmi: Acquire channel id = 0x%x, domain_id = %d paddr = 0x%lx\n",
        channel->agent_id, channel->domain_id, channel->paddr);

    /*
     * Dom0 (if present) needs to have an access to the guest memory range
     * to satisfy iomem_access_permitted() check in XEN_DOMCTL_iomem_permission
     * domctl.
     */
    if ( hardware_domain && !is_hardware_domain(d) )
    {
        ret = iomem_permit_access(hardware_domain, paddr_to_pfn(channel->paddr),
                                  paddr_to_pfn(channel->paddr + PAGE_SIZE - 1));
        if ( ret )
            goto error;
    }

    d->arch.sci_data = channel;
    d->arch.sci_enabled = true;

    return 0;

error:
    relinquish_scmi_channel(channel);
    return ret;
}

int scmi_domain_sanitise_config(struct xen_domctl_createdomain *config)
{
    if ( config->arch.arm_sci_type != XEN_DOMCTL_CONFIG_ARM_SCI_NONE &&
         config->arch.arm_sci_type != XEN_DOMCTL_CONFIG_ARM_SCI_SCMI_SMC )
    {
        dprintk(XENLOG_INFO, "scmi: Unsupported ARM_SCI type\n");
        return -EINVAL;
    }
    else if ( config->arch.arm_sci_type == XEN_DOMCTL_CONFIG_ARM_SCI_SCMI_SMC &&
              config->arch.arm_sci_agent_id == 0 )
    {
        dprintk(XENLOG_INFO,
                "scmi: A zero ARM_SCI agent_id is not supported\n");
        return -EINVAL;
    }

    return 0;
}

static int scmi_relinquish_resources(struct domain *d)
{
    int ret;
    struct scmi_channel *channel, *agent_channel;
    scmi_msg_header_t hdr;
    struct scmi_msg_base_reset_agent_cfg_a2p tx;

    /* TODO: return 0 */
    return 0;

    if ( !d->arch.sci_data )
        return 0;

    agent_channel = d->arch.sci_data;

    spin_lock(&agent_channel->lock);
    tx.agent_id = agent_channel->agent_id;
    spin_unlock(&agent_channel->lock);

    channel = get_channel_by_id(HYP_CHANNEL);
    if ( !channel )
    {
        printk(XENLOG_ERR
               "scmi: Unable to get Hypervisor scmi channel for domain %d\n",
               d->domain_id);
        return -EINVAL;
    }

    hdr.id = SCMI_BASE_RESET_AGENT_CONFIGURATION;
    hdr.type = 0;
    hdr.protocol = SCMI_BASE_PROTOCOL;

    tx.flags = 0;

    ret = do_smc_xfer(channel, &hdr, &tx, sizeof(tx), NULL, 0);
    if ( ret )
        return ret;

    return ret;
}

static void scmi_domain_destroy(struct domain *d)
{
    struct scmi_channel *channel;

    if ( !d->arch.sci_data )
        return;

    channel = d->arch.sci_data;
    spin_lock(&channel->lock);

    relinquish_scmi_channel(channel);
    printk(XENLOG_DEBUG "scmi: Free domain %d\n", d->domain_id);

    d->arch.sci_data = NULL;
    d->arch.sci_enabled = true;

    spin_unlock(&channel->lock);
}

static bool scmi_handle_call(struct cpu_user_regs *regs)
{
    bool res = false;
    struct scmi_channel *agent_channel;
    struct arm_smccc_res resp;
    struct domain *d = current->domain;

    if ( !sci_domain_is_enabled(d) )
        return false;

    agent_channel = d->arch.sci_data;
    spin_lock(&agent_channel->lock);

    if ( agent_channel->func_id != regs->r0 )
    {
        res = false;
        goto unlock;
    }

    arm_smccc_smc(regs->r0, 0, 0, 0, 0, 0, 0, 0, &resp);

    set_user_reg(regs, 0, resp.a0);
    set_user_reg(regs, 1, resp.a1);
    set_user_reg(regs, 2, resp.a2);
    set_user_reg(regs, 3, resp.a3);
    res = true;
unlock:
    spin_unlock(&agent_channel->lock);

    return res;
}

static const struct sci_mediator_ops scmi_ops =
{
    .domain_init = scmi_domain_init,
    .domain_destroy = scmi_domain_destroy,
    .relinquish_resources = scmi_relinquish_resources,
    .handle_call = scmi_handle_call,
    .dom0_dt_handle_node = scmi_dt_handle_node,
    .dom0_dt_finalize = scmi_dt_finalize,
    .domain_sanitise_config = scmi_domain_sanitise_config,
};

static __init int scmi_probe(struct dt_device_node *scmi_node, const void *data)
{
    u64 addr, size;
    int ret, i;
    struct scmi_channel *channel, *agent_channel;
    int n_agents;
    scmi_msg_header_t hdr;
    struct scmi_msg_base_attributes_p2a rx;

    ASSERT(scmi_node != NULL);

    INIT_LIST_HEAD(&scmi_data.channel_list);
    spin_lock_init(&scmi_data.channel_list_lock);

    if ( !dt_property_read_u32(scmi_node, "arm,smc-id", &scmi_data.func_id) )
    {
        printk(XENLOG_ERR "scmi: Unable to read smc-id from DT\n");
        return -ENOENT;
    }

    /* save shmem phandle and re-use it fro Dom0 DT shmem node */
    if ( !dt_property_read_u32(scmi_node, "shmem", &scmi_data.shmem_phandle) )
    {
        printk(XENLOG_ERR "scmi: Unable to read shmem phandle from DT\n");
        return -ENOENT;
    }

    ret = scmi_dt_read_hyp_channel_addr(scmi_node, &addr, &size);
    if ( IS_ERR_VALUE(ret) )
        return -ENOENT;

    if ( !IS_ALIGNED(size, SCMI_SHMEM_MAPPED_SIZE) )
    {
        printk(XENLOG_ERR "scmi: Reserved memory is not aligned\n");
        return -EINVAL;
    }

    channel = smc_create_channel(HYP_CHANNEL, scmi_data.func_id, addr);
    if ( IS_ERR(channel) )
        goto out;

    ret = map_channel_memory(channel);
    if ( ret )
        goto out;

    channel->domain_id = DOMID_XEN;

    hdr.id = SCMI_BASE_PROTOCOL_ATTIBUTES;
    hdr.type = 0;
    hdr.protocol = SCMI_BASE_PROTOCOL;

    ret = do_smc_xfer(channel, &hdr, NULL, 0, &rx, sizeof(rx));
    if ( ret )
        goto error;

    n_agents = SCMI_FIELD_GET(SCMI_BASE_ATTR_NUM_AGENT, rx.attributes);
    printk(XENLOG_DEBUG "scmi: Got agent count %d\n", n_agents);

    ret = collect_agents(scmi_node);
    if ( ret )
        goto error;

    i = 1;

    list_for_each_entry(agent_channel, &scmi_data.channel_list, list)
    {
        struct scmi_msg_base_discover_agent_p2a da_rx;
        struct scmi_msg_base_discover_agent_a2p da_tx;

        ret = map_channel_memory(agent_channel);
        if ( ret )
            goto error;

        hdr.id = SCMI_BASE_DISCOVER_AGENT;
        hdr.type = 0;
        hdr.protocol = SCMI_BASE_PROTOCOL;

        da_tx.agent_id = agent_channel->agent_id;

        ret = do_smc_xfer(agent_channel, &hdr, &da_tx,
                          sizeof(da_tx), &da_rx, sizeof(da_rx));
        if ( agent_channel->domain_id != DOMID_XEN )
            unmap_channel_memory(agent_channel);
        if ( ret )
            goto error;

        printk(XENLOG_DEBUG "id=0x%x name=%s\n",
               da_rx.agent_id, da_rx.name);

        agent_channel->agent_id = da_rx.agent_id;

        if ( i > n_agents )
            break;

        i++;
    }

    ret = sci_register(&scmi_ops);
    if (ret) {
        printk(XENLOG_ERR "SCMI: mediator already registered (ret = %d)\n",
               ret);
        return ret;
    }

    scmi_data.initialized = true;
    goto out;

error:
    unmap_channel_memory(channel);
    free_channel_list();
out:
    return ret;
}

static const struct dt_device_match scmi_smc_match[] __initconst =
{
    DT_MATCH_COMPATIBLE("arm,scmi-smc"),
    { /* sentinel */ },
};

DT_DEVICE_START(gicv3, "SCMI SMC MEDIATOR", DEVICE_ARM_SCI)
        .dt_match = scmi_smc_match,
        .init = scmi_probe,
DT_DEVICE_END
