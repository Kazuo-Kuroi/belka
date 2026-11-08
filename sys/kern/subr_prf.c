/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel formatted output and panic.
 *
 * The formatter supports the C99 conversions the kernel needs: d, i, u,
 * o, x, X, c, s, p, and %, with the flags '-', '0', and '#', a field width
 * (which may be '*'), a precision for strings, and the length modifiers
 * hh, h, l, ll, j, z, and t. Floating point is not supported.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/cons.h>
#include <sys/systm.h>
#include <sys/uadmin.h>

#include <machine/cpufunc.h>

/* Where formatted characters go. */
struct prf_sink {
        void    (*ps_put)(struct prf_sink *, int);
        char    *ps_buf;        /* snprintf: next byte to write */
        size_t  ps_left;        /* snprintf: room left, excluding NUL */
};

enum prf_length {
        LEN_INT,
        LEN_CHAR,
        LEN_SHORT,
        LEN_LONG,
        LEN_LLONG,
        LEN_INTMAX,
        LEN_SIZE,
        LEN_PTRDIFF
};

#define PRF_LEFT        0x01    /* '-' flag */
#define PRF_ZERO        0x02    /* '0' flag */
#define PRF_UPPER       0x04    /* upper-case hexadecimal */
#define PRF_ALT         0x08    /* '#' flag */

static void prf_cons_put(struct prf_sink *, int);
static void prf_buf_put(struct prf_sink *, int);
static int prf_number(struct prf_sink *, uintmax_t, int, const char *,
    int, int);
static int kvprintf(struct prf_sink *, const char *, va_list);

static int panicking;

static void
prf_cons_put(struct prf_sink *ps __unused, int c)
{
        cnputc(c);
}

static void
prf_buf_put(struct prf_sink *ps, int c)
{
        if (ps->ps_left > 0) {
                *ps->ps_buf++ = (char)c;
                ps->ps_left--;
        }
}

/*
 * Emit an unsigned magnitude in the given base, preceded by prefix (a
 * sign or "0x", possibly empty) and padded to width. Zero padding goes
 * between the prefix and the digits. Returns the number of characters
 * emitted.
 */
static int
prf_number(struct prf_sink *ps, uintmax_t value, int base,
    const char *prefix, int width, int flags)
{
        static const char lower[] = "0123456789abcdef";
        static const char upper[] = "0123456789ABCDEF";
        const char *digits = (flags & PRF_UPPER) ? upper : lower;
        char buf[sizeof(uintmax_t) * 3 + 1];
        int plen = (int)strlen(prefix);
        int len = 0;
        int count = 0;
        int pad;

        do {
                buf[len++] = digits[value % (unsigned)base];
                value /= (unsigned)base;
        } while (value != 0);

        pad = width - len - plen;

        if ((flags & PRF_LEFT) == 0 && (flags & PRF_ZERO) == 0) {
                for (; pad > 0; pad--, count++)
                        ps->ps_put(ps, ' ');
        }
        for (; *prefix != '\0'; prefix++, count++)
                ps->ps_put(ps, *prefix);
        if ((flags & PRF_LEFT) == 0 && (flags & PRF_ZERO) != 0) {
                for (; pad > 0; pad--, count++)
                        ps->ps_put(ps, '0');
        }
        while (len > 0) {
                ps->ps_put(ps, buf[--len]);
                count++;
        }
        for (; pad > 0; pad--, count++)
                ps->ps_put(ps, ' ');

        return count;
}

static int
kvprintf(struct prf_sink *ps, const char *fmt, va_list ap)
{
        int count = 0;

        for (; *fmt != '\0'; fmt++) {
                enum prf_length length = LEN_INT;
                int flags = 0;
                int width = 0;
                int precision = -1;
                uintmax_t uval;
                intmax_t sval;
                const char *prefix;
                const char *s;
                int base;
                int len;

                if (*fmt != '%') {
                        ps->ps_put(ps, *fmt);
                        count++;
                        continue;
                }
                fmt++;

                for (;; fmt++) {
                        if (*fmt == '-')
                                flags |= PRF_LEFT;
                        else if (*fmt == '#')
                                flags |= PRF_ALT;
                        else if (*fmt == '0')
                                flags |= PRF_ZERO;
                        else
                                break;
                }

                if (*fmt == '*') {
                        width = va_arg(ap, int);
                        if (width < 0) {
                                flags |= PRF_LEFT;
                                width = -width;
                        }
                        fmt++;
                } else {
                        while (*fmt >= '0' && *fmt <= '9')
                                width = width * 10 + (*fmt++ - '0');
                }

                if (*fmt == '.') {
                        fmt++;
                        precision = 0;
                        if (*fmt == '*') {
                                precision = va_arg(ap, int);
                                fmt++;
                        } else {
                                while (*fmt >= '0' && *fmt <= '9')
                                        precision = precision * 10 +
                                            (*fmt++ - '0');
                        }
                }

                switch (*fmt) {
                case 'h':
                        if (fmt[1] == 'h') {
                                length = LEN_CHAR;
                                fmt++;
                        } else {
                                length = LEN_SHORT;
                        }
                        fmt++;
                        break;
                case 'l':
                        if (fmt[1] == 'l') {
                                length = LEN_LLONG;
                                fmt++;
                        } else {
                                length = LEN_LONG;
                        }
                        fmt++;
                        break;
                case 'j':
                        length = LEN_INTMAX;
                        fmt++;
                        break;
                case 'z':
                        length = LEN_SIZE;
                        fmt++;
                        break;
                case 't':
                        length = LEN_PTRDIFF;
                        fmt++;
                        break;
                default:
                        break;
                }

                switch (*fmt) {
                case 'd':
                case 'i':
                        switch (length) {
                        case LEN_LONG:
                                sval = va_arg(ap, long);
                                break;
                        case LEN_LLONG:
                                sval = va_arg(ap, long long);
                                break;
                        case LEN_INTMAX:
                                sval = va_arg(ap, intmax_t);
                                break;
                        case LEN_SIZE:
                        case LEN_PTRDIFF:
                                sval = va_arg(ap, ptrdiff_t);
                                break;
                        case LEN_CHAR:
                                sval = (signed char)va_arg(ap, int);
                                break;
                        case LEN_SHORT:
                                sval = (short)va_arg(ap, int);
                                break;
                        default:
                                sval = va_arg(ap, int);
                                break;
                        }
                        if (sval < 0)
                                uval = (uintmax_t)0 - (uintmax_t)sval;
                        else
                                uval = (uintmax_t)sval;
                        count += prf_number(ps, uval, 10,
                            sval < 0 ? "-" : "", width, flags);
                        break;
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                        switch (length) {
                        case LEN_LONG:
                                uval = va_arg(ap, unsigned long);
                                break;
                        case LEN_LLONG:
                                uval = va_arg(ap, unsigned long long);
                                break;
                        case LEN_INTMAX:
                                uval = va_arg(ap, uintmax_t);
                                break;
                        case LEN_SIZE:
                        case LEN_PTRDIFF:
                                uval = va_arg(ap, size_t);
                                break;
                        case LEN_CHAR:
                                uval = (unsigned char)va_arg(ap, unsigned);
                                break;
                        case LEN_SHORT:
                                uval = (unsigned short)va_arg(ap, unsigned);
                                break;
                        default:
                                uval = va_arg(ap, unsigned);
                                break;
                        }
                        if (*fmt == 'u')
                                base = 10;
                        else if (*fmt == 'o')
                                base = 8;
                        else
                                base = 16;
                        if (*fmt == 'X')
                                flags |= PRF_UPPER;
                        prefix = "";
                        if ((flags & PRF_ALT) != 0 && uval != 0) {
                                if (base == 16)
                                        prefix = (*fmt == 'X') ? "0X" : "0x";
                                else if (base == 8)
                                        prefix = "0";
                        }
                        count += prf_number(ps, uval, base, prefix, width,
                            flags);
                        break;
                case 'p':
                        uval = (uintptr_t)va_arg(ap, void *);
                        count += prf_number(ps, uval, 16, "0x",
                            (int)sizeof(void *) * 2 + 2, PRF_ZERO);
                        break;
                case 'c':
                        ps->ps_put(ps, va_arg(ap, int));
                        count++;
                        break;
                case 's':
                        if ((s = va_arg(ap, const char *)) == NULL)
                                s = "(null)";
                        for (len = 0; s[len] != '\0'; len++) {
                                if (precision >= 0 && len >= precision)
                                        break;
                        }
                        if ((flags & PRF_LEFT) == 0) {
                                for (; width > len; width--, count++)
                                        ps->ps_put(ps, ' ');
                        }
                        count += len;
                        for (int i = 0; i < len; i++)
                                ps->ps_put(ps, s[i]);
                        if ((flags & PRF_LEFT) != 0) {
                                for (; width > len; width--, count++)
                                        ps->ps_put(ps, ' ');
                        }
                        break;
                case '%':
                        ps->ps_put(ps, '%');
                        count++;
                        break;
                case '\0':
                        /* A lone '%' at the end of the format. */
                        return count;
                default:
                        /* Unknown conversion: print it literally. */
                        ps->ps_put(ps, '%');
                        ps->ps_put(ps, *fmt);
                        count += 2;
                        break;
                }
        }

        return count;
}

int
vprintf(const char *fmt, va_list ap)
{
        struct prf_sink ps;

        ps.ps_put = prf_cons_put;
        ps.ps_buf = NULL;
        ps.ps_left = 0;
        return kvprintf(&ps, fmt, ap);
}

int
printf(const char *fmt, ...)
{
        va_list ap;
        int count;

        va_start(ap, fmt);
        count = vprintf(fmt, ap);
        va_end(ap);
        return count;
}

/*
 * Format into buf, which holds size bytes. The result is always NUL
 * terminated when size is nonzero. Returns the length the full result
 * would have had, as C99 vsnprintf() does.
 */
int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
        struct prf_sink ps;
        int count;

        ps.ps_put = prf_buf_put;
        ps.ps_buf = buf;
        ps.ps_left = (size > 0) ? size - 1 : 0;
        count = kvprintf(&ps, fmt, ap);
        if (size > 0)
                *ps.ps_buf = '\0';
        return count;
}

int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
        va_list ap;
        int count;

        va_start(ap, fmt);
        count = vsnprintf(buf, size, fmt, ap);
        va_end(ap);
        return count;
}

/*
 * Report a fatal error and stop. A panic while panicking stops at once
 * without formatting, in case the formatter itself is at fault.
 */
void
panic(const char *fmt, ...)
{
        va_list ap;

        disable_intr();
        if (panicking++ == 0) {
                printf("\npanic: ");
                va_start(ap, fmt);
                vprintf(fmt, ap);
                va_end(ap);
                printf("\n");
        }
        mdboot(AD_HALT, 1);
}
