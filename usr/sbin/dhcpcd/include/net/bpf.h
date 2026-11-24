/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * The classic Berkeley packet filter's instructions, in their usual
 * encoding, for dhcpcd's filters. Belka has no BPF device: dhcpcd's
 * link back end (usr/sbin/dhcpcd/bpf-belka.c) runs the programs itself
 * over the frames its DLPI stream receives.
 */

#ifndef _DHCPCD_NET_BPF_H_
#define _DHCPCD_NET_BPF_H_

#include <stdint.h>

struct bpf_insn {
        uint16_t        code;
        uint8_t         jt;
        uint8_t         jf;
        uint32_t        k;
};

struct bpf_program {
        unsigned int    bf_len;
        struct bpf_insn *bf_insns;
};

#define BPF_CLASS(c)    ((c) & 0x07)
#define BPF_LD          0x00
#define BPF_LDX         0x01
#define BPF_ST          0x02
#define BPF_STX         0x03
#define BPF_ALU         0x04
#define BPF_JMP         0x05
#define BPF_RET         0x06
#define BPF_MISC        0x07

#define BPF_SIZE(c)     ((c) & 0x18)
#define BPF_W           0x00
#define BPF_H           0x08
#define BPF_B           0x10

#define BPF_MODE(c)     ((c) & 0xe0)
#define BPF_IMM         0x00
#define BPF_ABS         0x20
#define BPF_IND         0x40
#define BPF_MEM         0x60
#define BPF_LEN         0x80
#define BPF_MSH         0xa0

#define BPF_OP(c)       ((c) & 0xf0)
#define BPF_ADD         0x00
#define BPF_SUB         0x10
#define BPF_MUL         0x20
#define BPF_DIV         0x30
#define BPF_OR          0x40
#define BPF_AND         0x50
#define BPF_LSH         0x60
#define BPF_RSH         0x70
#define BPF_NEG         0x80
#define BPF_MOD         0x90
#define BPF_XOR         0xa0

#define BPF_JA          0x00
#define BPF_JEQ         0x10
#define BPF_JGT         0x20
#define BPF_JGE         0x30
#define BPF_JSET        0x40

#define BPF_SRC(c)      ((c) & 0x08)
#define BPF_K           0x00
#define BPF_X           0x08

#define BPF_RVAL(c)     ((c) & 0x18)
#define BPF_A           0x10

#define BPF_MISCOP(c)   ((c) & 0xf8)
#define BPF_TAX         0x00
#define BPF_TXA         0x80

#define BPF_MEMWORDS    16
#define BPF_MAXINSNS    512

#define BPF_STMT(c, v)          { (uint16_t)(c), 0, 0, (uint32_t)(v) }
#define BPF_JUMP(c, v, t, f)    { (uint16_t)(c), (t), (f), (uint32_t)(v) }

#endif /* _DHCPCD_NET_BPF_H_ */
