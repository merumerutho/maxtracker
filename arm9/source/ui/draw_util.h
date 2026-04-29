/*
 * draw_util.h -- Shared pixel plotting and touch-coordinate helpers.
 *
 * NDS VRAM requires 16-bit aligned writes; this helper handles the
 * read-modify-write for plotting a single 8-bit palette pixel.
 */

#ifndef MT_DRAW_UTIL_H
#define MT_DRAW_UTIL_H

#include <nds.h>

/* Plot a single pixel on an 8bpp framebuffer using 16-bit VRAM writes.
 * Includes bounds checking for safety (NDS screens are 256x192). */
static inline void plot_pixel(u8 *fb, int x, int y, u8 color)
{
    if (x < 0 || x >= 256 || y < 0 || y >= 192) return;
    int offset = y * 256 + x;
    vu16 *hw = (vu16 *)(fb + (offset & ~1));
    u16 val = *hw;
    if (offset & 1)
        val = (val & 0x00FF) | ((u16)color << 8);
    else
        val = (val & 0xFF00) | (u16)color;
    *hw = val;
}

#define MT_TOUCH_RAW_X_MIN  320
#define MT_TOUCH_RAW_X_MAX  3808
#define MT_TOUCH_RAW_Y_MIN  224
#define MT_TOUCH_RAW_Y_MAX  3904

static inline void touch_read_pixel(int *out_px, int *out_py)
{
    touchPosition t;
    touchRead(&t);

    int rx = (int)t.rawx - MT_TOUCH_RAW_X_MIN;
    int ry = (int)t.rawy - MT_TOUCH_RAW_Y_MIN;
    int px = (rx * 256) / (MT_TOUCH_RAW_X_MAX - MT_TOUCH_RAW_X_MIN);
    int py = (ry * 192) / (MT_TOUCH_RAW_Y_MAX - MT_TOUCH_RAW_Y_MIN);

    if (px < 0)    px = 0;
    if (px > 255)  px = 255;
    if (py < 0)    py = 0;
    if (py > 191)  py = 191;

    *out_px = px;
    *out_py = py;
}

static inline void ui_draw_fader(u8 *fb,
                                 int px_x, int px_y,
                                 int px_w, int px_h,
                                 int val, int val_max,
                                 u8 fill_color, u8 track_color)
{
    if (px_w < 1 || px_h < 1) return;
    if (val < 0)       val = 0;
    if (val > val_max) val = val_max;
    if (val_max <= 0)  val_max = 1;
    int fill_w = (val * px_w) / val_max;

    for (int y = px_y; y < px_y + px_h && y < 192; y++) {
        if (y < 0) continue;
        u8 *row = fb + y * 256;
        for (int x = px_x; x < px_x + fill_w && x < 256; x++)
            if (x >= 0) row[x] = fill_color;
        for (int x = px_x + fill_w; x < px_x + px_w && x < 256; x++)
            if (x >= 0) row[x] = track_color;
    }
}

#endif /* MT_DRAW_UTIL_H */
