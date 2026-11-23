/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * T_OPTMGMT_REQ for the transports: the request's options taken apart,
 * each handed to the transport's own function, and the T_OPTMGMT_ACK
 * built from what it says. See <sys/tihdr.h> for the format.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/stream.h>
#include <sys/systm.h>
#include <sys/tihdr.h>

#include <netinet/ip_var.h>

#define TPI_OPTALIGN(n) (((n) + 3) & ~(size_t)3)

static void
tpi_opterr(queue_t *q, mblk_t *mp, t_scalar_t tli, t_scalar_t unixerr)
{
        struct T_error_ack ea = { T_ERROR_ACK, T_OPTMGMT_REQ, tli, unixerr };

        freemsg(mp);
        if ((mp = allocb(sizeof(ea), BPRI_HI)) == NULL)
                return;
        mp->b_datap->db_type = M_PCPROTO;
        memcpy(mp->b_wptr, &ea, sizeof(ea));
        mp->b_wptr += sizeof(ea);
        qreply(q, mp);
}

/* How bad a status is: the acknowledgement's flags are the worst. */
static int
tpi_optrank(t_uscalar_t status)
{
        switch (status) {
        case T_SUCCESS:
                return 0;
        case T_PARTSUCCESS:
                return 1;
        case T_READONLY:
                return 2;
        case T_FAILURE:
                return 3;
        default:
                return 4;               /* T_NOTSUPPORT */
        }
}

void
tpi_optmgmt(queue_t *q, mblk_t *mp, tpi_optfn fn, void *arg)
{
        struct T_optmgmt_req req;
        struct T_optmgmt_ack ack;
        struct t_opthdr oh;
        const uint8_t *opts;
        size_t off, count = 0, olen;
        t_uscalar_t worst = T_SUCCESS;
        uint8_t *wp;
        mblk_t *ap;

        if (!pullupmsg(mp, -1) || MBLKL(mp) < sizeof(req)) {
                tpi_opterr(q, mp, TSYSERR, EINVAL);
                return;
        }
        memcpy(&req, mp->b_rptr, sizeof(req));
        if (req.MGMT_flags != T_NEGOTIATE && req.MGMT_flags != T_CURRENT) {
                tpi_opterr(q, mp, TBADFLAG, 0);
                return;
        }
        if (req.OPT_length < 0 || req.OPT_offset < 0 ||
            (size_t)req.OPT_offset > MBLKL(mp) ||
            (size_t)req.OPT_length > MBLKL(mp) - (size_t)req.OPT_offset) {
                tpi_opterr(q, mp, TBADOPT, 0);
                return;
        }
        opts = mp->b_rptr + req.OPT_offset;
        olen = (size_t)req.OPT_length;

        /* Each header whole and in bounds, and how many there are. */
        for (off = 0; off < olen; off += TPI_OPTALIGN(oh.len)) {
                if (olen - off < sizeof(oh)) {
                        tpi_opterr(q, mp, TBADOPT, 0);
                        return;
                }
                memcpy(&oh, opts + off, sizeof(oh));
                if (oh.len < sizeof(oh) || oh.len > olen - off) {
                        tpi_opterr(q, mp, TBADOPT, 0);
                        return;
                }
                count++;
        }
        if (count == 0) {
                tpi_opterr(q, mp, TBADOPT, 0);
                return;
        }
        if ((ap = allocb(sizeof(ack) + count * (sizeof(oh) + TPI_OPTVAL),
            BPRI_HI)) == NULL) {
                tpi_opterr(q, mp, TSYSERR, ENOSR);
                return;
        }
        ap->b_datap->db_type = M_PCPROTO;
        wp = ap->b_wptr + sizeof(ack);
        for (off = 0; off < olen; off += TPI_OPTALIGN(oh.len)) {
                uint8_t out[TPI_OPTVAL];
                size_t outlen = 0;

                memcpy(&oh, opts + off, sizeof(oh));
                memset(out, 0, sizeof(out));
                oh.status = (t_uscalar_t)fn(arg, req.MGMT_flags,
                    (int)oh.level, (int)oh.name, opts + off + sizeof(oh),
                    oh.len - sizeof(oh), out, &outlen);
                if (outlen > TPI_OPTVAL)
                        outlen = TPI_OPTVAL;
                if (tpi_optrank(oh.status) > tpi_optrank(worst))
                        worst = oh.status;
                oh.len = (t_uscalar_t)(sizeof(oh) + outlen);
                memcpy(wp, &oh, sizeof(oh));
                memset(wp + sizeof(oh), 0, TPI_OPTALIGN(outlen));
                memcpy(wp + sizeof(oh), out, outlen);
                wp += sizeof(oh) + TPI_OPTALIGN(outlen);
        }
        memset(&ack, 0, sizeof(ack));
        ack.PRIM_type = T_OPTMGMT_ACK;
        ack.OPT_offset = sizeof(ack);
        ack.OPT_length = (t_scalar_t)(wp - ap->b_wptr - sizeof(ack));
        ack.MGMT_flags = (t_scalar_t)worst;
        memcpy(ap->b_wptr, &ack, sizeof(ack));
        ap->b_wptr = wp;
        freemsg(mp);
        qreply(q, ap);
}

/* An int option's value: the first int of len bytes; -1 if too short. */
int
tpi_optint(const void *val, size_t len, int *v)
{
        if (len < sizeof(*v))
                return -1;
        memcpy(v, val, sizeof(*v));
        return 0;
}

/* An int option's value, out. */
void
tpi_optout(int v, void *out, size_t *outlen)
{
        memcpy(out, &v, sizeof(v));
        *outlen = sizeof(v);
}
