/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * Intel's gigabit Ethernet controllers of the e1000 family, as far as
 * e1000g.c uses them: register offsets in BAR 0, their bits, and the
 * legacy receive and transmit descriptors. Written from Intel's 8254x
 * and 82574 family datasheets; the names follow their register
 * mnemonics.
 */

#ifndef _DEV_PCI_E1000REG_H_
#define _DEV_PCI_E1000REG_H_

#include <stdint.h>

#define E1000_VENDOR_INTEL      0x8086
#define E1000_DEV_82540EM       0x100e          /* QEMU's "e1000" */
#define E1000_DEV_82540EM_LOM   0x1015
#define E1000_DEV_82545EM       0x100f          /* VMware's */
#define E1000_DEV_82574L        0x10d3
#define E1000_DEV_82574LA       0x10f6

/* General. */
#define E1000_CTRL              0x0000
#define  E1000_CTRL_FD          (1u << 0)       /* full duplex */
#define  E1000_CTRL_ASDE        (1u << 5)       /* speed from the link */
#define  E1000_CTRL_SLU         (1u << 6)       /* set link up */
#define  E1000_CTRL_RST         (1u << 26)      /* device reset */
#define E1000_STATUS            0x0008
#define  E1000_STATUS_FD        (1u << 0)
#define  E1000_STATUS_LU        (1u << 1)       /* link up */
#define  E1000_STATUS_SPEED(s)  (((s) >> 6) & 3)        /* 10, 100, 1000 */
#define E1000_CTRL_EXT          0x0018
#define  E1000_CTRL_EXT_PBA     (1u << 31)      /* MSI-X pending bits */

/* Interrupts. */
#define E1000_ICR               0x00c0          /* causes; write 1 to clear */
#define E1000_ICS               0x00c8
#define E1000_IMS               0x00d0          /* enable (set) */
#define E1000_IMC               0x00d8          /* disable (clear) */
#define E1000_IVAR              0x00e4          /* causes to MSI-X vectors */
#define  E1000_INT_TXDW         (1u << 0)       /* transmit written back */
#define  E1000_INT_LSC          (1u << 2)       /* link status change */
#define  E1000_INT_RXDMT0       (1u << 4)       /* receive ring running low */
#define  E1000_INT_RXO          (1u << 6)       /* receive overrun */
#define  E1000_INT_RXT0         (1u << 7)       /* receive timer */
#define  E1000_INT_RXQ0         (1u << 20)      /* MSI-X: receive queue 0 */
#define  E1000_INT_TXQ0         (1u << 22)      /* MSI-X: transmit queue 0 */
#define  E1000_INT_OTHER        (1u << 24)      /* MSI-X: other causes */
/* IVAR: a 3-bit vector and a valid bit for each cause, and write-backs. */
#define  E1000_IVAR_RXQ0(v)     ((uint32_t)((v) | 8) << 0)
#define  E1000_IVAR_TXQ0(v)     ((uint32_t)((v) | 8) << 8)
#define  E1000_IVAR_OTHER(v)    ((uint32_t)((v) | 8) << 16)
#define  E1000_IVAR_EVERYWB     (1u << 31)

/* Receive. */
#define E1000_RCTL              0x0100
#define  E1000_RCTL_EN          (1u << 1)
#define  E1000_RCTL_UPE         (1u << 3)       /* unicast promiscuous */
#define  E1000_RCTL_MPE         (1u << 4)       /* multicast promiscuous */
#define  E1000_RCTL_BAM         (1u << 15)      /* accept broadcast */
#define  E1000_RCTL_BSIZE_2048  (0u << 16)
#define  E1000_RCTL_SECRC       (1u << 26)      /* strip the CRC */
#define E1000_RDBAL             0x2800
#define E1000_RDBAH             0x2804
#define E1000_RDLEN             0x2808          /* a multiple of 128 */
#define E1000_RDH               0x2810
#define E1000_RDT               0x2818
#define E1000_RDTR              0x2820          /* receive delay, 1.024 us */
#define E1000_RADV              0x282c          /* absolute delay */

/* Transmit. */
#define E1000_TCTL              0x0400
#define  E1000_TCTL_EN          (1u << 1)
#define  E1000_TCTL_PSP         (1u << 3)       /* pad short packets */
#define  E1000_TCTL_CT(n)       ((uint32_t)(n) << 4)    /* collision threshold */
#define  E1000_TCTL_COLD(n)     ((uint32_t)(n) << 12)   /* collision distance */
#define E1000_TIPG              0x0410
#define  E1000_TIPG_DEFAULT     (8u | 8u << 10 | 6u << 20)
#define  E1000_TIPG_8254X       (10u | 8u << 10 | 6u << 20)     /* copper */
#define E1000_TDBAL             0x3800
#define E1000_TDBAH             0x3804
#define E1000_TDLEN             0x3808
#define E1000_TDH               0x3810
#define E1000_TDT               0x3818
#define E1000_TIDV              0x3820
#define E1000_TXDCTL            0x3828
#define  E1000_TXDCTL_COUNT     (1u << 22)      /* must be 1 on the 82574 */

/* Addresses. */
#define E1000_MTA               0x5200          /* multicast table, 128 */
#define E1000_NMTA              128
#define E1000_RAL0              0x5400
#define E1000_RAH0              0x5404
#define  E1000_RAH_AV           (1u << 31)      /* address valid */

/* The legacy receive descriptor. */
struct e1000_rxdesc {
        uint64_t        rd_addr;
        uint16_t        rd_len;
        uint16_t        rd_csum;
        uint8_t         rd_status;
        uint8_t         rd_errors;
        uint16_t        rd_special;
};
#define E1000_RXD_DD            0x01            /* descriptor done */
#define E1000_RXD_EOP           0x02            /* end of packet */
#define E1000_RXE_MASK          0x97            /* CE, SE, SEQ, CXE, RXE */

/* The legacy transmit descriptor. */
struct e1000_txdesc {
        uint64_t        td_addr;
        uint16_t        td_len;
        uint8_t         td_cso;
        uint8_t         td_cmd;
        uint8_t         td_status;
        uint8_t         td_css;
        uint16_t        td_special;
};
#define E1000_TXC_EOP           0x01            /* end of packet */
#define E1000_TXC_IFCS          0x02            /* insert the CRC */
#define E1000_TXC_RS            0x08            /* report status */
#define E1000_TXS_DD            0x01

#endif /* _DEV_PCI_E1000REG_H_ */
