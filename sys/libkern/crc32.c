/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * CRC-32, the IEEE 802.3 polynomial (0x04c11db7, bit-reflected
 * 0xedb88320), as GPT and Ethernet use it: crc32(0, p, n) of "123456789"
 * is 0xcbf43926. A running CRC goes back in as the first argument. A
 * table of 256 words is made at the first call.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/systm.h>

static uint32_t crc32_table[256];
static volatile int crc32_ready;

uint32_t
crc32(uint32_t crc, const void *buf, size_t len)
{
        const uint8_t *p = buf;

        if (!crc32_ready) {
                for (uint32_t i = 0; i < 256; i++) {
                        uint32_t c = i;

                        for (int k = 0; k < 8; k++)
                                c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) :
                                    c >> 1;
                        crc32_table[i] = c;
                }
                crc32_ready = 1;
        }
        crc = ~crc;
        while (len-- > 0)
                crc = crc32_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
        return ~crc;
}
