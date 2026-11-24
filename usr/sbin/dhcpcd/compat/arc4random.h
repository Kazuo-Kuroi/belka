/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/* arc4random for dhcpcd, over getentropy() (compat-belka.c). */

#ifndef _DHCPCD_ARC4RANDOM_H_
#define _DHCPCD_ARC4RANDOM_H_

#include <stddef.h>
#include <stdint.h>

uint32_t arc4random(void);
void    arc4random_buf(void *, size_t);

#endif /* _DHCPCD_ARC4RANDOM_H_ */
