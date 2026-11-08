/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Write the test disk image: 16384 sectors of 512 bytes. Sector n holds
 * n as a little-endian 64-bit value, then "BELKADSK", then bytes
 * (n * 7 + offset) mod 256. The kernel self-tests check this pattern.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTORS         16384
#define SECSIZE         512

int
main(int argc, char **argv)
{
        unsigned char sec[SECSIZE];
        FILE *f;

        if (argc != 2) {
                fprintf(stderr, "usage: %s image\n", argv[0]);
                return EXIT_FAILURE;
        }
        if ((f = fopen(argv[1], "wb")) == NULL) {
                perror(argv[1]);
                return EXIT_FAILURE;
        }
        for (uint64_t n = 0; n < SECTORS; n++) {
                for (int i = 0; i < 8; i++)
                        sec[i] = (unsigned char)(n >> (8 * i));
                memcpy(sec + 8, "BELKADSK", 8);
                for (int i = 16; i < SECSIZE; i++)
                        sec[i] = (unsigned char)((n * 7 + (uint64_t)i) & 0xff);
                if (fwrite(sec, sizeof(sec), 1, f) != 1) {
                        perror(argv[1]);
                        fclose(f);
                        return EXIT_FAILURE;
                }
        }
        if (fclose(f) != 0) {
                perror(argv[1]);
                return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
}
