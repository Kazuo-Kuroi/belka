/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-26
 * SPDX-License-Identifier: MIT
 */

/*
 * Bitmap output for the system console. The character grid lives in
 * ordinary RAM so that scrolling never reads from display memory.
 * The print lock serializes this output with the primary console.
 */

#include <sys/cons.h>
#include <sys/fbcons.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#include <misc-fixed/ascii8x13.h>

#define FB_COLS         80
#define FB_ROWS         24
#define CELL_WIDTH      8
#define CELL_HEIGHT     16

enum fb_escape {
        FB_TEXT, FB_ESC, FB_CSI, FB_IGNORE_CSI, FB_OSC, FB_OSC_ESC
};

static struct {
        volatile uint32_t *origin;      /* top left of the character grid */
        size_t          stride;         /* pixels between scanlines */
        unsigned int    scale;
        uint32_t        foreground;
        uint32_t        background;
        unsigned int    row, col;
        int             wrap;
        enum fb_escape  esc;
        unsigned int    args[2];
        unsigned int    arg;
        unsigned char   cells[FB_ROWS][FB_COLS];
} fb;

static void
fb_draw(unsigned int row, unsigned int col, unsigned char c)
{
        unsigned int s = fb.scale;
        volatile uint32_t *dst = fb.origin +
            row * CELL_HEIGHT * s * fb.stride + col * CELL_WIDTH * s;

        for (unsigned int y = 0; y < CELL_HEIGHT; y++) {
                unsigned int bits = y < 13 ? ascii8x13[c - ' '][y] : 0;

                for (unsigned int dy = 0; dy < s; dy++) {
                        for (unsigned int x = 0; x < CELL_WIDTH; x++) {
                                uint32_t pixel = (bits & (0x80U >> x)) != 0 ?
                                    fb.foreground : fb.background;
                                for (unsigned int dx = 0; dx < s; dx++)
                                        dst[x * s + dx] = pixel;
                        }
                        dst += fb.stride;
                }
        }
}

/* The underline occupies two blank scanlines below each glyph. */
static void
fb_cursor(int visible)
{
        unsigned int s = fb.scale;
        volatile uint32_t *dst = fb.origin +
            (fb.row * CELL_HEIGHT + CELL_HEIGHT - 2) * s * fb.stride +
            fb.col * CELL_WIDTH * s;
        uint32_t color = visible ? fb.foreground : fb.background;

        for (unsigned int y = 0; y < 2 * s; y++) {
                for (unsigned int x = 0; x < CELL_WIDTH * s; x++)
                        dst[x] = color;
                dst += fb.stride;
        }
}

static void
fb_cell(unsigned int row, unsigned int col, unsigned char c)
{
        if (fb.cells[row][col] != c) {
                fb.cells[row][col] = c;
                fb_draw(row, col, c);
        }
}

static void
fb_erase(unsigned int first, unsigned int last)
{
        for (unsigned int i = first; i < last; i++)
                fb_cell(i / FB_COLS, i % FB_COLS, ' ');
}

static void
fb_linefeed(void)
{
        fb.wrap = 0;
        if (fb.row + 1 < FB_ROWS) {
                fb.row++;
                return;
        }
        for (unsigned int row = 0; row + 1 < FB_ROWS; row++)
                for (unsigned int col = 0; col < FB_COLS; col++)
                        fb_cell(row, col, fb.cells[row + 1][col]);
        fb_erase((FB_ROWS - 1) * FB_COLS, FB_ROWS * FB_COLS);
}

static unsigned int
fb_limit(unsigned int value, unsigned int last)
{
        return value < last ? value : last;
}

/* Basic ANSI cursor motion and erasure used by terminal programs. */
static void
fb_csi(int c)
{
        unsigned int a = fb.args[0], b = fb.args[1];
        unsigned int n = a != 0 ? a : 1;
        unsigned int here = fb.row * FB_COLS + fb.col;

        switch (c) {
        case 'A':
                fb.row -= fb_limit(n, fb.row);
                break;
        case 'B':
                fb.row = fb_limit(fb.row + n, FB_ROWS - 1);
                break;
        case 'C':
                fb.col = fb_limit(fb.col + n, FB_COLS - 1);
                break;
        case 'D':
                fb.col -= fb_limit(n, fb.col);
                break;
        case 'G':
                fb.col = fb_limit(n - 1, FB_COLS - 1);
                break;
        case 'H':
        case 'f':
                fb.row = fb_limit(n - 1, FB_ROWS - 1);
                fb.col = fb_limit(b == 0 ? 0 : b - 1, FB_COLS - 1);
                break;
        case 'J':
                if (a == 0)
                        fb_erase(here, FB_ROWS * FB_COLS);
                else if (a == 1)
                        fb_erase(0, here + 1);
                else if (a == 2)
                        fb_erase(0, FB_ROWS * FB_COLS);
                break;
        case 'K':
                if (a == 0)
                        fb_erase(here, (fb.row + 1) * FB_COLS);
                else if (a == 1)
                        fb_erase(fb.row * FB_COLS, here + 1);
                else if (a == 2)
                        fb_erase(fb.row * FB_COLS, (fb.row + 1) * FB_COLS);
                break;
        default:
                return;
        }
        fb.wrap = 0;
}

/* Return nonzero if the byte belongs to an escape sequence. */
static int
fb_escape_byte(int c)
{
        if (c == '\r' || c == '\n') {
                fb.esc = FB_TEXT;
                return 0;
        }
        if (fb.esc == FB_OSC || fb.esc == FB_OSC_ESC) {
                if (c == '\a' || (fb.esc == FB_OSC_ESC && c == '\\'))
                        fb.esc = FB_TEXT;
                else
                        fb.esc = c == 27 ? FB_OSC_ESC : FB_OSC;
                return 1;
        }
        if (c == 27) {
                fb.esc = FB_ESC;
                return 1;
        }
        switch (fb.esc) {
        case FB_ESC:
                if (c == '[') {
                        fb.esc = FB_CSI;
                        fb.args[0] = fb.args[1] = fb.arg = 0;
                } else {
                        fb.esc = c == ']' ? FB_OSC : FB_TEXT;
                }
                break;
        case FB_CSI:
                if (c >= '0' && c <= '9') {
                        fb.args[fb.arg] = fb_limit(
                            fb.args[fb.arg] * 10 + (unsigned int)(c - '0'),
                            999);
                } else if (c == ';' && fb.arg == 0) {
                        fb.arg++;
                } else if (c >= 0x40 && c <= 0x7e) {
                        fb_csi(c);
                        fb.esc = FB_TEXT;
                } else {
                        fb.esc = FB_IGNORE_CSI;
                }
                break;
        case FB_IGNORE_CSI:
                if (c >= 0x40 && c <= 0x7e)
                        fb.esc = FB_TEXT;
                break;
        case FB_TEXT:
                return 0;
        default:
                break;
        }
        return 1;
}

static void
fb_text(int c)
{
        switch (c) {
        case '\r':
                fb.col = 0;
                fb.wrap = 0;
                break;
        case '\n':
                fb_linefeed();
                break;
        case '\b':
                if (!fb.wrap && fb.col != 0)
                        fb.col--;
                fb.wrap = 0;
                break;
        case '\t':
                fb.col = fb_limit((fb.col + 8) & ~7U, FB_COLS - 1);
                fb.wrap = 0;
                break;
        default:
                if (c < ' ' || c == 127)
                        break;
                if (fb.wrap) {
                        fb.col = 0;
                        fb_linefeed();
                }
                fb_cell(fb.row, fb.col, c < 127 ? (unsigned char)c : '?');
                if (fb.col + 1 < FB_COLS)
                        fb.col++;
                else
                        fb.wrap = 1;
                break;
        }
}

static void
fb_putc(struct consdev *cp __unused, int c)
{
        fb_cursor(0);
        if (!fb_escape_byte(c))
                fb_text(c);
        fb_cursor(1);
        /* Drain writes to the WC mapping before another CPU can print. */
        membar_sync();
}

void
fbcons_init(volatile uint32_t *pixels, size_t width, size_t height,
    size_t pitch, uint32_t foreground, uint32_t background)
{
        static struct consdev dev = { fb_putc, NULL, NULL, "framebuffer" };
        size_t x, y;

        fb.stride = pitch / sizeof(*pixels);
        fb.scale = width >= 2 * FB_COLS * CELL_WIDTH &&
            height >= 2 * FB_ROWS * CELL_HEIGHT ? 2 : 1;
        x = (width - FB_COLS * CELL_WIDTH * fb.scale) / 2;
        y = (height - FB_ROWS * CELL_HEIGHT * fb.scale) / 2;
        fb.origin = pixels + y * fb.stride + x;
        fb.foreground = foreground;
        fb.background = background;
        memset(fb.cells, ' ', sizeof(fb.cells));
        for (size_t row = 0; row < height; row++)
                for (size_t col = 0; col < width; col++)
                        pixels[row * fb.stride + col] = fb.background;
        fb_cursor(1);
        membar_sync();
        cnmirror(&dev);
}
