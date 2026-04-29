/*
 * tab_drawn.c — Draw Waveform tab for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Touchscreen-driven single-cycle waveform editor. The user drags the
 * stylus across a 256-pixel canvas to shape one cycle; X/A commits the
 * result as a looped sample. The bottom row of the bottom screen has
 * preset buttons (SIN/SAW/SQR/NOS/TRI) that preload a built-in shape.
 *
 * Unique among the tabs in that it owns BOTH screens: `draw_top` shows
 * help text instead of the shared scope, and `draw_bot` renders the
 * canvas + preset footer instead of a parameter panel.
 *
 * on_open seeds the canvas from the current sample slot so the user
 * can touch up an existing waveform rather than always starting blank.
 */

#ifdef MAXTRACKER_LFE

#include "tab_drawn.h"

#include "wv_internal.h"
#include "wv_common.h"

#include "font.h"
#include "screen.h"
#include "draw_util.h"
#include "debug_view.h"
#include "song.h"

#include "lfe.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Canvas geometry (bottom-screen pixel coordinates). */
#define CANVAS_Y_TOP      16
#define CANVAS_Y_BOT      176
#define CANVAS_Y_HEIGHT   (CANVAS_Y_BOT - CANVAS_Y_TOP)
#define CANVAS_Y_CENTER   ((CANVAS_Y_TOP + CANVAS_Y_BOT) / 2)

/* File-scoped state (Step 5 encapsulation). `buf` holds the 256-point
 * single-cycle waveform the user edits; `prev_x` is the previous
 * stylus x for line-interpolated drawing (negative = pen up). */
static struct {
    int8_t buf[WV_CANVAS_LEN];
    bool   dirty;
    int    prev_x;
} dr = {
    .prev_x = -1,
};

static int8_t canvas_y_to_sample(int y)
{
    if (y < CANVAS_Y_TOP)   y = CANVAS_Y_TOP;
    if (y >= CANVAS_Y_BOT)  y = CANVAS_Y_BOT - 1;
    int val = 127 - ((y - CANVAS_Y_TOP) * 255) / (CANVAS_Y_HEIGHT - 1);
    if (val >  127) val =  127;
    if (val < -128) val = -128;
    return (int8_t)val;
}

static int sample_to_canvas_y(int8_t val)
{
    int y = CANVAS_Y_CENTER - (val * (CANVAS_Y_HEIGHT / 2)) / 128;
    if (y < CANVAS_Y_TOP)   y = CANVAS_Y_TOP;
    if (y >= CANVAS_Y_BOT)  y = CANVAS_Y_BOT - 1;
    return y;
}

/* Linear-interpolated stylus drawing across the canvas. Connects the
 * previous touch point to the current one so quick stylus motions
 * still produce a continuous waveform line. */
static void canvas_touch(int tx, int ty)
{
    int8_t cur_val = canvas_y_to_sample(ty);

    if (dr.prev_x < 0) {
        if (tx >= 0 && tx < WV_CANVAS_LEN)
            dr.buf[tx] = cur_val;
        dr.prev_x = tx;
        dr.dirty  = true;
        return;
    }

    int x0 = dr.prev_x;
    int x1 = tx;
    int8_t v0 = dr.buf[x0 < WV_CANVAS_LEN ? x0 : WV_CANVAS_LEN - 1];
    int8_t v1 = cur_val;

    int dx    = x1 - x0;
    int step  = (dx > 0) ? 1 : -1;
    int absdx = (dx > 0) ? dx : -dx;

    if (absdx == 0) {
        if (x0 >= 0 && x0 < WV_CANVAS_LEN)
            dr.buf[x0] = v1;
    } else {
        for (int i = 0; i <= absdx; i++) {
            int x = x0 + i * step;
            if (x >= 0 && x < WV_CANVAS_LEN) {
                int8_t v = (int8_t)(v0 + ((int)(v1 - v0) * i) / absdx);
                dr.buf[x] = v;
            }
        }
    }

    dr.prev_x = tx;
    dr.dirty  = true;
}

/* Footer preset buttons — pixel x ranges match the legacy maxtracker
 * layout (labels at char columns 1 / 9 / 17 / 25 / 33). */
static void check_preset_touch(int tx, int ty)
{
    if (ty < 177) return;

    lfe_drawn_preset preset;
    if      (tx >= 4   && tx <= 31)  preset = LFE_DRAWN_PRESET_SINE;
    else if (tx >= 36  && tx <= 63)  preset = LFE_DRAWN_PRESET_SAW;
    else if (tx >= 68  && tx <= 95)  preset = LFE_DRAWN_PRESET_SQUARE;
    else if (tx >= 100 && tx <= 127) preset = LFE_DRAWN_PRESET_NOISE;
    else if (tx >= 132 && tx <= 159) preset = LFE_DRAWN_PRESET_TRIANGLE;
    else return;

    lfe_drawn_fill_preset(dr.buf, WV_CANVAS_LEN, preset);
    dr.dirty = true;
}

/* Seed the canvas from an existing sample in the current slot via
 * nearest-neighbor resampling into the fixed 256-point buffer. 16-bit
 * PCM is quantized by taking the high byte. Called from on_open so
 * touching up an existing sample doesn't require drawing from blank. */
static void drawn_preload_canvas(void)
{
    memset(dr.buf, 0, sizeof(dr.buf));

    int idx = wv.sample_idx;
    if (idx < 0 || idx >= MT_MAX_SAMPLES) return;
    MT_Sample *s = &song.samples[idx];
    if (!s->active || !s->pcm_data || s->length == 0) return;

    for (uint32_t i = 0; i < WV_CANVAS_LEN; i++) {
        uint32_t src_idx = (i * s->length) / WV_CANVAS_LEN;
        if (s->bits == 8) {
            dr.buf[i] = (int8_t)((int8_t *)s->pcm_data)[src_idx];
        } else if (s->bits == 16) {
            int16_t v = ((int16_t *)s->pcm_data)[src_idx];
            dr.buf[i] = (int8_t)(v >> 8);
        }
    }
}

static void drawn_on_open(void)
{
    drawn_preload_canvas();
    dr.dirty  = false;
    dr.prev_x = -1;
}

static void drawn_generate(void)
{
    dbg_log("drawn_generate");

    int16_t *pcm = (int16_t *)malloc(WV_CANVAS_LEN * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("drawn: malloc failed");
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer out = {
        .data   = pcm,
        .length = WV_CANVAS_LEN,
        .rate   = LFE_RATE_32000,
    };
    lfe_drawn_params p = {
        .canvas        = dr.buf,
        .canvas_length = WV_CANVAS_LEN,
    };

    lfe_status rc = lfe_gen_drawn(&out, &p);
    if (rc != LFE_OK) {
        free(pcm);
        dbg_set_last_error("drawn: gen rc=%d", (int)rc);
        snprintf(wv.status, sizeof(wv.status),
                 "Generate failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    /* 256-point loop @ 8363 Hz — matches legacy maxtracker behavior. */
    wv_write_draft_16(pcm, WV_CANVAS_LEN, 8363u, WV_CANVAS_LEN, 1, "Drawn");

    snprintf(wv.status, sizeof(wv.status), "Drawn waveform committed");
    wv.status_timer = 180;
    dr.dirty = false;
}

static void drawn_input(u32 down, u32 held)
{
    if (held & KEY_TOUCH) {
        int tx, ty;
        touch_read_pixel(&tx, &ty);

        if (ty >= CANVAS_Y_TOP && ty < CANVAS_Y_BOT &&
            tx >= 0 && tx < WV_CANVAS_LEN) {
            canvas_touch(tx, ty);
        } else if (down & KEY_TOUCH) {
            /* Preset tap — only on initial touch-down, not while held. */
            check_preset_touch(tx, ty);
        }
    } else {
        dr.prev_x = -1;
    }

    /* Both A and X commit the drawn waveform — A for legacy muscle
     * memory, X for consistency with the other tabs. */
    if ((down & KEY_A) || (down & KEY_X)) {
        if (dr.dirty) {
            drawn_generate();
        } else {
            dbg_set_last_error("drawn: canvas empty, nothing to commit");
            snprintf(wv.status, sizeof(wv.status),
                     "Canvas is empty — draw something first");
            wv.status_timer = 120;
        }
    }
}

static void drawn_draw_top(u8 *top_fb)
{
    font_puts(top_fb, 1, 4, "Generator: Draw Waveform", PAL_DIM);

    font_puts(top_fb, 1, 7,
              "Use the stylus on the bottom screen to draw",
              PAL_GRAY);
    font_puts(top_fb, 1, 8,
              "one cycle of a waveform.", PAL_GRAY);

    font_puts(top_fb, 1, 11,
              "Tap a preset button (bottom row) to start",
              PAL_GRAY);
    font_puts(top_fb, 1, 12,
              "from a built-in shape.", PAL_GRAY);

    font_puts(top_fb, 1, 15,
              "256-point loop @ 8363 Hz fundamental",
              PAL_DIM);
    font_puts(top_fb, 1, 16,
              "(playback rate sets the pitch when triggered)",
              PAL_DIM);

    font_puts(top_fb, 0, font_scale_row(25),
              "Stylus: draw on bottom canvas",
              PAL_DIM);
    font_puts(top_fb, 0, font_scale_row(26),
              "A or START: commit drawn waveform",
              PAL_WHITE);
}

static void drawn_draw_bot(u8 *bot_fb)
{
    font_fill_row(bot_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(bot_fb, 0, 0, PAL_TEXT,
                "DRAW SAMPLE %02X", wv.sample_idx + 1);

    font_fill_row(bot_fb, 1, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 0, 1, "Drag the stylus across the canvas",
              PAL_DIM);

    /* Center reference line. */
    for (int x = 0; x < WV_CANVAS_LEN; x++)
        plot_pixel(bot_fb, x, CANVAS_Y_CENTER, PAL_DIM);

    /* Render the canvas as a vertical-fill bar chart. */
    for (int x = 0; x < WV_CANVAS_LEN; x++) {
        int y = sample_to_canvas_y(dr.buf[x]);

        int y0 = CANVAS_Y_CENTER;
        int y1 = y;
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        for (int yy = y0; yy <= y1; yy++)
            plot_pixel(bot_fb, x, yy, PAL_PLAY);

        plot_pixel(bot_fb, x, y, PAL_WHITE);
    }

    int foot_row = font_scale_row(30);
    int tport_row = font_scale_row(31);
    font_fill_row(bot_fb, foot_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 1,                  foot_row, "SIN", PAL_NOTE);
    font_puts(bot_fb, font_scale_col(9),  foot_row, "SAW", PAL_NOTE);
    font_puts(bot_fb, font_scale_col(17), foot_row, "SQR", PAL_NOTE);
    font_puts(bot_fb, font_scale_col(25), foot_row, "NOS", PAL_NOTE);
    font_puts(bot_fb, font_scale_col(33), foot_row, "TRI", PAL_NOTE);

    font_fill_row(bot_fb, tport_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 0, tport_row, "A/X:draft  SEL+A:commit  Stylus:draw",
              PAL_DIM);
    if (dr.dirty)
        font_puts(bot_fb, font_scale_col(40), tport_row,
                  "[modified]", PAL_ORANGE);
}

const wv_tab wv_tab_drawn = {
    .name     = "Draw",
    .on_open  = drawn_on_open,
    .input    = drawn_input,
    .draw_top = drawn_draw_top,
    .draw_bot = drawn_draw_bot,
};

#endif /* MAXTRACKER_LFE */
