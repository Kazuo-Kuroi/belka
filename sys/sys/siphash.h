/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/* SipHash-2-4 (kern/subr_siphash.c): a keyed hash, 64 bits. */

#ifndef _SYS_SIPHASH_H_
#define _SYS_SIPHASH_H_

#ifdef _KERNEL

#include <stddef.h>
#include <stdint.h>

#define SIPHASH_KEYLEN  16

uint64_t siphash24(const uint8_t key[SIPHASH_KEYLEN], const void *, size_t);

#endif /* _KERNEL */

#endif /* _SYS_SIPHASH_H_ */
