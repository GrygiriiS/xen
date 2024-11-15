/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xen/arch/arm/scmi-smc.c
 *
 * ARM System Control and Management Interface (SCMI) over SMC
 * Generic handling layer
 *
 * Andrei Cherechesu <andrei.cherechesu@nxp.com>
 * Copyright 2024 NXP
 */

#include <asm/device.h>
#include <xen/acpi.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/sci/sci.h>
#include <asm/smccc.h>

static uint32_t scmi_smc_id;
static struct domain *hw_dom;

/*
 * Generic handler for SCMI-SMC requests, currently only forwarding the
 * request to FW running at EL3 if it came from Dom0. Is called from the vSMC
 * layer for SiP SMCs, since SCMI calls are usually provided this way.
 * Can also be called from `platform_smc()` plat-specific callback.
 *
 * Returns true if SMC was handled (regardless of response), false otherwise.
 */
static bool scmi_handle_smc(struct cpu_user_regs *regs)
{
    uint32_t smc_id = get_user_reg(regs, 0);
    struct arm_smccc_res res;

    /* Only the hardware domain should use SCMI calls */
    if ( hw_dom != current->domain || smc_id != scmi_smc_id )
        return false;

    /* For the moment, forward the SCMI Request to FW running at EL3 */
    arm_smccc_1_1_smc(smc_id,
                      get_user_reg(regs, 1),
                      get_user_reg(regs, 2),
                      get_user_reg(regs, 3),
                      get_user_reg(regs, 4),
                      get_user_reg(regs, 5),
                      get_user_reg(regs, 6),
                      get_user_reg(regs, 7),
                      &res);

    set_user_reg(regs, 0, res.a0);
    set_user_reg(regs, 1, res.a1);
    set_user_reg(regs, 2, res.a2);
    set_user_reg(regs, 3, res.a3);

    return true;
}

static int scmi_dom0_domain_init(struct domain *d)
{
/*
    if ( !is_hardware_domain(d) )
        return 0;
*/

    if (d->domain_id != 1)
        return 0;

    if (hw_dom)
        return -EEXIST;

    hw_dom = d;
    printk(XENLOG_INFO "SCMI: %pd init\n", d);
    return 0;
}

static void scmi_dom0_domain_destroy(struct domain *d)
{
    if ( hw_dom != d )
        return;

    hw_dom = NULL;
    printk(XENLOG_INFO "SCMI: %pd destroy\n", d);
}

static int __init scmi_check_smccc_ver(void)
{
    if ( smccc_ver < ARM_SMCCC_VERSION_1_1 )
    {
        printk(XENLOG_ERR
               "SCMI: No SMCCC 1.1 support, SCMI calls forwarding disabled\n");
        return -ENOSYS;
    }

    return 0;
}

static const struct sci_mediator_ops scmi_dom0_ops = {
        .handle_call = scmi_handle_smc,
        .domain_init = scmi_dom0_domain_init,
        .domain_destroy = scmi_dom0_domain_destroy,
};

/* Initialize the SCMI layer based on SMCs and Device-tree */
static int __init scmi_dom0_init(struct dt_device_node *dev, const void *data)
{
    int ret;

    ret = scmi_check_smccc_ver();
    if ( ret )
        goto err;

    ret = dt_property_read_u32(dev, "arm,smc-id", &scmi_smc_id);
    if ( !ret )
    {
        printk(XENLOG_ERR "SCMI: No valid \"%s\" property in \"%s\" DT node\n",
                "arm,smc-id", dt_node_full_name(dev));
        return -ENOENT;
    }

    ret = sci_register(&scmi_dom0_ops);
    if (ret) {
        printk(XENLOG_ERR "SCMI: mediator already registered (ret = %d)\n",
               ret);
        return ret;
    }

    printk(XENLOG_INFO "Using SCMI with SMC ID: 0x%x\n", scmi_smc_id);
    return 0;

err:
    printk(XENLOG_ERR "SCMI: Initialization failed (ret = %d)\n", ret);
    return ret;
}

static const struct dt_device_match scmi_smc_match[] __initconst =
{
    DT_MATCH_COMPATIBLE("arm,scmi-smc"),
    { /* sentinel */ },
};

DT_DEVICE_START(gicv3, "SCMI SMC DOM0", DEVICE_SCI)
        .dt_match = scmi_smc_match,
        .init = scmi_dom0_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
