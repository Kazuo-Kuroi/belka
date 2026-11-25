/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * AHCI (Serial ATA host controllers) and the ATA commands ahci.c sends:
 * the HBA's registers in BAR 5, each port's, the command header and
 * table, and the register FIS. Written from the AHCI specification,
 * revision 1.3.1, and the ATA command set; the names follow their
 * mnemonics.
 */

#ifndef _DEV_PCI_AHCIREG_H_
#define _DEV_PCI_AHCIREG_H_

#include <stdint.h>

#define AHCI_PCI_CLASS          0x01    /* mass storage */
#define AHCI_PCI_SUBCLASS       0x06    /* serial ATA */
#define AHCI_PCI_PROGIF         0x01    /* AHCI */
#define AHCI_BAR                5       /* ABAR */

/* The HBA. */
#define AHCI_CAP                0x00
#define  AHCI_CAP_NP(c)         (((c) & 0x1f) + 1)      /* ports */
#define  AHCI_CAP_NCS(c)        ((((c) >> 8) & 0x1f) + 1)       /* slots */
#define  AHCI_CAP_S64A          (1u << 31)
#define AHCI_GHC                0x04
#define  AHCI_GHC_HR            (1u << 0)       /* HBA reset */
#define  AHCI_GHC_IE            (1u << 1)       /* interrupts */
#define  AHCI_GHC_AE            (1u << 31)      /* AHCI enable */
#define AHCI_IS                 0x08            /* a bit per port */
#define AHCI_PI                 0x0c            /* ports implemented */
#define AHCI_VS                 0x10
#define AHCI_NPORTS             32

/* A port's registers, at AHCI_PORT(p) + reg. */
#define AHCI_PORT(p)            (0x100 + 0x80 * (p))
#define AHCI_PxCLB              0x00            /* command list, 1 KB */
#define AHCI_PxCLBU             0x04
#define AHCI_PxFB               0x08            /* received FISes, 256 */
#define AHCI_PxFBU              0x0c
#define AHCI_PxIS               0x10
#define AHCI_PxIE               0x14
#define  AHCI_PxI_DHRS          (1u << 0)       /* D2H register FIS */
#define  AHCI_PxI_PSS           (1u << 1)       /* PIO setup FIS */
#define  AHCI_PxI_DSS           (1u << 2)       /* DMA setup FIS */
#define  AHCI_PxI_SDBS          (1u << 3)       /* set device bits FIS */
#define  AHCI_PxI_DPS           (1u << 5)       /* a PRD done */
#define  AHCI_PxI_IFS           (1u << 27)      /* interface fatal */
#define  AHCI_PxI_HBDS          (1u << 28)      /* host bus data error */
#define  AHCI_PxI_HBFS          (1u << 29)      /* host bus fatal */
#define  AHCI_PxI_TFES          (1u << 30)      /* task file error */
#define  AHCI_PxI_ERRORS        (AHCI_PxI_IFS | AHCI_PxI_HBDS | \
                                    AHCI_PxI_HBFS | AHCI_PxI_TFES)
#define AHCI_PxCMD              0x18
#define  AHCI_PxCMD_ST          (1u << 0)       /* start */
#define  AHCI_PxCMD_SUD         (1u << 1)       /* spin up */
#define  AHCI_PxCMD_POD         (1u << 2)       /* power on */
#define  AHCI_PxCMD_FRE         (1u << 4)       /* FIS receive enable */
#define  AHCI_PxCMD_FR          (1u << 14)      /* FIS receive running */
#define  AHCI_PxCMD_CR          (1u << 15)      /* command list running */
#define AHCI_PxTFD              0x20
#define  AHCI_TFD_ERR           0x01
#define  AHCI_TFD_DRQ           0x08
#define  AHCI_TFD_BSY           0x80
#define AHCI_PxSIG              0x24
#define  AHCI_SIG_ATA           0x00000101
#define  AHCI_SIG_ATAPI         0xeb140101
#define AHCI_PxSSTS             0x28
#define  AHCI_SSTS_DET(s)       ((s) & 0xf)     /* 3: device and link */
#define AHCI_PxSCTL             0x2c
#define  AHCI_SCTL_DET_INIT     0x1             /* COMRESET */
#define AHCI_PxSERR             0x30
#define AHCI_PxCI               0x38            /* commands issued */

/* A command header: 32 bytes, 32 to a list. */
struct ahci_cmdhdr {
        uint16_t        ch_flags;       /* CFL, A, W, P, R, B, C, PMP */
        uint16_t        ch_prdtl;       /* PRD entries */
        uint32_t        ch_prdbc;       /* bytes transferred */
        uint32_t        ch_ctba;        /* the command table, 128-aligned */
        uint32_t        ch_ctbau;
        uint32_t        ch_rsvd[4];
};
#define AHCI_CH_CFL(dw)         ((uint16_t)(dw))        /* FIS dwords */
#define AHCI_CH_W               (1u << 6)               /* to the device */

/* A physical region descriptor: 16 bytes. */
struct ahci_prd {
        uint32_t        prd_dba;        /* even */
        uint32_t        prd_dbau;
        uint32_t        prd_rsvd;
        uint32_t        prd_dbc;        /* bytes less one; bit 31 I */
};

/* A command table: the command FIS, then the PRDs from 0x80. */
#define AHCI_CT_CFIS            0x00
#define AHCI_CT_PRDT            0x80

/* The register FIS, host to device. */
#define FIS_TYPE_REG_H2D        0x27
#define FIS_H2D_C               0x80            /* a command */
#define FIS_H2D_LEN             20              /* bytes */

/* ATA commands. */
#define ATA_READ_DMA_EXT        0x25
#define ATA_WRITE_DMA_EXT       0x35
#define ATA_FLUSH_CACHE_EXT     0xea
#define ATA_IDENTIFY            0xec
#define ATA_DEV_LBA             0x40

/* IDENTIFY DEVICE data, by 16-bit word. */
#define ATA_ID_MODEL            27              /* 20 words, swapped */
#define ATA_ID_LBA28            60              /* 2 words */
#define ATA_ID_CMDSET2          83
#define  ATA_CMDSET2_LBA48      (1u << 10)
#define ATA_ID_LBA48            100             /* 4 words */
#define ATA_ID_SECSIZE          106
#define  ATA_SECSIZE_VALID(w)   (((w) & 0xc000) == 0x4000)
#define  ATA_SECSIZE_LARGE      (1u << 12)      /* words 117-118 */
#define ATA_ID_LSECSIZE         117             /* words per sector */

#endif /* _DEV_PCI_AHCIREG_H_ */
