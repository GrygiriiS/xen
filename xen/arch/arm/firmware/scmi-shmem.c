/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SCI SCMI multi-agent driver, using SMC/HVC shmem as transport.
 *
 * Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 * Copyright (c) 2025 EPAM Systems
 */
/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/io.h>
#include <xen/err.h>

#include "scmi-proto.h"
#include "scmi-shmem.h"

/*
 * Copy data from IO memory space to "real" memory space.
 */
static void __memcpy_fromio(void *to, const volatile void __iomem *from,
                            size_t count)
{
    while ( count && !IS_ALIGNED((unsigned long)from, 4) )
    {
        *(u8 *)to = readb_relaxed(from);
        from++;
        to++;
        count--;
    }

    while ( count >= 4 )
    {
        *(u32 *)to = readl_relaxed(from);
        from += 4;
        to += 4;
        count -= 4;
    }

    while ( count )
    {
        *(u8 *)to = readb_relaxed(from);
        from++;
        to++;
        count--;
    }
}

/*
 * Copy data from "real" memory space to IO memory space.
 */
static void __memcpy_toio(volatile void __iomem *to, const void *from,
                          size_t count)
{
    while ( count && !IS_ALIGNED((unsigned long)to, 4) )
    {
        writeb_relaxed(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }

    while ( count >= 4 )
    {
        writel_relaxed(*(u32 *)from, to);
        from += 4;
        to += 4;
        count -= 4;
    }

    while ( count )
    {
        writeb_relaxed(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }
}

static inline int
shmem_channel_is_free(const volatile struct scmi_shared_mem __iomem *shmem)
{
    return (readl(&shmem->channel_status) &
            SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE) ? 0 : -EBUSY;
}

int shmem_put_message(volatile struct scmi_shared_mem __iomem *shmem,
                      scmi_msg_header_t *hdr, void *data, int len)
{
    int ret;

    if ( (len + sizeof(shmem->msg_header)) > SCMI_SHMEM_MAPPED_SIZE )
    {
        printk(XENLOG_ERR "scmi: Wrong size of smc message. Data is invalid\n");
        return -EINVAL;
    }

    ret = shmem_channel_is_free(shmem);
    if ( ret )
        return ret;

    writel_relaxed(0x0, &shmem->channel_status);
    /* Writing 0x0 right now, but "shmem"_FLAG_INTR_ENABLED can be set */
    writel_relaxed(0x0, &shmem->flags);
    writel_relaxed(sizeof(shmem->msg_header) + len, &shmem->length);
    writel(pack_scmi_header(hdr), &shmem->msg_header);

    if ( len > 0 && data )
        __memcpy_toio(shmem->msg_payload, data, len);

    return 0;
}

int shmem_get_response(const volatile struct scmi_shared_mem __iomem *shmem,
                            scmi_msg_header_t *hdr, void *data, int len)
{
    int recv_len;
    int ret;
    int pad = sizeof(hdr->status);

    if ( len >= SCMI_SHMEM_MAPPED_SIZE - sizeof(shmem) )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of input smc message. Data may be invalid\n");
        return -EINVAL;
    }

    ret = shmem_channel_is_free(shmem);
    if ( ret )
        return ret;

    recv_len = readl(&shmem->length) - sizeof(shmem->msg_header);

    if ( recv_len < 0 )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of smc message. Data may be invalid\n");
        return -EINVAL;
    }

    unpack_scmi_header(readl(&shmem->msg_header), hdr);

    hdr->status = readl(&shmem->msg_payload);
    recv_len = recv_len > pad ? recv_len - pad : 0;

    ret = scmi_to_xen_errno(hdr->status);
    if ( ret ) {
        printk(XENLOG_DEBUG "scmi: Error received: %d\n", ret);
        return ret;
    }

    if ( recv_len > len )
    {
        printk(XENLOG_ERR
               "scmi: Not enough buffer for message %d, expecting %d\n",
               recv_len, len);
        return -EINVAL;
    }

    if ( recv_len > 0 )
        __memcpy_fromio(data, shmem->msg_payload + pad, recv_len);

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
