# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-17
# SPDX-License-Identifier: MIT
# Default rules for bmake; the native compiler is installed separately.
.SUFFIXES: .o .c .y .l
CC?=cc
CFLAGS?=-O2
AR?=ar
ARFLAGS?=rv
YACC?=yacc
LEX?=lex
.c.o:
	${CC} ${CPPFLAGS} ${CFLAGS} -c ${.IMPSRC}
.c:
	${CC} ${CPPFLAGS} ${CFLAGS} ${LDFLAGS} -o ${.TARGET} ${.IMPSRC} ${LDLIBS}
.y.c:
	${YACC} ${YFLAGS} ${.IMPSRC}
	mv y.tab.c ${.TARGET}
.l.c:
	${LEX} ${LFLAGS} -t ${.IMPSRC} > ${.TARGET}
