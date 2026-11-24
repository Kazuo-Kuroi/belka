/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * dhcpcd's link back end for Belka, in place of a BPF device: a DLPI
 * stream of its own on /dev/<interface>, bound to the Ethernet type its
 * filter is for (IPv4 for BOOTP, ARP for ARP). Belka's DLPI providers
 * give every bound stream the frames of its type for the station and
 * the broadcasts, also while IP has the device linked under it, so
 * dhcpcd sees replies before and after the interface has an address.
 *
 * DLPI gives the frame's payload with its addresses beside it; dhcpcd
 * wants the whole frame, so each read puts an Ethernet header back in
 * front, runs dhcpcd's filter program over it (a classic BPF program,
 * checked when it is set), and hands over the first frame that passes.
 * A write takes the Ethernet header off again and sends the rest as a
 * DL_UNITDATA_REQ to the header's destination.
 */

#include <sys/types.h>
#include <sys/uio.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>

#include <sys/dlpi.h>

#include "config.h"
#include "arp.h"
#include "bpf.h"
#include "common.h"
#include "dhcpcd.h"
#include "if.h"
#include "logerr.h"

#define DLADDR_LEN      (ETHER_ADDR_LEN + 2)    /* station, then SAP */
#define ETHER_HDR_LEN   ((int)sizeof(struct ether_header))
#define ETHER_MTU       1500

const char *bpf_name = "DLPI";

struct bpf_belka {
        struct bpf_insn *bb_prog;
        unsigned int    bb_len;
        uint16_t        bb_sap;
};

/* --- The filter. --- */

/* Whether prog is one bpf_run can run safely. */
static int
bpf_valid(const struct bpf_insn *prog, unsigned int len)
{
        if (len == 0 || len > BPF_MAXINSNS)
                return 0;
        for (unsigned int pc = 0; pc < len; pc++) {
                const struct bpf_insn *in = &prog[pc];
                unsigned int left = len - pc - 1;

                switch (BPF_CLASS(in->code)) {
                case BPF_LD:
                case BPF_LDX:
                        if (BPF_MODE(in->code) == BPF_MEM &&
                            in->k >= BPF_MEMWORDS)
                                return 0;
                        break;
                case BPF_ST:
                case BPF_STX:
                        if (in->k >= BPF_MEMWORDS)
                                return 0;
                        break;
                case BPF_ALU:
                        if ((BPF_OP(in->code) == BPF_DIV ||
                            BPF_OP(in->code) == BPF_MOD) &&
                            BPF_SRC(in->code) == BPF_K && in->k == 0)
                                return 0;
                        break;
                case BPF_JMP:
                        if (BPF_OP(in->code) == BPF_JA) {
                                if (in->k > left)
                                        return 0;
                        } else if (in->jt > left || in->jf > left) {
                                return 0;
                        }
                        break;
                default:
                        break;
                }
        }
        return BPF_CLASS(prog[len - 1].code) == BPF_RET;
}

static int
bpf_load(const uint8_t *p, size_t plen, uint32_t off, int size, uint32_t *v)
{
        if (off > plen || (size_t)size > plen - off)
                return 0;
        switch (size) {
        case 4:
                *v = (uint32_t)p[off] << 24 | (uint32_t)p[off + 1] << 16 |
                    (uint32_t)p[off + 2] << 8 | p[off + 3];
                break;
        case 2:
                *v = (uint32_t)p[off] << 8 | p[off + 1];
                break;
        default:
                *v = p[off];
                break;
        }
        return 1;
}

/* How much of the frame the program keeps: 0, not wanted. */
static uint32_t
bpf_run(const struct bpf_insn *prog, const uint8_t *p, size_t plen)
{
        uint32_t a = 0, x = 0, mem[BPF_MEMWORDS] = { 0 };
        unsigned int pc = 0;

        for (;; pc++) {
                const struct bpf_insn *in = &prog[pc];
                uint32_t src, v;
                int size;

                switch (BPF_CLASS(in->code)) {
                case BPF_RET:
                        return BPF_RVAL(in->code) == BPF_A ? a : in->k;
                case BPF_LD:
                case BPF_LDX:
                        size = BPF_SIZE(in->code) == BPF_W ? 4 :
                            BPF_SIZE(in->code) == BPF_H ? 2 : 1;
                        switch (BPF_MODE(in->code)) {
                        case BPF_IMM:
                                v = in->k;
                                break;
                        case BPF_LEN:
                                v = (uint32_t)plen;
                                break;
                        case BPF_MEM:
                                v = mem[in->k];
                                break;
                        case BPF_ABS:
                                if (!bpf_load(p, plen, in->k, size, &v))
                                        return 0;
                                break;
                        case BPF_IND:
                                if (in->k > UINT32_MAX - x ||
                                    !bpf_load(p, plen, x + in->k, size, &v))
                                        return 0;
                                break;
                        case BPF_MSH:
                                if (!bpf_load(p, plen, in->k, 1, &v))
                                        return 0;
                                v = (v & 0x0f) << 2;
                                break;
                        default:
                                return 0;
                        }
                        if (BPF_CLASS(in->code) == BPF_LD)
                                a = v;
                        else
                                x = v;
                        break;
                case BPF_ST:
                        mem[in->k] = a;
                        break;
                case BPF_STX:
                        mem[in->k] = x;
                        break;
                case BPF_ALU:
                        src = BPF_SRC(in->code) == BPF_X ? x : in->k;
                        switch (BPF_OP(in->code)) {
                        case BPF_ADD: a += src; break;
                        case BPF_SUB: a -= src; break;
                        case BPF_MUL: a *= src; break;
                        case BPF_OR: a |= src; break;
                        case BPF_AND: a &= src; break;
                        case BPF_XOR: a ^= src; break;
                        case BPF_LSH: a = src < 32 ? a << src : 0; break;
                        case BPF_RSH: a = src < 32 ? a >> src : 0; break;
                        case BPF_NEG: a = -a; break;
                        case BPF_DIV:
                        case BPF_MOD:
                                if (src == 0)
                                        return 0;
                                a = BPF_OP(in->code) == BPF_DIV ? a / src :
                                    a % src;
                                break;
                        default:
                                return 0;
                        }
                        break;
                case BPF_JMP:
                        src = BPF_SRC(in->code) == BPF_X ? x : in->k;
                        switch (BPF_OP(in->code)) {
                        case BPF_JA:
                                pc += in->k;
                                continue;
                        case BPF_JEQ: v = a == src; break;
                        case BPF_JGT: v = a > src; break;
                        case BPF_JGE: v = a >= src; break;
                        case BPF_JSET: v = (a & src) != 0; break;
                        default:
                                return 0;
                        }
                        pc += v ? in->jt : in->jf;
                        break;
                case BPF_MISC:
                        if (BPF_MISCOP(in->code) == BPF_TAX)
                                x = a;
                        else
                                a = x;
                        break;
                default:
                        return 0;
                }
        }
}

/* --- The stream. --- */

static int
dl_bind(int fd, uint16_t sap)
{
        dl_bind_req_t req;
        union {
                t_uscalar_t prim;
                dl_bind_ack_t ack;
                dl_error_ack_t err;
                char buf[64];
        } ans;
        struct strbuf c = { 0, (int)sizeof(req), (char *)&req };
        int flags = 0;

        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_BIND_REQ;
        req.dl_sap = sap;
        req.dl_service_mode = DL_CLDLS;
        c.len = (int)sizeof(req);
        if (putmsg(fd, &c, NULL, 0) == -1)
                return -1;
        c.maxlen = (int)sizeof(ans);
        c.len = 0;
        c.buf = ans.buf;
        if (getmsg(fd, &c, NULL, &flags) == -1)
                return -1;
        if (c.len < (int)sizeof(t_uscalar_t) || ans.prim != DL_BIND_ACK) {
                errno = (c.len >= (int)sizeof(ans.err) &&
                    ans.prim == DL_ERROR_ACK && ans.err.dl_unix_errno != 0) ?
                    (int)ans.err.dl_unix_errno : EPROTO;
                return -1;
        }
        return 0;
}

struct bpf *
bpf_open(const struct interface *ifp,
    int (*filter)(const struct bpf *, const struct in_addr *),
    const struct in_addr *ia)
{
        struct bpf_belka *bb;
        struct bpf *bpf;
        char path[sizeof("/dev/") + IF_NAMESIZE];
        int mtu;

        if ((bpf = calloc(1, sizeof(*bpf))) == NULL)
                return NULL;
        bpf->bpf_fd = -1;
        bpf->bpf_ifp = ifp;
        bpf->bpf_flags = BPF_EOF;
        if ((bb = calloc(1, sizeof(*bb))) == NULL)
                goto fail;
        bpf->bpf_handle = bb;
        bb->bb_sap = filter == bpf_filter_arp ? ETHERTYPE_ARP : ETHERTYPE_IP;

        snprintf(path, sizeof(path), "/dev/%s", ifp->name);
        if ((bpf->bpf_fd = open(path, O_RDWR | O_CLOEXEC)) == -1)
                goto fail;
        if (dl_bind(bpf->bpf_fd, bb->bb_sap) == -1)
                goto fail;
        if (fcntl(bpf->bpf_fd, F_SETFL, O_NONBLOCK) == -1)
                goto fail;
        if (filter(bpf, ia) != 0)
                goto fail;

        mtu = ifp->mtu > 0 ? ifp->mtu : ETHER_MTU;
        bpf->bpf_size = (size_t)ETHER_HDR_LEN + (size_t)mtu;
        if ((bpf->bpf_buffer = malloc(bpf->bpf_size)) == NULL)
                goto fail;
        return bpf;

fail:
        bpf_close(bpf);
        return NULL;
}

/*
 * The next frame that passes the filter, its Ethernet header put back,
 * into data; 0 when none waits (BPF_EOF is then set, as it is after
 * each frame: the descriptor polls readable again while more wait).
 */
ssize_t
bpf_read(struct bpf *bpf, void *data, size_t len)
{
        struct bpf_belka *bb = bpf->bpf_handle;
        uint8_t *frame = bpf->bpf_buffer;

        bpf->bpf_flags &= ~BPF_EOF;
        for (;;) {
                union {
                        t_uscalar_t prim;
                        dl_unitdata_ind_t ind;
                        char buf[sizeof(dl_unitdata_ind_t) + 2 * 32];
                } ctl;
                struct strbuf c = { (int)sizeof(ctl), 0, ctl.buf };
                struct strbuf d = { (int)(bpf->bpf_size - (size_t)ETHER_HDR_LEN), 0,
                    (char *)frame + ETHER_HDR_LEN };
                int flags = 0, more;
                uint32_t keep;
                size_t flen;

                if ((more = getmsg(bpf->bpf_fd, &c, &d, &flags)) == -1) {
                        if (errno == EAGAIN || errno == EINTR) {
                                bpf->bpf_flags |= BPF_EOF;
                                return 0;
                        }
                        return -1;
                }
                if (more != 0) {
                        /* Larger than the MTU: drop the rest of it. */
                        char junk[256];

                        do {
                                c.maxlen = (more & MORECTL) ?
                                    (int)sizeof(junk) : -1;
                                c.buf = junk;
                                d.maxlen = (more & MOREDATA) ?
                                    (int)sizeof(junk) : -1;
                                d.buf = junk;
                                flags = 0;
                                more = getmsg(bpf->bpf_fd, &c, &d, &flags);
                        } while (more > 0);
                        continue;
                }
                if (c.len < (int)sizeof(ctl.ind) ||
                    ctl.prim != DL_UNITDATA_IND || d.len < 0 ||
                    ctl.ind.dl_dest_addr_length < ETHER_ADDR_LEN ||
                    ctl.ind.dl_src_addr_length < ETHER_ADDR_LEN ||
                    ctl.ind.dl_dest_addr_offset > (t_uscalar_t)c.len -
                    ETHER_ADDR_LEN ||
                    ctl.ind.dl_src_addr_offset > (t_uscalar_t)c.len -
                    ETHER_ADDR_LEN)
                        continue;

                memcpy(frame, ctl.buf + ctl.ind.dl_dest_addr_offset,
                    ETHER_ADDR_LEN);
                memcpy(frame + ETHER_ADDR_LEN,
                    ctl.buf + ctl.ind.dl_src_addr_offset, ETHER_ADDR_LEN);
                frame[12] = (uint8_t)(bb->bb_sap >> 8);
                frame[13] = (uint8_t)bb->bb_sap;
                flen = (size_t)ETHER_HDR_LEN + (size_t)d.len;

                if (bb->bb_prog != NULL &&
                    (keep = bpf_run(bb->bb_prog, frame, flen)) == 0)
                        continue;
                if (bb->bb_prog != NULL && keep < flen)
                        flen = keep;
                if (flen > len)
                        flen = len;
                if (bpf_frame_bcast(bpf->bpf_ifp, frame) == 0)
                        bpf->bpf_flags |= BPF_BCAST;
                else
                        bpf->bpf_flags &= ~BPF_BCAST;
                memcpy(data, frame, flen);
                bpf->bpf_flags |= BPF_EOF;
                return (ssize_t)flen;
        }
}

int
bpf_setfilter(const struct bpf *bpf, void *filter, unsigned int filter_len)
{
        struct bpf_belka *bb = bpf->bpf_handle;
        struct bpf_insn *prog;

        if (!bpf_valid(filter, filter_len)) {
                errno = EINVAL;
                return -1;
        }
        if ((prog = reallocarray(NULL, filter_len, sizeof(*prog))) == NULL)
                return -1;
        memcpy(prog, filter, filter_len * sizeof(*prog));
        free(bb->bb_prog);
        bb->bb_prog = prog;
        bb->bb_len = filter_len;
        return 0;
}

/* Writes are bound by the stream's type already; nothing to lock. */
int
bpf_setwfilter(const struct bpf *bpf, void *filter, unsigned int filter_len)
{
        UNUSED(bpf);
        UNUSED(filter);
        UNUSED(filter_len);
        errno = ENOSYS;
        return -1;
}

int
bpf_lockfilter(const struct bpf *bpf)
{
        UNUSED(bpf);
        errno = ENOSYS;
        return -1;
}

ssize_t
bpf_writev(const struct bpf *bpf, struct iovec *iov, int iovcnt)
{
        struct bpf_belka *bb = bpf->bpf_handle;
        uint8_t *frame = bpf->bpf_buffer;
        union {
                dl_unitdata_req_t req;
                char buf[sizeof(dl_unitdata_req_t) + DLADDR_LEN];
        } ctl;
        struct strbuf c, d;
        size_t len = 0;
        uint16_t sap;

        for (int i = 0; i < iovcnt; i++) {
                if (iov[i].iov_len > bpf->bpf_size - len) {
                        errno = EMSGSIZE;
                        return -1;
                }
                memcpy(frame + len, iov[i].iov_base, iov[i].iov_len);
                len += iov[i].iov_len;
        }
        if (len < (size_t)ETHER_HDR_LEN) {
                errno = EINVAL;
                return -1;
        }
        sap = (uint16_t)(frame[12] << 8 | frame[13]);
        if (sap != bb->bb_sap) {
                errno = EINVAL;
                return -1;
        }

        memset(&ctl, 0, sizeof(ctl));
        ctl.req.dl_primitive = DL_UNITDATA_REQ;
        ctl.req.dl_dest_addr_length = DLADDR_LEN;
        ctl.req.dl_dest_addr_offset = sizeof(ctl.req);
        memcpy(ctl.buf + sizeof(ctl.req), frame, ETHER_ADDR_LEN);
        memcpy(ctl.buf + sizeof(ctl.req) + ETHER_ADDR_LEN, &sap,
            sizeof(sap));       /* the SAP in host order */
        c.maxlen = 0;
        c.len = (int)sizeof(ctl);
        c.buf = ctl.buf;
        d.maxlen = 0;
        d.len = (int)(len - (size_t)ETHER_HDR_LEN);
        d.buf = (char *)frame + ETHER_HDR_LEN;
        if (putmsg(bpf->bpf_fd, &c, &d, 0) == -1)
                return -1;
        return (ssize_t)len;
}

void
bpf_close(struct bpf *bpf)
{
        struct bpf_belka *bb;

        if (bpf == NULL)
                return;
        if ((bb = bpf->bpf_handle) != NULL) {
                free(bb->bb_prog);
                free(bb);
        }
        if (bpf->bpf_fd != -1)
                close(bpf->bpf_fd);
        free(bpf->bpf_buffer);
        free(bpf);
}
