/*
 * font.c — Multi-face dual-size bitmap font renderer.
 *
 * SMALL mode: 4x6 glyphs, 64x32 grid. 1 byte per row, 6 bytes per glyph.
 *             Bits 7..4 = pixels left-to-right (bit 7 = leftmost).
 * BIG mode:   6x8 glyphs, 42x24 grid. 1 byte per row, 8 bytes per glyph.
 *             Bits 7..2 = pixels left-to-right (bit 7 = leftmost).
 *
 * Font faces are defined in individual headers under fonts/. To add a
 * new font, create a header with the glyph array and add one line to
 * the SMALL_FONTS or BIG_FONTS X-macro table below.
 */

#include "font.h"
#include "screen.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ================================================================== */
/* Font data (each header defines one static const u8[] array)         */
/* ================================================================== */

#include "fonts/font_max_small.h"
#include "fonts/font_roseumteam.h"
#include "fonts/font_max_big.h"
#include "fonts/font_portfolio.h"

/* ================================================================== */
/* Font face tables — X(name_string, array_symbol)                     */
/*                                                                     */
/* To add a font: include its header above, then add an X() line here. */
/* ================================================================== */

#define SMALL_FONTS(X) \
    X("maxSmall",    font_max_small)   \
    X("roseumteam",  font_roseumteam)

#define BIG_FONTS(X) \
    X("maxBig",     font_max_big)    \
    X("Portfolio",  font_portfolio)

typedef struct {
    const char *name;
    const u8   *data;
} FontFaceEntry;

#define FACE_ENTRY(n, d)  { n, d },

static const FontFaceEntry small_faces[] = {
    SMALL_FONTS(FACE_ENTRY)
};

static const FontFaceEntry big_faces[] = {
    BIG_FONTS(FACE_ENTRY)
};

#define SMALL_FACE_COUNT (int)(sizeof(small_faces) / sizeof(small_faces[0]))
#define BIG_FACE_COUNT   (int)(sizeof(big_faces)   / sizeof(big_faces[0]))

/* ================================================================== */
/* Runtime font state                                                  */
/* ================================================================== */

int FONT_W = 6;
int FONT_H = 8;
int FONT_COLS = 42;
int FONT_ROWS = 24;

static FontMode current_mode = FONT_MODE_BIG;
static int face_idx_small = 0;
static int face_idx_big   = 1;

static const u8 *active_small = font_max_small;
static const u8 *active_big   = font_portfolio;

void font_init(void)
{
    font_set_mode(FONT_MODE_BIG);
}

static void apply_face(void)
{
    active_small = small_faces[face_idx_small].data;
    active_big   = big_faces[face_idx_big].data;
}

void font_set_mode(FontMode mode)
{
    current_mode = mode;
    if (mode == FONT_MODE_BIG) {
        FONT_W = 6;
        FONT_H = 8;
    } else {
        FONT_W = 4;
        FONT_H = 6;
    }
    FONT_COLS = 256 / FONT_W;
    FONT_ROWS = 192 / FONT_H;

    if (top_fb) font_clear(top_fb, PAL_BG);
    if (bot_fb) font_clear(bot_fb, PAL_BG);
}

FontMode font_get_mode(void)
{
    return current_mode;
}

int font_face_count(FontMode mode)
{
    return (mode == FONT_MODE_BIG) ? BIG_FACE_COUNT : SMALL_FACE_COUNT;
}

const char *font_face_name(FontMode mode, int index)
{
    if (mode == FONT_MODE_BIG) {
        if (index < 0 || index >= BIG_FACE_COUNT) return "?";
        return big_faces[index].name;
    } else {
        if (index < 0 || index >= SMALL_FACE_COUNT) return "?";
        return small_faces[index].name;
    }
}

void font_set_face(FontMode mode, int index)
{
    if (mode == FONT_MODE_BIG) {
        if (index < 0 || index >= BIG_FACE_COUNT) return;
        face_idx_big = index;
    } else {
        if (index < 0 || index >= SMALL_FACE_COUNT) return;
        face_idx_small = index;
    }
    apply_face();

    if (top_fb) font_clear(top_fb, PAL_BG);
    if (bot_fb) font_clear(bot_fb, PAL_BG);
}

int font_get_face(FontMode mode)
{
    return (mode == FONT_MODE_BIG) ? face_idx_big : face_idx_small;
}

/* ================================================================== */
/* Glyph rendering                                                     */
/* ================================================================== */

static inline void draw_glyph_small(u8 *fb, int px, int py,
                                    u8 glyph_idx, u8 color)
{
    const u8 *g = &active_small[glyph_idx * 6];

    for (int row = 0; row < 6; row++) {
        int y = py + row;
        if (y < 0 || y >= 192) continue;

        u8 bits = g[row];
        if (bits == 0) continue;

        u8 *dst = fb + y * 256 + px;
        if (bits & 0x80) dst[0] = color;
        if (bits & 0x40) dst[1] = color;
        if (bits & 0x20) dst[2] = color;
        if (bits & 0x10) dst[3] = color;
    }
}

static inline void draw_glyph_big(u8 *fb, int px, int py,
                                  u8 glyph_idx, u8 color)
{
    const u8 *g = &active_big[glyph_idx * 8];

    for (int row = 0; row < 8; row++) {
        int y = py + row;
        if (y < 0 || y >= 192) continue;

        u8 bits = g[row];
        if (bits == 0) continue;

        u8 *dst = fb + y * 256 + px;
        if (bits & 0x80) dst[0] = color;
        if (bits & 0x40) dst[1] = color;
        if (bits & 0x20) dst[2] = color;
        if (bits & 0x10) dst[3] = color;
        if (bits & 0x08) dst[4] = color;
        if (bits & 0x04) dst[5] = color;
    }
}

void font_putc(u8 *fb, int col, int row, char ch, u8 color)
{
    if (col < 0 || col >= FONT_COLS || row < 0 || row >= FONT_ROWS) return;
    u8 idx = (u8)ch;
    if (idx < 0x20 || idx > 0x7F) idx = 0x20;
    int px = col * FONT_W;
    int py = row * FONT_H;
    if (px + FONT_W > 256 || py + FONT_H > 192) return;

    if (current_mode == FONT_MODE_BIG)
        draw_glyph_big(fb, px, py, idx - 0x20, color);
    else
        draw_glyph_small(fb, px, py, idx - 0x20, color);
}

int font_puts(u8 *fb, int col, int row, const char *str, u8 color)
{
    int n = 0;
    while (*str && col < FONT_COLS) {
        font_putc(fb, col, row, *str, color);
        col++;
        str++;
        n++;
    }
    return n;
}

void font_puts_colored(u8 *fb, int col, int row,
                       const char *str, const u8 *colors, int len)
{
    for (int i = 0; i < len && col < FONT_COLS; i++, col++)
        font_putc(fb, col, row, str[i], colors[i]);
}

/* ================================================================== */
/* Row/screen fills                                                    */
/* ================================================================== */

static inline void fill_span(u8 *fb, int y, int px_start, int px_end, u8 bg)
{
    if (y < 0 || y >= 192) return;
    if (px_start < 0) px_start = 0;
    if (px_end > 256) px_end = 256;
    if (px_end <= px_start) return;

    u8 *row = fb + y * 256;
    int x = px_start;

    while (x < px_end && (x & 3)) row[x++] = bg;

    if (x + 4 <= px_end) {
        u32 quad = ((u32)bg << 24) | ((u32)bg << 16) |
                   ((u32)bg << 8)  | (u32)bg;
        u32 *w = (u32 *)(row + x);
        int words = (px_end - x) >> 2;
        for (int i = 0; i < words; i++) w[i] = quad;
        x += words * 4;
    }

    while (x < px_end) row[x++] = bg;
}

void font_fill_row(u8 *fb, int row, int col_start, int col_end, u8 bg)
{
    if (row < 0 || row >= FONT_ROWS) return;
    if (col_start < 0) col_start = 0;
    if (col_start >= FONT_COLS) return;
    if (col_end > FONT_COLS) col_end = FONT_COLS;
    if (col_end <= col_start) return;

    int px_start = col_start * FONT_W;
    int px_end   = col_end * FONT_W;
    int py_start = row * FONT_H;

    for (int y = py_start; y < py_start + FONT_H && y < 192; y++)
        fill_span(fb, y, px_start, px_end, bg);
}

void font_clear(u8 *fb, u8 bg)
{
    u32 quad = ((u32)bg << 24) | ((u32)bg << 16) | ((u32)bg << 8) | (u32)bg;
    u32 *dst = (u32 *)fb;
    int words = (256 * 192) >> 2;
    for (int i = 0; i < words; i++) dst[i] = quad;
}

int font_printf(u8 *fb, int col, int row, u8 color, const char *fmt, ...)
{
    char buf[FONT_COLS_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return font_puts(fb, col, row, buf, color);
}
