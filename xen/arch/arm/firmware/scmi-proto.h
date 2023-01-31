/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm System Control and Management Interface definitions
 * Version 3.0 (DEN0056C)
 *
 * Copyright (c) 2024 EPAM Systems
 */

#ifndef XEN_ARCH_ARM_SCI_SCMI_PROTO_H_
#define XEN_ARCH_ARM_SCI_SCMI_PROTO_H_

#include <xen/stdint.h>

#define SCMI_SHORT_NAME_MAX_SIZE 16

/* SCMI status codes. See section 4.1.4 */
#define SCMI_SUCCESS              0
#define SCMI_NOT_SUPPORTED      (-1)
#define SCMI_INVALID_PARAMETERS (-2)
#define SCMI_DENIED             (-3)
#define SCMI_NOT_FOUND          (-4)
#define SCMI_OUT_OF_RANGE       (-5)
#define SCMI_BUSY               (-6)
#define SCMI_COMMS_ERROR        (-7)
#define SCMI_GENERIC_ERROR      (-8)
#define SCMI_HARDWARE_ERROR     (-9)
#define SCMI_PROTOCOL_ERROR     (-10)

/* Protocol IDs */
#define SCMI_BASE_PROTOCOL 0x10

/* Base protocol message IDs */
#define SCMI_BASE_PROTOCOL_VERSION            0x0
#define SCMI_BASE_PROTOCOL_ATTIBUTES          0x1
#define SCMI_BASE_PROTOCOL_MESSAGE_ATTRIBUTES 0x2
#define SCMI_BASE_DISCOVER_AGENT              0x7
#define SCMI_BASE_SET_DEVICE_PERMISSIONS      0x9
#define SCMI_BASE_RESET_AGENT_CONFIGURATION   0xB

typedef struct scmi_msg_header {
    uint8_t id;
    uint8_t type;
    uint8_t protocol;
    uint32_t status;
} scmi_msg_header_t;

/* Table 2 Message header format */
#define SCMI_HDR_ID    GENMASK(7, 0)
#define SCMI_HDR_TYPE  GENMASK(9, 8)
#define SCMI_HDR_PROTO GENMASK(17, 10)

#define SCMI_FIELD_GET(_mask, _reg)                                            \
    ((typeof(_mask))(((_reg) & (_mask)) >> (ffs64(_mask) - 1)))
#define SCMI_FIELD_PREP(_mask, _val)                                           \
    (((typeof(_mask))(_val) << (ffs64(_mask) - 1)) & (_mask))

static inline uint32_t pack_scmi_header(scmi_msg_header_t *hdr)
{
    return SCMI_FIELD_PREP(SCMI_HDR_ID, hdr->id) |
           SCMI_FIELD_PREP(SCMI_HDR_TYPE, hdr->type) |
           SCMI_FIELD_PREP(SCMI_HDR_PROTO, hdr->protocol);
}

static inline void unpack_scmi_header(uint32_t msg_hdr, scmi_msg_header_t *hdr)
{
    hdr->id = SCMI_FIELD_GET(SCMI_HDR_ID, msg_hdr);
    hdr->type = SCMI_FIELD_GET(SCMI_HDR_TYPE, msg_hdr);
    hdr->protocol = SCMI_FIELD_GET(SCMI_HDR_PROTO, msg_hdr);
}

static inline int scmi_to_xen_errno(int scmi_status)
{
    if ( scmi_status == SCMI_SUCCESS )
        return 0;

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

/* PROTOCOL_VERSION */
#define SCMI_VERSION_MINOR GENMASK(15, 0)
#define SCMI_VERSION_MAJOR GENMASK(31, 16)

struct scmi_msg_prot_version_p2a {
    uint32_t version;
} __packed;

/* BASE PROTOCOL_ATTRIBUTES */
#define SCMI_BASE_ATTR_NUM_PROTO GENMASK(7, 0)
#define SCMI_BASE_ATTR_NUM_AGENT GENMASK(15, 8)

struct scmi_msg_base_attributes_p2a {
    uint32_t attributes;
} __packed;

/*
 * BASE_DISCOVER_AGENT
 */
#define SCMI_BASE_AGENT_ID_OWN 0xFFFFFFFF

struct scmi_msg_base_discover_agent_a2p {
    uint32_t agent_id;
} __packed;

struct scmi_msg_base_discover_agent_p2a {
    uint32_t agent_id;
    char name[SCMI_SHORT_NAME_MAX_SIZE];
} __packed;

/*
 * BASE_SET_DEVICE_PERMISSIONS
 */
#define SCMI_BASE_DEVICE_ACCESS_ALLOW           BIT(0, UL)

struct scmi_msg_base_set_device_permissions_a2p {
    uint32_t agent_id;
    uint32_t device_id;
    uint32_t flags;
} __packed;

/*
 * BASE_RESET_AGENT_CONFIGURATION
 */
#define SCMI_BASE_AGENT_PERMISSIONS_RESET       BIT(0, UL)

struct scmi_msg_base_reset_agent_cfg_a2p {
    uint32_t agent_id;
    uint32_t flags;
} __packed;

#endif /* XEN_ARCH_ARM_SCI_SCMI_PROTO_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
