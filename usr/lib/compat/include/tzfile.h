/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/* The calendar constants of BSD's <tzfile.h> that programs use. */

#ifndef _COMPAT_TZFILE_H_
#define _COMPAT_TZFILE_H_

#define SECSPERMIN      60
#define MINSPERHOUR     60
#define HOURSPERDAY     24
#define DAYSPERWEEK     7
#define DAYSPERNYEAR    365
#define DAYSPERLYEAR    366
#define SECSPERHOUR     (SECSPERMIN * MINSPERHOUR)
#define SECSPERDAY      ((long)SECSPERHOUR * HOURSPERDAY)
#define MONSPERYEAR     12
#define TM_YEAR_BASE    1900

#endif /* _COMPAT_TZFILE_H_ */
