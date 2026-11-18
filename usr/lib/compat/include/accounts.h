/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef BELKA_ACCOUNTS_H
#define BELKA_ACCOUNTS_H

#define ACCT_FILES 3

extern const char *const acct_files[ACCT_FILES];

/* The caller holds the password-file lock between lock and unlock. */
int acct_lock(void);
void acct_unlock(void);
int acct_commit(const char *const [ACCT_FILES]);

#endif
