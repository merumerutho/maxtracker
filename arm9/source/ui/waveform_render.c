/*
 * waveform_render.c — Shared waveform visualization.
 *
 * See header for the design rationale. All three waveform scopes in
 * the tracker (sample_view, waveform_view, lfe_fx_view) route through
 * this single renderer so their styling stays in lockstep.
 *
 * Rendering is done via direct byte stores into the shadow framebuffer
 * (cached main RAM, not VRAM) — see screen.c's double-buffering note.
 */

#include "waveform_render.h"
#include "screen.h"

#include <stdint.h>

/* FIT + 3 zoomed-in levels. FIT is index 0 (value 0 = use full length). */
const u32 waveform_zoom_levels[WAVEFORM_ZOOM_COUNT] = {
    0,      /* FIT */
    4096,
    1024,
    256,
};

/* Clear a rectangular region of the shadow framebuffer with bg_color.
 * Uses CPU word stores for cache coherency with subsequent pixel writes. */
static void clear_region(u8 *fb, int y_top, int height, u8 bg_color)
{
    u32 quad = ((u32)bg_color << 24) | ((u32)bg_color << 16) |
               ((u32)bg_color << 8)  | (u32)bg_color;
    u32 *dst = (u32 *)(fb + y_top * 256);
    int words = (256 * height) >> 2;
    for (int i = 0; i < words; i++) dst[i] = quad;
}

/* Draw a horizontal line spanning the full 256-column width. */
static void draw_hline(u8 *fb, int y, u8 color)
{
    u32 quad = ((u32)color << 24) | ((u32)color << 16) |
               ((u32)color << 8)  | (u32)color;
    u32 *dst = (u32 *)(fb + y * 256);
    for (int i = 0; i < 256 / 4; i++) dst[i] = quad;
}

/* Draw a solid vertical line at column px from y0 to y1 inclusive. */
static void draw_vline(u8 *fb, int px, int y0, int y1, u8 color)
{
    if (px < 0 || px >= 256) return;
    for (int y = y0; y <= y1; y++)
        fb[y * 256 + px] = color;
}

/* Draw a dotted vertical line (every other row). */
static void draw_vline_dotted(u8 *fb, int px, int y0, int y1, u8 color)
{
    if (px < 0 || px >= 256) return;
    for (int y = y0; y <= y1; y += 2)
        fb[y * 256 + px] = color;
}

void waveform_render(const WaveformRenderCfg *cfg)
{
    if (!cfg || !cfg->fb || cfg->y_height <= 0) return;

    u8 *fb = cfg->fb;
    int y_top = cfg->y_top;
    int y_bot = y_top + cfg->y_height;      /* exclusive */
    int y_mid = y_top + cfg->y_height / 2;

    /* Background + center line. */
    clear_region(fb, y_top, cfg->y_height, cfg->color_bg);
    draw_hline(fb, y_mid, cfg->color_center);

    /* No data → stop here (caller may overlay a "no data" label). */
    if (!cfg->pcm || cfg->length == 0) return;

    /* Resolve visible sample count (FIT sentinel → full length). */
    u32 visible = cfg->visible;
    if (visible == 0 || visible > cfg->length) visible = cfg->length;
    if (visible == 0) return;

    u32 scroll = cfg->scroll;
    if (scroll >= cfg->length) scroll = 0;
    if (scroll + visible > cfg->length) {
        scroll = cfg->length > visible ? cfg->length - visible : 0;
    }

    bool has_range = (cfg->sel_end > cfg->sel_start);
    int half_h = cfg->y_height / 2;

    /* Main waveform draw: one sample per column, vertical fill. */
    for (int px = 0; px < 256; px++) {
        u32 si = scroll + (u32)((uint64_t)px * visible / 256);
        if (si >= cfg->length) si = cfg->length - 1;

        int val;
        if (cfg->is_16bit) {
            const int16_t *p16 = (const int16_t *)cfg->pcm;
            val = (p16[si] * half_h) / 32768;
        } else {
            const int8_t *p8 = (const int8_t *)cfg->pcm;
            val = (p8[si] * half_h) / 128;
        }

        int y = y_mid - val;
        if (y < y_top)     y = y_top;
        if (y >= y_bot)    y = y_bot - 1;

        u8 color = cfg->color_wave;
        if (has_range) {
            bool in_range = (si >= cfg->sel_start && si < cfg->sel_end);
            color = in_range ? cfg->color_sel_in : cfg->color_wave;
        }

        int ya = (y < y_mid) ? y : y_mid;
        int yb = (y < y_mid) ? y_mid : y;
        draw_vline(fb, px, ya, yb, color);
    }

    /* Selection boundary markers (solid vertical lines). */
    if (has_range && cfg->color_sel_marker) {
        if (cfg->sel_start >= scroll) {
            u32 px = (u32)((uint64_t)(cfg->sel_start - scroll) * 256 / visible);
            if (px < 256)
                draw_vline(fb, (int)px, y_top, y_bot - 1, cfg->color_sel_marker);
        }
        if (cfg->sel_end >= scroll) {
            u32 px = (u32)((uint64_t)(cfg->sel_end - scroll) * 256 / visible);
            if (px > 255) px = 255;
            draw_vline(fb, (int)px, y_top, y_bot - 1, cfg->color_sel_marker);
        }
    }

    /* Loop markers (dotted vertical lines). */
    if (cfg->loop_length > 0 && cfg->color_loop) {
        u32 ls = cfg->loop_start;
        u32 le = cfg->loop_start + cfg->loop_length;
        if (ls >= scroll) {
            u32 px = (u32)((uint64_t)(ls - scroll) * 256 / visible);
            if (px < 256)
                draw_vline_dotted(fb, (int)px, y_top, y_bot - 1, cfg->color_loop);
        }
        if (le >= scroll) {
            u32 px = (u32)((uint64_t)(le - scroll) * 256 / visible);
            if (px < 256)
                draw_vline_dotted(fb, (int)px, y_top, y_bot - 1, cfg->color_loop);
        }
    }
}
