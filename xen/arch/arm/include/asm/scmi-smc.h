/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xen/arch/arm/include/asm/scmi-smc.h
 *
 * ARM System Control and Management Interface (SCMI) over SMC
 * Generic handling layer
 *
 * Andrei Cherechesu <andrei.cherechesu@nxp.com>
 * Copyright 2024 NXP
 */

#ifndef __ASM_SCMI_SMC_H__
#define __ASM_SCMI_SMC_H__

#include <xen/types.h>
#include <asm/regs.h>

#ifdef CONFIG_SCMI_SMC_DOM0

bool scmi_is_enabled(void);
bool scmi_is_valid_smc_id(uint32_t fid);
bool scmi_handle_smc(struct cpu_user_regs *regs);

#else

static inline bool scmi_is_enabled(void)
{
    return false;
}

static inline bool scmi_is_valid_smc_id(uint32_t fid)
{
    return false;
}

static inline bool scmi_handle_smc(struct cpu_user_regs *regs)
{
    return false;
}

#endif /* CONFIG_SCMI_SMC */

#endif /* __ASM_SCMI_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
