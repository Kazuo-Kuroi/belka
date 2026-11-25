/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * NVM Express, as far as nvme.c uses it: the controller registers in
 * BAR 0, the submission and completion queue entries, and the admin and
 * NVM command set opcodes. Written from the NVM Express base
 * specification, revision 1.4; the names follow its mnemonics.
 */

#ifndef _DEV_PCI_NVMEREG_H_
#define _DEV_PCI_NVMEREG_H_

#include <stdint.h>

#define NVME_PCI_CLASS          0x01    /* mass storage */
#define NVME_PCI_SUBCLASS       0x08    /* non-volatile memory */
#define NVME_PCI_PROGIF         0x02    /* NVM Express */

/* Controller registers. */
#define NVME_CAP                0x00            /* 64 bits */
#define  NVME_CAP_MQES(c)       ((uint32_t)((c) & 0xffff) + 1)  /* entries */
#define  NVME_CAP_TO(c)         ((uint32_t)(((c) >> 24) & 0xff))  /* 500 ms */
#define  NVME_CAP_DSTRD(c)      ((uint32_t)(((c) >> 32) & 0xf))
#define  NVME_CAP_MPSMIN(c)     ((uint32_t)(((c) >> 48) & 0xf))  /* 4K << n */
#define NVME_VS                 0x08
#define NVME_INTMS              0x0c
#define NVME_INTMC              0x10
#define NVME_CC                 0x14
#define  NVME_CC_EN             (1u << 0)
#define  NVME_CC_CSS_NVM        (0u << 4)
#define  NVME_CC_MPS(n)         ((uint32_t)(n) << 7)    /* 4K << n */
#define  NVME_CC_IOSQES(n)      ((uint32_t)(n) << 16)   /* 2^n bytes */
#define  NVME_CC_IOCQES(n)      ((uint32_t)(n) << 20)
#define  NVME_CC_SHN_NORMAL     (1u << 14)      /* shut down, normally */
#define  NVME_CC_SHN_MASK       (3u << 14)
#define NVME_CSTS               0x1c
#define  NVME_CSTS_RDY          (1u << 0)
#define  NVME_CSTS_CFS          (1u << 1)       /* fatal status */
#define  NVME_CSTS_SHST_MASK    (3u << 2)
#define  NVME_CSTS_SHST_DONE    (2u << 2)       /* shutdown complete */
#define NVME_AQA                0x24            /* sizes less one */
#define NVME_ASQ                0x28            /* 64 bits */
#define NVME_ACQ                0x30            /* 64 bits */
#define NVME_DOORBELL           0x1000
/* Queue y's submission tail and completion head doorbells. */
#define NVME_SQTDBL(y, dstrd)   (NVME_DOORBELL + (2 * (y)) * (4u << (dstrd)))
#define NVME_CQHDBL(y, dstrd)   (NVME_DOORBELL + (2 * (y) + 1) * (4u << (dstrd)))

/* A submission queue entry: 64 bytes. */
struct nvme_sqe {
        uint8_t         sqe_opc;
        uint8_t         sqe_flags;      /* FUSE, PSDT: 0 (PRPs) */
        uint16_t        sqe_cid;
        uint32_t        sqe_nsid;
        uint64_t        sqe_rsvd;
        uint64_t        sqe_mptr;
        uint64_t        sqe_prp1;
        uint64_t        sqe_prp2;
        uint32_t        sqe_cdw10;
        uint32_t        sqe_cdw11;
        uint32_t        sqe_cdw12;
        uint32_t        sqe_cdw13;
        uint32_t        sqe_cdw14;
        uint32_t        sqe_cdw15;
};

/* A completion queue entry: 16 bytes. */
struct nvme_cqe {
        uint32_t        cqe_dw0;
        uint32_t        cqe_rsvd;
        uint16_t        cqe_sqhd;
        uint16_t        cqe_sqid;
        uint16_t        cqe_cid;
        uint16_t        cqe_status;     /* phase in bit 0, status above */
};
#define NVME_CQE_PHASE(s)       ((s) & 1)
#define NVME_CQE_STATUS(s)      ((s) >> 1)      /* 0: success */

/* Admin commands. */
#define NVME_ADM_CREATE_SQ      0x01
#define NVME_ADM_CREATE_CQ      0x05
#define NVME_ADM_IDENTIFY       0x06
#define NVME_ADM_SET_FEATURES   0x09
#define  NVME_FEAT_NQUEUES      0x07
#define  NVME_CNS_NAMESPACE     0x00
#define  NVME_CNS_CONTROLLER    0x01
#define  NVME_QUEUE_PC          (1u << 0)       /* physically contiguous */
#define  NVME_CQ_IEN            (1u << 1)       /* interrupts enabled */

/* NVM commands. */
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

/* Identify data, by byte offset. */
#define NVME_IDC_SN             4               /* serial, 20 bytes */
#define NVME_IDC_MN             24              /* model, 40 bytes */
#define NVME_IDC_MDTS           77              /* 2^n minimum pages */
#define NVME_IDC_RTD3E          88              /* entry latency, us, 4 bytes */
#define NVME_IDC_NN             516             /* namespaces, 4 bytes */
#define NVME_IDC_VWC            525             /* volatile write cache */
#define NVME_IDN_NSZE           0               /* blocks, 8 bytes */
#define NVME_IDN_FLBAS          26              /* the format in use */
#define NVME_IDN_LBAF           128             /* formats, 4 bytes each */
#define  NVME_LBAF_LBADS(f)     (((f) >> 16) & 0xff)    /* 2^n bytes */

#endif /* _DEV_PCI_NVMEREG_H_ */
