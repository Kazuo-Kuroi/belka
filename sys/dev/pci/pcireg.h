/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * PCI configuration space registers, from the PCI Local Bus and PCI
 * Express specifications.
 */

#ifndef _DEV_PCI_PCIREG_H_
#define _DEV_PCI_PCIREG_H_

#define PCI_ID_REG              0x00    /* vendor (low 16), device (high) */
#define PCI_COMMAND_STATUS_REG  0x04
#define PCI_CLASS_REG           0x08    /* revision, prog-if, subclass, class */
#define PCI_BHLC_REG            0x0c    /* header type in bits 16-23 */
#define PCI_BAR0                0x10
#define PCI_NBARS               6
#define PCI_SUBSYS_ID_REG       0x2c
#define PCI_CAPLIST_PTR_REG     0x34
#define PCI_INTERRUPT_REG       0x3c    /* line (low 8), pin (next 8) */
#define PCI_BRIDGE_BUS_REG      0x18    /* primary, secondary, subordinate */

#define PCI_VENDOR_INVALID      0xffff

#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEM         0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400
#define PCI_STATUS_CAPLIST      0x00100000      /* in the 32-bit register */

#define PCI_HDR_TYPE_MASK       0x7f
#define PCI_HDR_MULTIFN         0x80
#define PCI_HDR_DEVICE          0x00
#define PCI_HDR_BRIDGE          0x01

#define PCI_CLASS_BRIDGE        0x06
#define PCI_SUBCLASS_BRIDGE_HOST 0x00
#define PCI_SUBCLASS_BRIDGE_PCI 0x04

#define PCI_BAR_IO              0x00000001
#define PCI_BAR_MEM_TYPE_MASK   0x00000006
#define PCI_BAR_MEM_64          0x00000004
#define PCI_BAR_MEM_PREFETCH    0x00000008
#define PCI_BAR_IO_MASK         0xfffffffc
#define PCI_BAR_MEM_MASK        0xfffffff0

#define PCI_CAP_MSI             0x05
#define PCI_CAP_VENDOR          0x09
#define PCI_CAP_PCIE            0x10
#define PCI_CAP_MSIX            0x11

/* MSI-X capability. */
#define PCI_MSIX_CTL            0x02    /* 16 bits */
#define PCI_MSIX_CTL_TBLSIZE    0x07ff  /* table size minus one */
#define PCI_MSIX_CTL_FUNCMASK   0x4000
#define PCI_MSIX_CTL_ENABLE     0x8000
#define PCI_MSIX_TABLE          0x04    /* offset | BAR index */
#define PCI_MSIX_PBA            0x08
#define PCI_MSIX_BIR_MASK       0x00000007

/* One MSI-X table entry, 16 bytes. */
#define PCI_MSIX_ENTRY_SIZE     16
#define PCI_MSIX_ENTRY_ADDR_LO  0x0
#define PCI_MSIX_ENTRY_ADDR_HI  0x4
#define PCI_MSIX_ENTRY_DATA     0x8
#define PCI_MSIX_ENTRY_VCTL     0xc
#define PCI_MSIX_VCTL_MASK      0x00000001

/* The MSI capability. */
#define PCI_MSI_CTL             0x02    /* 16 bits */
#define PCI_MSI_CTL_ENABLE      0x0001
#define PCI_MSI_CTL_MME_MASK    0x0070  /* vectors enabled: one */
#define PCI_MSI_CTL_64BIT       0x0080
#define PCI_MSI_ADDR_LO         0x04
#define PCI_MSI_ADDR_HI         0x08    /* if 64-bit */
#define PCI_MSI_DATA_32         0x08
#define PCI_MSI_DATA_64         0x0c

#endif /* _DEV_PCI_PCIREG_H_ */
