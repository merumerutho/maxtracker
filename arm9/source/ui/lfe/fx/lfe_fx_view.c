/*
 * lfe_fx_view.c — LFE Effects room (dispatcher).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Owns the shared view state (current effect index, cursor row, range
 * selection, zoom level, status line) and the key-routing / rendering
 * frame. Per-effect state and logic live in fx_<name>.c modules, each
 * exporting a wv_fx descriptor wired into the registry below.
 */

#ifdef MAXTRACKER_LFE

#include "lfe_fx_view.h"
#include "waveform_view.h"
#include "waveform_render.h"
#include "screen.h"
#include "font.h"
#include "draw_util.h"
#include "editor_state.h"
#include "lfe.h"

#include "wv_fx.h"
#include "fx_distortion.h"
#include "fx_filter.h"
#include "fx_delay.h"
#include "fx_envelope.h"
#include "fx_normalize.h"
#include "fx_ott.h"
#include "fx_trim.h"
#include "fx_reverse.h"
#include "fx_bitcrush.h"

#include <nds.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Effect registry                                                     */
/* ================================================================== */

static const wv_fx * const wv_fxs[] = {
    &wv_fx_distortion,
    &wv_fx_filter,
    &wv_fx_delay,
    &wv_fx_envelope,
    &wv_fx_normalize,
    &wv_fx_ott,
    &wv_fx_trim,
    &wv_fx_reverse,
    &wv_fx_bitcrush,
};
#define FX_COUNT ((int)(sizeof(wv_fxs) / sizeof(wv_fxs[0])))

/* ================================================================== */
/* View state                                                          */
/* ================================================================== */

/* Touch drag mode — which UI region owns the current touch stream. Set
 * on the initial KEY_TOUCH press, held until release. */
typedef enum { FXV_TOUCH_NONE = 0, FXV_TOUCH_RANGE, FXV_TOUCH_SCROLL } fxv_touch_mode;

static struct {
    bool active;
    int  current_fx;
    int  param_row;

    u32  range_start;
    u32  range_end;
    int  vis_zoom;

    /* Horizontal scroll in samples. When scroll_manual is false, the
     * waveform auto-centers around the range; a scrollbar drag flips it
     * true and cycling zoom (Y) resets it. */
    u32  scroll;
    bool scroll_manual;

    /* Which edge the current touch drag controls: 0 = start, 1 = end.
     * On initial touch we pick the nearest edge; subsequent drag
     * samples move only that edge. */
    int  drag_edge;
    fxv_touch_mode touch_mode;

    char status[48];
    int  status_timer;
} fxv;

/* Bot-screen waveform viewport geometry (must match draw_waveform). */
#define FXV_WAVE_Y_TOP       18
#define FXV_WAVE_Y_HEIGHT    138
#define FXV_SCROLLBAR_Y      158
#define FXV_SCROLLBAR_HEIGHT 10
#define FXV_SCREEN_W         256

/* ------------------------------------------------------------------ */
/* Shared helpers exposed to the effect modules                        */
/* ------------------------------------------------------------------ */

uint16_t wv_fx_to_q15(int val)
{
    if (val <= 0) return 0;
    if (val >= 127) return LFE_Q15_ONE;
    return (uint16_t)((val * LFE_Q15_ONE) / 127);
}

void wv_fx_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fxv.status, sizeof(fxv.status), fmt, ap);
    va_end(ap);
    fxv.status_timer = 180;
}

void wv_fx_report_apply(const char *name,
                        lfe_status rc,
                        const lfe_fx_range *range)
{
    if (rc == LFE_OK)
        wv_fx_set_status("%s applied [%04X-%04X]", name,
                         (unsigned)range->start, (unsigned)range->end);
    else
        wv_fx_set_status("FX error %d", (int)rc);
}

void wv_fx_draw_row(u8 *fb, int *row, int cursor, int index,
                    const char *fmt, ...)
{
    u8 color = (index == cursor) ? PAL_WHITE : PAL_GRAY;
    if (index == cursor) font_fill_row(fb, *row, 0, 40, PAL_ROW_CURSOR);

    char line[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    font_puts(fb, 1, *row, line, color);
    (*row)++;
}

/* Fader bar geometry — pixel coordinates derived from the font grid.
 * The bar starts after ~16 text columns and extends to near the right
 * edge, leaving a 1-column margin. */
#define FADER_COL_START  16
#define FADER_COL_END    40
#define FADER_TRACK_PAL  PAL_HEADER_BG

void wv_fx_draw_fader_row(u8 *fb, int *row, int cursor, int index,
                          int val, int val_max, u8 bar_color,
                          const char *fmt, ...)
{
    u8 color = (index == cursor) ? PAL_WHITE : PAL_GRAY;
    if (index == cursor) font_fill_row(fb, *row, 0, FADER_COL_END, PAL_ROW_CURSOR);

    char line[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    font_puts(fb, 1, *row, line, color);

    int px_x = FADER_COL_START * FONT_W + 4;
    int bar_w = ((FADER_COL_END - FADER_COL_START) * FONT_W * 2) / 3 - 4;
    if (bar_w < 4) bar_w = 4;
    int py_top = (*row) * FONT_H + 1;
    int bar_h  = FONT_H - 4;
    if (bar_h < 1) bar_h = 1;

    ui_draw_fader(fb, px_x, py_top, bar_w, bar_h,
                  val, val_max, bar_color, FADER_TRACK_PAL);

    (*row)++;
}

static const wv_fx *cur_fx(void)
{
    int i = fxv.current_fx;
    if (i < 0 || i >= FX_COUNT) return wv_fxs[0];
    return wv_fxs[i];
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

static void fxv_reset(void)
{
    fxv.current_fx    = 0;
    fxv.param_row     = 0;
    fxv.range_start   = 0;
    fxv.range_end     = wv_get_draft_length();
    fxv.vis_zoom      = 0;
    fxv.scroll        = 0;
    fxv.scroll_manual = false;
    fxv.touch_mode    = FXV_TOUCH_NONE;
    fxv.drag_edge     = 0;
    fxv.status[0]     = '\0';
    fxv.status_timer  = 0;

    for (int i = 0; i < FX_COUNT; i++) {
        if (wv_fxs[i]->on_reset) wv_fxs[i]->on_reset();
    }
}

void lfe_fx_view_open(void)
{
    fxv_reset();
    fxv.active = true;
}

void lfe_fx_view_close(void)
{
    fxv.active = false;
}

bool lfe_fx_view_is_active(void)
{
    return fxv.active;
}

/* ================================================================== */
/* Effect application (X button)                                       */
/* ================================================================== */

static void apply_effect(void)
{
    lfe_buffer buf;
    if (!wv_get_draft_buffer(&buf)) {
        wv_fx_set_status("No draft loaded");
        return;
    }

    u32 len = buf.length;
    u32 rs = fxv.range_start;
    u32 re = fxv.range_end;
    if (rs >= len) rs = 0;
    if (re > len)  re = len;
    if (re <= rs) { re = len; rs = 0; }

    lfe_fx_range range = { .start = rs, .end = re };
    cur_fx()->apply(&buf, &range);

    wv_mark_draft_dirty();
    mt_mark_song_modified();
}

/* ================================================================== */
/* Input                                                               */
/* ================================================================== */

/* Forward decl — used inside fxv_clamp_to_draft, defined below. */
static u32 visible_samples(u32 len);

/* Re-clamp fxv.range_start/range_end and fxv.scroll against the current
 * draft length. The draft can be reallocated behind our back when the
 * user runs a LFE generator (which replaces wv_draft.pcm with a new
 * length), so on every frame we make sure the UI indices stay in
 * bounds. A length that dropped below the old selection now silently
 * resets to the full buffer instead of pointing past the end. */
static void fxv_clamp_to_draft(void)
{
    u32 len = wv_get_draft_length();
    if (len == 0) {
        fxv.range_start   = 0;
        fxv.range_end     = 0;
        fxv.scroll        = 0;
        fxv.scroll_manual = false;
        return;
    }
    if (fxv.range_end > len)       fxv.range_end   = len;
    if (fxv.range_start >= len)    fxv.range_start = 0;
    if (fxv.range_end <= fxv.range_start) {
        fxv.range_start = 0;
        fxv.range_end   = len;
    }
    u32 visible = visible_samples(len);
    if (visible >= len) {
        fxv.scroll        = 0;
        fxv.scroll_manual = false;
    } else if (fxv.scroll > len - visible) {
        fxv.scroll = len - visible;
    }
}

/* Number of visible samples at the current zoom, clamped to buffer length.
 * Returns the FIT value when zoom index is 0 or exceeds the buffer. */
static u32 visible_samples(u32 len)
{
    int z = fxv.vis_zoom;
    if (z < 0) z = 0;
    if (z >= WAVEFORM_ZOOM_COUNT) z = WAVEFORM_ZOOM_COUNT - 1;
    u32 v = waveform_zoom_levels[z];
    if (v == 0 || v > len) v = len;
    return v;
}

/* Current horizontal scroll offset in samples — manually set when the
 * user drags the scrollbar, otherwise auto-centered on the range. */
static u32 effective_scroll(u32 len, u32 visible)
{
    if (visible >= len) return 0;
    u32 max_scroll = len - visible;
    if (fxv.scroll_manual) {
        return fxv.scroll > max_scroll ? max_scroll : fxv.scroll;
    }
    u32 mid = (fxv.range_start + fxv.range_end) / 2;
    u32 scroll = mid > visible / 2 ? mid - visible / 2 : 0;
    if (scroll > max_scroll) scroll = max_scroll;
    return scroll;
}

/* Map a bot-screen X pixel inside the waveform viewport to a sample index. */
static u32 px_to_sample(int px, u32 scroll, u32 visible)
{
    if (px < 0) px = 0;
    if (px >= FXV_SCREEN_W) px = FXV_SCREEN_W - 1;
    u32 s = scroll + ((u32)px * visible) / FXV_SCREEN_W;
    return s;
}

/* Map a bot-screen X pixel along the scrollbar to a scroll offset. */
static u32 px_to_scroll(int px, u32 len, u32 visible)
{
    if (visible >= len) return 0;
    if (px < 0) px = 0;
    if (px >= FXV_SCREEN_W) px = FXV_SCREEN_W - 1;
    /* Treat touch px as the desired scroll left edge in proportion to
     * the trackable pixel range. Center the thumb on the finger by
     * subtracting half of the thumb width. */
    u32 thumb_w = (visible * FXV_SCREEN_W) / len;
    if (thumb_w > FXV_SCREEN_W) thumb_w = FXV_SCREEN_W;
    int adj = px - (int)(thumb_w / 2);
    if (adj < 0) adj = 0;
    u32 track = FXV_SCREEN_W - thumb_w;
    if (track == 0) return 0;
    u32 scroll = ((u32)adj * (len - visible)) / track;
    if (scroll > len - visible) scroll = len - visible;
    return scroll;
}

static void handle_touch(u32 down, u32 held)
{
    /* Release: clear the drag mode so the next press re-classifies. */
    if (!(held & KEY_TOUCH)) {
        fxv.touch_mode = FXV_TOUCH_NONE;
        return;
    }

    u32 len = wv_get_draft_length();
    if (len == 0) return;
    u32 visible = visible_samples(len);
    u32 scroll  = effective_scroll(len, visible);

    int px, py;
    touch_read_pixel(&px, &py);

    bool fresh = (down & KEY_TOUCH) != 0;
    if (fresh) {
        if (py >= FXV_WAVE_Y_TOP &&
            py <  FXV_WAVE_Y_TOP + FXV_WAVE_Y_HEIGHT) {
            fxv.touch_mode    = FXV_TOUCH_RANGE;
            u32 s = px_to_sample(px, scroll, visible);
            if (s >= len) s = len - 1;
            u32 dist_start = (s >= fxv.range_start)
                             ? s - fxv.range_start
                             : fxv.range_start - s;
            u32 dist_end   = (s >= fxv.range_end)
                             ? s - fxv.range_end
                             : fxv.range_end - s;
            fxv.drag_edge = (dist_start <= dist_end) ? 0 : 1;
            if (fxv.drag_edge == 0)
                fxv.range_start = s;
            else
                fxv.range_end = s + 1 <= len ? s + 1 : len;
            if (fxv.range_end <= fxv.range_start)
                fxv.range_end = fxv.range_start + 1;
            return;
        }
        if (visible < len &&
            py >= FXV_SCROLLBAR_Y &&
            py <  FXV_SCROLLBAR_Y + FXV_SCROLLBAR_HEIGHT) {
            fxv.touch_mode    = FXV_TOUCH_SCROLL;
            fxv.scroll_manual = true;
            fxv.scroll        = px_to_scroll(px, len, visible);
            return;
        }
        fxv.touch_mode = FXV_TOUCH_NONE;
        return;
    }

    /* Continuation of an ongoing drag — move only the locked edge. */
    if (fxv.touch_mode == FXV_TOUCH_RANGE) {
        u32 s = px_to_sample(px, scroll, visible);
        if (s >= len) s = len - 1;
        if (fxv.drag_edge == 0) {
            fxv.range_start = s;
            if (fxv.range_start >= fxv.range_end)
                fxv.range_start = fxv.range_end > 1 ? fxv.range_end - 1 : 0;
        } else {
            fxv.range_end = s + 1 <= len ? s + 1 : len;
            if (fxv.range_end <= fxv.range_start)
                fxv.range_end = fxv.range_start + 1;
        }
    } else if (fxv.touch_mode == FXV_TOUCH_SCROLL) {
        fxv.scroll = px_to_scroll(px, len, visible);
    }
}

void lfe_fx_view_input(u32 down, u32 held)
{
    u32 rep = keysDownRepeat();

    if (fxv.status_timer > 0) fxv.status_timer--;

    /* The draft buffer can be resized by LFE generators between visits
     * to this room. Re-clamp before touching the range so stale indices
     * never reach the FX workers or the renderer. */
    fxv_clamp_to_draft();

    /* Touchscreen drives range selection and horizontal scroll. Runs
     * first so an active drag takes priority over d-pad chords. */
    handle_touch(down, held);
    if (fxv.touch_mode != FXV_TOUCH_NONE) return;

    if ((held & KEY_SELECT) && (down & KEY_A)) {
        wv_commit_draft();
        wv_fx_set_status("Draft committed");
        return;
    }

    if ((held & KEY_SELECT) && (down & KEY_B)) {
        wv_restore_original();
        fxv.range_end = wv_get_draft_length();
        wv_fx_set_status("Original restored");
        return;
    }

    if (down & KEY_X) {
        apply_effect();
        return;
    }

    if (down & KEY_Y) {
        fxv.vis_zoom = (fxv.vis_zoom + 1) % WAVEFORM_ZOOM_COUNT;
        /* Re-center on the current range after a zoom change — manual
         * scroll from a previous zoom level isn't meaningful here. */
        fxv.scroll_manual = false;
        return;
    }

    if (down & KEY_L) {
        fxv.current_fx = (fxv.current_fx + FX_COUNT - 1) % FX_COUNT;
        fxv.param_row = 0;
        return;
    }
    if (down & KEY_R) {
        fxv.current_fx = (fxv.current_fx + 1) % FX_COUNT;
        fxv.param_row = 0;
        return;
    }

    if (held & KEY_A) {
        if (rep & KEY_RIGHT) cur_fx()->adjust(fxv.param_row, +1, +1);
        if (rep & KEY_LEFT)  cur_fx()->adjust(fxv.param_row, -1, -1);
        if (rep & KEY_UP)    cur_fx()->adjust(fxv.param_row, +1, +16);
        if (rep & KEY_DOWN)  cur_fx()->adjust(fxv.param_row, -1, -16);
        return;
    }

    if (rep & KEY_UP) {
        if (fxv.param_row > 0) fxv.param_row--;
    }
    if (rep & KEY_DOWN) {
        int max_row = cur_fx()->param_count - 1;
        if (fxv.param_row < max_row) fxv.param_row++;
    }
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

/* Paint a flat 1-pixel-tall band on the shadow framebuffer. Used for
 * the scrollbar track and thumb — independent of the font grid. */
static void fb_hline(u8 *fb, int y, int x0, int x1, u8 color)
{
    if (y < 0 || y >= 192) return;
    if (x0 < 0) x0 = 0;
    if (x1 > FXV_SCREEN_W) x1 = FXV_SCREEN_W;
    for (int x = x0; x < x1; x++) fb[y * FXV_SCREEN_W + x] = color;
}

static void fb_fill_rect(u8 *fb, int y, int h, int x0, int x1, u8 color)
{
    for (int i = 0; i < h; i++) fb_hline(fb, y + i, x0, x1, color);
}

static void draw_scrollbar(u8 *fb, u32 len, u32 visible, u32 scroll)
{
    if (visible >= len) return;
    u32 thumb_w = (visible * FXV_SCREEN_W) / len;
    if (thumb_w < 8)            thumb_w = 8;
    if (thumb_w > FXV_SCREEN_W) thumb_w = FXV_SCREEN_W;
    u32 track = FXV_SCREEN_W - thumb_w;
    u32 thumb_x = track > 0 ? (scroll * track) / (len - visible) : 0;

    /* Track: dim horizontal stripe. */
    fb_fill_rect(fb, FXV_SCROLLBAR_Y, FXV_SCROLLBAR_HEIGHT,
                 0, FXV_SCREEN_W, PAL_HEADER_BG);
    /* Thumb: brighter fill showing the current window. */
    fb_fill_rect(fb, FXV_SCROLLBAR_Y + 1, FXV_SCROLLBAR_HEIGHT - 2,
                 (int)thumb_x, (int)(thumb_x + thumb_w), PAL_ORANGE);
}

static void draw_waveform(u8 *fb)
{
    font_clear(fb, PAL_BG);

    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(fb, 0, 0, PAL_TEXT, "LFE FX: %s", cur_fx()->name);
    font_puts(fb, font_scale_col(48), 0,
              "[PARAM]", PAL_ORANGE);

    lfe_buffer buf;
    bool have = wv_get_draft_buffer(&buf) && buf.length > 0;

    u32 len     = have ? buf.length : 0;
    u32 visible = have ? visible_samples(len) : 0;
    u32 scroll  = have ? effective_scroll(len, visible) : 0;

    WaveformRenderCfg cfg = {
        .fb               = fb,
        .y_top            = FXV_WAVE_Y_TOP,
        .y_height         = FXV_WAVE_Y_HEIGHT,
        .pcm              = have ? buf.data : NULL,
        .is_16bit         = true,
        .length           = len,
        .visible          = visible,
        .scroll           = scroll,
        .color_bg         = PAL_BG,
        .color_center     = PAL_DIM,
        .color_wave       = PAL_DIM,
        .sel_start        = fxv.range_start,
        .sel_end          = fxv.range_end,
        .color_sel_in     = PAL_NOTE,
        .color_sel_marker = PAL_ORANGE,
    };
    waveform_render(&cfg);

    if (!have) {
        font_puts(fb, 8, 14, "No draft loaded", PAL_DIM);
        return;
    }

    if (visible < len)
        draw_scrollbar(fb, len, visible, scroll);

    /* Status rows moved from 27/28 to 29/30 to clear the scrollbar
     * band at y=158..168. */
    int z = fxv.vis_zoom;
    font_printf(fb, 0, font_scale_row(29), PAL_GRAY,
                "Range:%04X-%04X  Len:%04X  Zoom:%s",
                fxv.range_start, fxv.range_end,
                fxv.range_end - fxv.range_start,
                (z == 0) ? "FIT" : "ZOOM");
    if (fxv.status_timer > 0)
        font_puts(fb, 0, font_scale_row(30), fxv.status, PAL_PLAY);
}

static void draw_params(u8 *fb)
{
    font_clear(fb, PAL_BG);

    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(fb, 0, 0, PAL_TEXT, "< %s >  L/R:switch  X:apply",
                cur_fx()->name);

    cur_fx()->draw(fb, 2, fxv.param_row);

    int help = 2 + cur_fx()->param_count + 2;
    font_puts(fb, 1, help++, "A+L/R:adj  A+U/D:big adj", PAL_DIM);
    font_puts(fb, 1, help++, "X:apply  Touch:range",      PAL_DIM);
    font_puts(fb, 1, help++, "Y:zoom  SEL+A:commit",     PAL_DIM);

    int foot = font_scale_row(30);
    int tport = font_scale_row(31);
    font_fill_row(fb, foot, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, foot, "SEL+L:back  SEL+A:commit  SEL+B:restore", PAL_DIM);
    font_fill_row(fb, tport, 0, FONT_COLS, PAL_HEADER_BG);
    u32 len = wv_get_draft_length();
    u32 rate = wv_get_draft_rate();
    font_printf(fb, 0, tport, PAL_DIM, "Draft:%lu smp  %luHz",
                (unsigned long)len, (unsigned long)rate);
}

void lfe_fx_view_draw(u8 *top_fb, u8 *bot_fb)
{
    /* Params on the top screen, visualization on the bottom (touch)
     * screen — the touchscreen owns range selection and scroll drags. */
    fxv_clamp_to_draft();
    draw_params(top_fb);
    draw_waveform(bot_fb);
}

#endif /* MAXTRACKER_LFE */
