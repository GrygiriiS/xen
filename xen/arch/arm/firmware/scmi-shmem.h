/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm System Control and Management Interface definitions
 * Version 3.0 (DEN0056C)
 * Shared Memory based Transport
 *
 * Copyright (c) 2024 EPAM Systems
 */

#ifndef XEN_ARCH_ARM_SCI_SCMI_SHMEM_H_
#define XEN_ARCH_ARM_SCI_SCMI_SHMEM_H_

#include <xen/stdint.h>

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

#define SCMI_SHMEM_MAPPED_SIZE PAGE_SIZE

int shmem_put_message(volatile struct scmi_shared_mem __iomem *shmem,
                      scmi_msg_header_t *hdr, void *data, int len);

int shmem_get_response(const volatile struct scmi_shared_mem __iomem *shmem,
                            scmi_msg_header_t *hdr, void *data, int len);
#endif /* XEN_ARCH_ARM_SCI_SCMI_SHMEM_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
