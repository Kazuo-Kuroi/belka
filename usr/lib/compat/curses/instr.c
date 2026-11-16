/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-15
 * SPDX-License-Identifier: MIT
 */

/*
 * Reading a window's contents back: the instr and inchstr families of
 * curses(3), Belka's own, in place of NetBSD's (whose licence has an
 * advertising clause). They start at the cursor and stop at the end of
 * its line, and leave the cursor where it was.
 *
 *   winnstr(win, str, n)       at most n characters, as text, and a
 *                              NUL; n < 0 means the rest of the line.
 *                              Returns the number stored.
 *   winchnstr(win, chstr, n)   at most n characters with their
 *                              attributes, and a 0. Returns OK.
 *
 * The others are these, on stdscr, after a move, or without a limit.
 * Built on the public interface alone (winch and wmove).
 */

#include "curses.h"

/* The cells from the cursor to the end of its line, at most n. */
static int
span(WINDOW *win, int n)
{
        int left;

        left = getmaxx(win) - getcurx(win);
        if (left < 0)
                left = 0;
        if (n >= 0 && n < left)
                left = n;
        return left;
}

int
winnstr(WINDOW *win, char *str, int n)
{
        int y, x, i, count;

        if (win == NULL || str == NULL)
                return ERR;
        y = getcury(win);
        x = getcurx(win);
        count = span(win, n);
        for (i = 0; i < count; i++) {
                if (wmove(win, y, x + i) == ERR)
                        break;
                str[i] = (char)(winch(win) & A_CHARTEXT);
        }
        str[i] = '\0';
        wmove(win, y, x);
        return i;
}

int
winchnstr(WINDOW *win, chtype *chstr, int n)
{
        int y, x, i, count;

        if (win == NULL || chstr == NULL)
                return ERR;
        y = getcury(win);
        x = getcurx(win);
        count = span(win, n);
        for (i = 0; i < count; i++) {
                if (wmove(win, y, x + i) == ERR)
                        break;
                chstr[i] = winch(win);
        }
        chstr[i] = 0;
        wmove(win, y, x);
        return OK;
}

int
winstr(WINDOW *win, char *str)
{
        return winnstr(win, str, -1);
}

int
winchstr(WINDOW *win, chtype *chstr)
{
        return winchnstr(win, chstr, -1);
}

#ifndef _CURSES_USE_MACROS

int
innstr(char *str, int n)
{
        return winnstr(stdscr, str, n);
}

int
instr(char *str)
{
        return winnstr(stdscr, str, -1);
}

int
inchnstr(chtype *chstr, int n)
{
        return winchnstr(stdscr, chstr, n);
}

int
inchstr(chtype *chstr)
{
        return winchnstr(stdscr, chstr, -1);
}

int
mvwinnstr(WINDOW *win, int y, int x, char *str, int n)
{
        if (wmove(win, y, x) == ERR)
                return ERR;
        return winnstr(win, str, n);
}

int
mvwinstr(WINDOW *win, int y, int x, char *str)
{
        return mvwinnstr(win, y, x, str, -1);
}

int
mvwinchnstr(WINDOW *win, int y, int x, chtype *chstr, int n)
{
        if (wmove(win, y, x) == ERR)
                return ERR;
        return winchnstr(win, chstr, n);
}

int
mvwinchstr(WINDOW *win, int y, int x, chtype *chstr)
{
        return mvwinchnstr(win, y, x, chstr, -1);
}

int
mvinnstr(int y, int x, char *str, int n)
{
        return mvwinnstr(stdscr, y, x, str, n);
}

int
mvinstr(int y, int x, char *str)
{
        return mvwinnstr(stdscr, y, x, str, -1);
}

int
mvinchnstr(int y, int x, chtype *chstr, int n)
{
        return mvwinchnstr(stdscr, y, x, chstr, n);
}

int
mvinchstr(int y, int x, chtype *chstr)
{
        return mvwinchnstr(stdscr, y, x, chstr, -1);
}

#endif /* !_CURSES_USE_MACROS */
