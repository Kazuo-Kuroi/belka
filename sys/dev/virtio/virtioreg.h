/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio registers and ring layouts, from the OASIS Virtual I/O Device
 * specification, version 1.x. All fields are little-endian, which is
 * the host order on amd64.
 */

#ifndef _DEV_VIRTIO_VIRTIOREG_H_
#define _DEV_VIRTIO_VIRTIOREG_H_

#include <stdint.h>

/* Device status. */
#define VIRTIO_STATUS_ACK               0x01
#define VIRTIO_STATUS_DRIVER            0x02
#define VIRTIO_STATUS_DRIVER_OK         0x04
#define VIRTIO_STATUS_FEATURES_OK       0x08
#define VIRTIO_STATUS_FAILED            0x80

/* Transport-independent feature bits. */
#define VIRTIO_F_VERSION_1              32

/* Device types. */
#define VIRTIO_ID_NET                   1
#define VIRTIO_ID_BLOCK                 2
#define VIRTIO_ID_CONSOLE               3

/* PCI vendor capability types (cfg_type at capability offset 3). */
#define VIRTIO_PCI_CAP_COMMON_CFG       1
#define VIRTIO_PCI_CAP_NOTIFY_CFG       2
#define VIRTIO_PCI_CAP_ISR_CFG          3
#define VIRTIO_PCI_CAP_DEVICE_CFG       4

/* Offsets within a virtio PCI capability. */
#define VIRTIO_PCI_CAP_TYPE             3
#define VIRTIO_PCI_CAP_BAR              4
#define VIRTIO_PCI_CAP_OFFSET           8
#define VIRTIO_PCI_CAP_LENGTH           12
#define VIRTIO_PCI_NOTIFY_MULT          16

/* The common configuration structure. */
#define VIRTIO_COMMON_DFSELECT          0       /* 32 bits */
#define VIRTIO_COMMON_DF                4
#define VIRTIO_COMMON_GFSELECT          8
#define VIRTIO_COMMON_GF                12
#define VIRTIO_COMMON_MSIX              16      /* 16 bits */
#define VIRTIO_COMMON_NUMQ              18
#define VIRTIO_COMMON_STATUS            20      /* 8 bits */
#define VIRTIO_COMMON_CFGGEN            21
#define VIRTIO_COMMON_Q_SELECT          22      /* 16 bits */
#define VIRTIO_COMMON_Q_SIZE            24
#define VIRTIO_COMMON_Q_MSIX            26
#define VIRTIO_COMMON_Q_ENABLE          28
#define VIRTIO_COMMON_Q_NOFF            30
#define VIRTIO_COMMON_Q_DESC            32      /* 64 bits */
#define VIRTIO_COMMON_Q_AVAIL           40
#define VIRTIO_COMMON_Q_USED            48

#define VIRTIO_MSI_NO_VECTOR            0xffff

/* Split virtqueue. */
#define VRING_DESC_F_NEXT               1
#define VRING_DESC_F_WRITE              2

struct vring_desc {
        uint64_t        addr;
        uint32_t        len;
        uint16_t        flags;
        uint16_t        next;
};

struct vring_avail {
        uint16_t        flags;
        uint16_t        idx;
        uint16_t        ring[];
};

struct vring_used_elem {
        uint32_t        id;
        uint32_t        len;
};

struct vring_used {
        uint16_t        flags;
        uint16_t        idx;
        struct vring_used_elem ring[];
};

/* Block device. */
#define VIRTIO_BLK_F_RO                 5
#define VIRTIO_BLK_F_BLK_SIZE           6
#define VIRTIO_BLK_F_FLUSH              9

#define VIRTIO_BLK_CFG_CAPACITY         0       /* 64 bits, in 512 bytes */
#define VIRTIO_BLK_CFG_BLK_SIZE         20      /* 32 bits */

#define VIRTIO_BLK_T_IN                 0
#define VIRTIO_BLK_T_OUT                1
#define VIRTIO_BLK_T_FLUSH              4

#define VIRTIO_BLK_S_OK                 0
#define VIRTIO_BLK_S_IOERR              1
#define VIRTIO_BLK_S_UNSUPP             2

#endif /* _DEV_VIRTIO_VIRTIOREG_H_ */
