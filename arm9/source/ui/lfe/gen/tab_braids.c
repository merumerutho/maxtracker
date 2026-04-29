/*
 * tab_braids.c — Braids (18 macro-shape) generator tab.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of Mutable Instruments Braids' 18 macro shapes. Each shape is
 * controlled by shape + pitch + Timbre + Color, same mapping as the
 * original hardware. See project_phase6_braids_plan.md and
 * reference_lfe_braids.md for the port rationale.
 */

#ifdef MAXTRACKER_LFE

#include "tab_braids.h"

#include "wv_internal.h"
#include "wv_common.h"

#include "font.h"
#include "screen.h"
#include "debug_view.h"

#include "lfe.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    BRAIDS_PARAM_SHAPE = 0,
    BRAIDS_PARAM_PITCH_HZ,
    BRAIDS_PARAM_TIMBRE,
    BRAIDS_PARAM_COLOR,
    BRAIDS_PARAM_LENGTH_MS,
    BRAIDS_PARAM_COUNT,
} BraidsParam;

static const char *braids_param_labels[BRAIDS_PARAM_COUNT] = {
    "Shape",
    "Pitch (Hz)",
    "Timbre",
    "Color",
    "Length (ms)",
};

/* Short labels for the 18 shapes — order locked to lfe_braids_shape
 * in include/lfe.h. */
#define BRAIDS_SHAPE_COUNT LFE_BRAIDS_SHAPE_COUNT
static const char *braids_shape_names[BRAIDS_SHAPE_COUNT] = {
    "CSAW", "MORPH", "SAW/SQ", "SIN/TRI",
    "3xSAW", "3xSQ", "3xTRI", "3xSIN",
    "3xRING", "SWARM", "SAW/COMB",
    "VOWEL", "PLUCK", "BOWED",
    "BLOWN", "FLUTE", "TWIN_PK", "QPSK",
};

/* File-scoped state (Step 5 encapsulation). Timbre and color are Q15
 * in [0, 32767]; displayed as a percent. */
static struct {
    int param_row;
    int shape;
    int pitch_hz;
    int timbre;
    int color;
} br = {
    .pitch_hz  = 220,    /* A3 */
    .timbre    = 16384,  /* mid Q15 */
    .color     = 16384,
};

static void braids_generate(void)
{
    dbg_log("braids_generate shape=%d hz=%d t=%d c=%d dur=%d",
            br.shape, br.pitch_hz,
            br.timbre, br.color, wv.length_ms);

    const uint32_t rate   = 32000u;
    const uint32_t length = (uint32_t)wv.length_ms * rate / 1000u;
    if (length == 0) {
        snprintf(wv.status, sizeof(wv.status), "Duration too short");
        wv.status_timer = 180;
        return;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)length * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("braids: malloc %lu failed", (unsigned long)length);
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer outbuf = { .data = pcm, .length = length, .rate = LFE_RATE_32000 };
    lfe_braids_params p = {
        .shape       = (lfe_braids_shape)br.shape,
        .pitch_hz_q8 = (uint32_t)br.pitch_hz << 8,
        .timbre      = (uint16_t)br.timbre,
        .color       = (uint16_t)br.color,
        .seed        = 0,
    };

    lfe_status rc = lfe_gen_braids(&outbuf, &p);
    if (rc < 0) {
        free(pcm);
        snprintf(wv.status, sizeof(wv.status),
                 "Braids gen failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    wv_write_draft_16(pcm, length, rate, 0, 0, "Braids");
}

static void braids_param_adjust(int param, int delta)
{
    switch (param) {
    case BRAIDS_PARAM_SHAPE:
        br.shape += delta;
        while (br.shape < 0)                   br.shape += BRAIDS_SHAPE_COUNT;
        while (br.shape >= BRAIDS_SHAPE_COUNT) br.shape -= BRAIDS_SHAPE_COUNT;
        break;
    case BRAIDS_PARAM_PITCH_HZ:
        br.pitch_hz += delta;
        if (br.pitch_hz <    20) br.pitch_hz =   20;
        if (br.pitch_hz >  4000) br.pitch_hz = 4000;
        break;
    case BRAIDS_PARAM_TIMBRE:
        /* Q15 in 256-step increments — one A+L/R nudge ≈ 0.78%. */
        br.timbre += delta * 256;
        if (br.timbre < 0)     br.timbre = 0;
        if (br.timbre > 32767) br.timbre = 32767;
        break;
    case BRAIDS_PARAM_COLOR:
        br.color += delta * 256;
        if (br.color < 0)     br.color = 0;
        if (br.color > 32767) br.color = 32767;
        break;
    case BRAIDS_PARAM_LENGTH_MS:
        wv.length_ms += delta * 50;
        if (wv.length_ms <  100) wv.length_ms =  100;
        if (wv.length_ms > 4000) wv.length_ms = 4000;
        break;
    default:
        break;
    }
}

static void braids_input(u32 down, u32 held)
{
    if (down & KEY_X) {
        braids_generate();
        return;
    }

    if (!(held & KEY_A)) {
        if (down & KEY_UP) {
            if (br.param_row > 0) br.param_row--;
            else                          br.param_row = BRAIDS_PARAM_COUNT - 1;
        }
        if (down & KEY_DOWN) {
            if (br.param_row < BRAIDS_PARAM_COUNT - 1) br.param_row++;
            else                                               br.param_row = 0;
        }
    } else {
        u32 rep = keysDownRepeat();
        if (rep & KEY_LEFT)  braids_param_adjust(br.param_row,  -1);
        if (rep & KEY_RIGHT) braids_param_adjust(br.param_row,  +1);
        if (rep & KEY_DOWN)  braids_param_adjust(br.param_row, -10);
        if (rep & KEY_UP)    braids_param_adjust(br.param_row, +10);
    }
}

static void braids_draw(u8 *top_fb)
{
    font_puts(top_fb, 1, 4, "Generator: Braids (18 macro shapes)", PAL_DIM);

    int row_y = 7;
    for (int i = 0; i < BRAIDS_PARAM_COUNT; i++) {
        u8 color = (i == br.param_row) ? PAL_WHITE : PAL_GRAY;
        const char *marker = (i == br.param_row) ? ">" : " ";
        font_printf(top_fb, 1, row_y + i * 2, color, "%s %s",
                    marker, braids_param_labels[i]);

        char val[32];
        switch (i) {
        case BRAIDS_PARAM_SHAPE:
            snprintf(val, sizeof(val), "%s", braids_shape_names[br.shape]);
            break;
        case BRAIDS_PARAM_PITCH_HZ:
            snprintf(val, sizeof(val), "%4d", br.pitch_hz);
            break;
        case BRAIDS_PARAM_TIMBRE:
            snprintf(val, sizeof(val), "%3d%%", (br.timbre * 100) / 32767);
            break;
        case BRAIDS_PARAM_COLOR:
            snprintf(val, sizeof(val), "%3d%%", (br.color * 100) / 32767);
            break;
        case BRAIDS_PARAM_LENGTH_MS:
            wv_format_length_with_size(val, sizeof(val), wv.length_ms, 32000u);
            break;
        }
        font_puts(top_fb, font_scale_col(30), row_y + i * 2, val, color);
    }

    font_puts(top_fb, 1, 18,
              "18 shapes from MI Braids: triples,",
              PAL_DIM);
    font_puts(top_fb, 1, 19,
              "vowels, plucked/bowed/blown, modems.",
              PAL_DIM);

    font_puts(top_fb, 0, font_scale_row(25),
              "UP/DN:row  A+L/R:-+1  A+UP/DN:+-10",
              PAL_DIM);
    font_puts(top_fb, 0, font_scale_row(26),
              "X: generate into draft",
              PAL_WHITE);
}

const wv_tab wv_tab_braids = {
    .name        = "BRAIDS",
    .input       = braids_input,
    .draw_params = braids_draw,
};

#endif /* MAXTRACKER_LFE */
