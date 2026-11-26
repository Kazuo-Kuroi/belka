/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-26
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_FBCONS_H_
#define _SYS_FBCONS_H_

#include <stddef.h>
#include <stdint.h>

/* 32-bit RGB888 framebuffer, with pitch in bytes and packed colors. */
void fbcons_init(volatile uint32_t *, size_t, size_t, size_t, uint32_t,
    uint32_t);

#endif /* _SYS_FBCONS_H_ */
