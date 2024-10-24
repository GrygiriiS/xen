/*
 * xen/drivers/passthrough/arm/plat-rcar/rproc.c
 *
 * Renesas R-Car Gen4-specific RPROC pass-through code
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <xen/types.h>
#include <xen/vmap.h>

#include <asm/io.h>
#include <asm/platform.h>
#include <asm/platforms/rcar.h>
#include <asm/regs.h>
#include <asm/smccc.h>

#include <public/arch-arm.h>

#define MFIS_MAX_CHANNELS 8

#define MFIS_IICR(k) (0x1400 + 0x1008 * k + 0x20 * 0)
#define MFIS_EICR(i) (0x9404 + 0x1020 * 0 + 0x8 * i)

#define MFIS_SMC_TRIG  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,         \
                                          ARM_SMCCC_CONV_32,           \
                                          ARM_SMCCC_OWNER_SIP,         \
                                          0x100)
#define MFIS_SMC_ERR_BUSY               0x01
#define MFIS_SMC_ERR_NOT_AVAILABLE      0x02

#define RPMSG_MAX_VQS MFIS_MAX_CHANNELS

#define RPMSG_SMC_GET_VDEV_INFO  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                    ARM_SMCCC_CONV_32,  \
                                                    ARM_SMCCC_OWNER_SIP, \
                                                    0x200)
#define RPMSG_SMC_GET_VRING_INFO  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                     ARM_SMCCC_CONV_32, \
                                                     ARM_SMCCC_OWNER_SIP, \
                                                     0x201)
#define RPMSG_SMC_SET_VRING_DATA  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                     ARM_SMCCC_CONV_32, \
                                                     ARM_SMCCC_OWNER_SIP, \
                                                     0x202)

#define RPROC_SMC_ERR_NOT_AVAILABLE      0x01

/* Driver has used its parts of the config, and is happy */
#define VIRTIO_CONFIG_S_DRIVER_OK   4

struct mfis_data
{
    void *base;
    uint8_t chan_cnt;
    int irqs[MFIS_MAX_CHANNELS];
    struct domain* domains[MFIS_MAX_CHANNELS];
};

struct resource_table {
    u32 ver;
    u32 num;
    u32 reserved[2];
    u32 offset[0];
} __packed;

struct fw_rsc_hdr {
    u32 type;
	u8 data[0];
} __packed;

enum fw_resource_type {
	RSC_CARVEOUT	= 0,
	RSC_DEVMEM	= 1,
	RSC_TRACE	= 2,
	RSC_VDEV	= 3,
	RSC_LAST	= 4,
};

struct fw_rsc_vdev_vring {
	u32 da;
	u32 align;
	u32 num;
	u32 notifyid;
	u32 pa;
} __packed;

struct fw_rsc_vdev {
	u32 id;
	u32 notifyid;
	u32 dfeatures;
	u32 gfeatures;
	u32 config_len;
	u8 status;
	u8 num_of_vrings;
	u8 reserved[2];
	struct fw_rsc_vdev_vring vring[0];
} __packed;

struct rproc_data
{
    void *base;
    uint8_t vq_cnt;
    int irqs[RPMSG_MAX_VQS];
    struct {
        struct domain* d;
        struct page_info *vring_pg[2];
        struct fw_rsc_vdev *vdev;
    } channels[RPMSG_MAX_VQS];
};

static struct mfis_data *mfis_data;
static struct rproc_data *rproc_data;

static bool rproc_handle_smc(struct cpu_user_regs *regs);
static int rproc_assign_domain(struct domain *d, int chan);
static int rproc_remove_domain(struct domain *d);

static struct rproc_ops rcar_rproc_ops = {
    .handle_smc = rproc_handle_smc,
    .assign_domain = rproc_assign_domain,
    .remove_domain = rproc_remove_domain,
};

/* TODO: Move to own file */
struct rproc_ops *rproc_ops;

static void mfis_irq_handler(int irq, void *dev_id, struct cpu_user_regs *regs)
{
    int i;

    ASSERT(dev_id == mfis_data);

    /* Find domain for irq */
    for ( i = 0; i < mfis_data->chan_cnt; i++)
        /* TODO: in general case i != chan */
        if ( mfis_data->irqs[i] == irq )
        {

            uint32_t val;

            val = readl(mfis_data->base + MFIS_EICR(i));

            if ( !(val & 0x1 ) )
            {
                printk(XENLOG_WARNING"MFIS: Spurious IRQ for chan %d\n", i);
                break;
            }

            writel(val & ~0x1, mfis_data->base + MFIS_EICR(i));

	    if ( mfis_data->domains[i] )
                vgic_inject_irq(mfis_data->domains[i], NULL, GUEST_MFIS_SPI, true);
            else
                printk(XENLOG_WARNING"MFIS: IRQ for chan %d without domain\n", i);

            break;
        }
}

static int mfis_add_domain(struct domain* d, int chan)
{
    int ret;

    if ( chan >= mfis_data->chan_cnt )
        return -EINVAL;

    ret = vgic_reserve_virq(d, GUEST_MFIS_SPI);
    if ( ret < 0)
        return ret;

    mfis_data->domains[chan] = d;

    printk("MFIS: Added chan %d for domain %d\n", chan, d->domain_id);

    return 0;
}

static int mfis_remove_domain(int chan)
{

    if ( chan >= mfis_data->chan_cnt )
        return -EINVAL;

    mfis_data->domains[chan] = NULL;

    return 0;
}

static int mfis_trigger_chan(struct domain *d)
{
    int i;
    uint32_t val;

    /* Find chan for domain */
    for ( i = 0; i < mfis_data->chan_cnt; i++)
        if ( mfis_data->domains[i] == d )
        {
            val = readl(mfis_data->base + MFIS_IICR(i));

            if ( val & 0x1 )
                return -EBUSY;

            writel(1, mfis_data->base + MFIS_IICR(i));
            return 0;
        }

    return -ENOENT;
}

static int mfis_init(struct dt_device_node *node, const void *data)
{
    paddr_t start, len;
    int ret, i;
    u32 prop_len;
    const __be32 *prop_val;

    mfis_data = xzalloc(struct mfis_data);
    if (!mfis_data)
        return -ENOMEM;

    ret = dt_device_get_address(node, 0, &start, &len);
    if ( ret )
    {
        printk(XENLOG_ERR"Cannot read MFIS base address\n");
        goto err;
    }

    mfis_data->base = ioremap_nocache(start, len);
    if ( !mfis_data->base )
    {
        printk(XENLOG_ERR"Unable to map MFIS region!\n");
        goto err;
    }

    prop_val = dt_get_property(node, "renesas,mfis-channels", &prop_len);
    if ( !prop_val || prop_len < sizeof(uint32_t) )
        goto err;

    mfis_data->chan_cnt = prop_len / sizeof(uint32_t);

    if ( mfis_data->chan_cnt > MFIS_MAX_CHANNELS )
        mfis_data->chan_cnt = MFIS_MAX_CHANNELS;

    printk(XENLOG_INFO"MFIS: Found %d channels\n", mfis_data->chan_cnt);

    for ( i = 0; i < mfis_data->chan_cnt; i++)
    {
        uint32_t chan_id = be32_to_cpu(prop_val[i]);
        int irq = platform_get_irq(node, chan_id);

        if ( chan_id != i )
        {
            printk(XENLOG_ERR"MFIS: TODO: Setup where i != chan_id is not supported yet\n");
            goto err_free_irq;
        }
        if ( irq <= 0 )
        {
            printk(XENLOG_ERR "MFIS: Can't get irq for chan %d\n", chan_id);
            goto err_free_irq;
        }

        printk(XENLOG_INFO "MFIS: chan %d irq %d\n", chan_id, irq);

        if ( request_irq(irq, 0, mfis_irq_handler, "rcar-mfis",
                         mfis_data ) < 0 )
        {
            printk(XENLOG_ERR "MFIS: Can't request irq %d for chan %d\n", irq, chan_id);
            goto err_free_irq;
        }

        mfis_data->irqs[i] = irq;
    }

    dt_device_set_used_by(node, DOMID_XEN);

    /* if rproc already initialized */
    if ( rproc_data )
    {
        rproc_ops = &rcar_rproc_ops;
        printk(XENLOG_INFO"MFIS: rproc ops installed\n");
    }
    return 0;

err_free_irq:
    for ( i = 0; i < mfis_data->chan_cnt; i++)
        if ( mfis_data->irqs[i] )
            release_irq(mfis_data->irqs[i], mfis_data);

err:
    iounmap(mfis_data->base);
    xfree(mfis_data);
    mfis_data = NULL;
    return -ENODEV;
}


static const struct dt_device_match mfis_dt_match[] __initconst =
{
    {
        .compatible = "renesas,mfis",
    },
    { /* sentinel */ },
};

DT_DEVICE_START(rcar_mfis, "Renesas MFIS for RPROC", DEVICE_MISC)
    .dt_match = mfis_dt_match,
    .init = mfis_init,
DT_DEVICE_END

static int rproc_handle_vdev(struct fw_rsc_vdev *vdev)
{
    int n = rproc_data->vq_cnt;

    rproc_data->channels[n].vdev = vdev;
    rproc_data->vq_cnt = n + 1;

    return 0;
}

static int rproc_parse_rtable(void)
{

    struct resource_table *rtable;
    struct fw_rsc_hdr *hdr;
    struct fw_rsc_vdev *vdev;
    int entry;
    int ret;

    ASSERT(rproc_data && rproc_data->base);

    rtable = rproc_data->base;

    if ( rtable->ver != 1 )
    {
        printk(XENLOG_ERR"rproc: unknown resource table version %d\n", rtable->ver);
        return -EINVAL;
    }

    printk(XENLOG_INFO"rproc: found %d entries\n", rtable->num);

    for ( entry = 0; entry < rtable->num; entry++ )
    {
        hdr = rproc_data->base + rtable->offset[entry];
        switch (hdr->type)
        {
        case RSC_CARVEOUT:
            /* TODO: Handle carveout */
            break;
        case RSC_TRACE:
            break;
        case RSC_DEVMEM:
            /* TODO: Hadnle devmem */
            break;
        case RSC_VDEV:
            vdev = (void*)hdr->data;
            ret = rproc_handle_vdev(vdev);
            if ( ret )
                return ret;

            break;
        default:
            printk(XENLOG_INFO"rproc: found unknown entry %d. Skipping\n", hdr->type); ////
            break;
        }
    }

    return 0;
}

static int __init rproc_init(struct dt_device_node *node, const void *data)
{
    paddr_t start, len;
    int ret;
    mfn_t rtable_mfn;

    rproc_data = xzalloc(struct rproc_data);
    if ( !rproc_data )
        return -ENOMEM;

    ret = dt_device_get_address(node, 0, &start, &len);
    if ( ret )
    {
        printk(XENLOG_ERR"rproc: Cannot read rproc resource table addr\n");
        goto err;
    }

    if ( len > PAGE_SIZE )
    {
        /* TODO: Support bigger tables */
        printk(XENLOG_ERR"rproc: resource table does not fit into page\n");
        goto err;
    }

    rtable_mfn = maddr_to_mfn(start);
    rproc_data->base = vmap(&rtable_mfn, 1);
    if ( !rproc_data->base )
    {
        printk(XENLOG_ERR"Unable to map rproc resource table!\n");
        goto err;
    }

    ret = rproc_parse_rtable();
    if ( ret )
        goto err;

    dt_device_set_used_by(node, DOMID_XEN);

    printk(XENLOG_INFO"Successfully initialized renesas,rproc\n");

    /* if mfis already initialized */
    if ( mfis_data )
    {
        rproc_ops = &rcar_rproc_ops;
        printk(XENLOG_INFO"rproc: rproc ops installed\n");
    }

    return 0;

err:
    vunmap(rproc_data->base);
    xfree(rproc_data);
    rproc_data = NULL;

    return -ENODEV;
}

static int rproc_assign_domain(struct domain *d, int chan)
{
    int ret;

    if ( !rproc_data )
    {
        printk(XENLOG_WARNING"rproc is not initialized\n");
        return -ENODEV;
    }
    if ( !mfis_data )
    {
        printk(XENLOG_WARNING"mfis is not initialized\n");
        return -ENODEV;
    }

    if ( chan >= rproc_data->vq_cnt )
        return -EINVAL;
    ret = mfis_add_domain(d, chan);

    if ( ret )
        return ret;
    rproc_data->channels[chan].d = d;

    return 0;
}


static int rproc_remove_domain(struct domain *d)
{
    int i;

    if ( !rproc_data )
        return 0;

    if ( !mfis_data )
        return 0;

    for ( i = 0; i < mfis_data->chan_cnt; i++)
    {
        if ( mfis_data->domains[i] == d )
        {
            mfis_remove_domain(i);
            break;
        }
    }

    return 0;
}

static int rproc_find_chan(struct domain *d)
{
    int i;

    for ( i = 0; i < rproc_data->vq_cnt; i++)
        if ( rproc_data->channels[i].d == d )
            return i;

    return -ENOENT;
}

static int rproc_handle_get_vdev_info(struct domain *d,
                                      struct cpu_user_regs *regs)
{
    int ch = rproc_find_chan(d);

    if ( ch < 0 )
    {
        set_user_reg(regs, 0, RPROC_SMC_ERR_NOT_AVAILABLE);
        return ch;
    }

    set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
    set_user_reg(regs, 1, rproc_data->channels[ch].vdev->id);
    set_user_reg(regs, 2, rproc_data->channels[ch].vdev->dfeatures);

    return 0;
}

static int rproc_handle_get_vring_info(struct domain *d,
                                       struct cpu_user_regs *regs)
{
    int ch = rproc_find_chan(d);
    uint32_t ring = (uint32_t)get_user_reg(regs, 1);


    if ( ch < 0 || ring > 1 )
    {
        set_user_reg(regs, 0, RPROC_SMC_ERR_NOT_AVAILABLE);
        return ch;
    }

    set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
    set_user_reg(regs, 1, rproc_data->channels[ch].vdev->vring[ring].align);
    set_user_reg(regs, 2, rproc_data->channels[ch].vdev->vring[ring].num);
    set_user_reg(regs, 3, rproc_data->channels[ch].vdev->vring[ring].notifyid);

    return 0;
}

static int rproc_handle_set_vring_data(struct domain *d,
                                       struct cpu_user_regs *regs)
{
    int ch = rproc_find_chan(d);
    uint32_t ring = (uint32_t)get_user_reg(regs, 1);
    paddr_t pa, ga;
    uint32_t notify_id;
    struct page_info *pg;
    p2m_type_t t;

    if ( ch < 0 || ring > 1 )
    {
        set_user_reg(regs, 0, RPROC_SMC_ERR_NOT_AVAILABLE);
        return ch;
    }

    ga = get_user_reg(regs, 2);
    notify_id = (uint32_t)get_user_reg(regs, 3);

    pg = get_page_from_gfn(d, paddr_to_pfn(ga) , &t, P2M_ALLOC);
    /* HACK: Dirty hack for Dom0 direct mapped dma area */
    if ( t == p2m_mmio_direct_c )
    {
        pa = ga;
        goto got_pa;
    }
    if ( !pg || t != p2m_ram_rw )
    {
        if ( pg )
            goto put_pg;

        goto err;
    }

    pa = page_to_maddr(pg);
got_pa:
    printk("remoteproc: pa = %lx\n", pa);
    if ( pa & 0xFFFFFFFF00000000UL )
    {
        printk(XENLOG_ERR"rproc: provided page is above 4GB\n");
        goto put_pg;
    }

    rproc_data->channels[ch].vdev->vring[ring].notifyid = notify_id;
    rproc_data->channels[ch].vdev->vring[ring].da = pa;
    rproc_data->channels[ch].vring_pg[ring] = pg;

    /* HACK: TODO: Add separate operation RPMSG_SMC_SET_VDEV_READY */
    if (ring == 0)
        rproc_data->channels[ch].vdev->status |= VIRTIO_CONFIG_S_DRIVER_OK;

    /* TODO: Verify if we really need this */
    clean_and_invalidate_dcache_va_range(rproc_data->base, PAGE_SIZE);

    set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);

    return 0;

put_pg:
    put_page(pg);

err:
    set_user_reg(regs, 0, RPROC_SMC_ERR_NOT_AVAILABLE);

    return -EINVAL;
}

static bool rproc_handle_smc(struct cpu_user_regs *regs)
{
    if ( !mfis_data || !rproc_data )
    {
        printk(XENLOG_G_DEBUG"rproc: not initialized\n");
        return false;
    }

    switch ( get_user_reg(regs, 0) )
    {
    case MFIS_SMC_TRIG:
    {
        int ret;

        ret = mfis_trigger_chan(current->domain);
        if ( ret == 0 )
            set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
        else if ( ret == -EBUSY )
            set_user_reg(regs, 0, MFIS_SMC_ERR_BUSY);
        else if ( ret == -EINVAL )
            set_user_reg(regs, 0, MFIS_SMC_ERR_NOT_AVAILABLE);
        else
            set_user_reg(regs, 0, ARM_SMCCC_ERR_UNKNOWN_FUNCTION);
        return true;
    }
    case RPMSG_SMC_GET_VDEV_INFO:
        rproc_handle_get_vdev_info(current->domain, regs);
        return true;
    case RPMSG_SMC_GET_VRING_INFO:
        rproc_handle_get_vring_info(current->domain, regs);
        return true;
    case RPMSG_SMC_SET_VRING_DATA:
        rproc_handle_set_vring_data(current->domain, regs);
        return true;
    default:
        return false;
    }
}

static const struct dt_device_match rproc_dt_match[] __initconst =
{
    {
        .compatible = "renesas,rproc",
    },
    { /* sentinel */ },
};

DT_DEVICE_START(rcar_rproc, "Renesas RPROC", DEVICE_MISC)
    .dt_match = rproc_dt_match,
    .init = rproc_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */