/*
 * tab_tone.c — Test Tone generator tab for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The simplest tab: three scalar parameters (frequency, amplitude,
 * duration) dialed with the standard LSDJ A-held d-pad pattern, and
 * X to render a sine tone of the selected shape into the draft.
 *
 * Follows the tab-module template:
 *   - SYNTH_PARAMS-style X-macro is not used here (only three rows,
 *     hand-written switch is clearer for this size).
 *   - State lives in the shared `wv` struct (Step 5 will move the
 *     tt_* fields into a file-scoped struct here).
 *   - Exports `wv_tab_tone` via tab_tone.h for the dispatcher.
 */

#if defined(MAXTRACKER_LFE) && defined(MAXTRACKER_LFE_ENABLE_TESTTONE)

#include "tab_tone.h"

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
    TT_PARAM_FREQ_HZ = 0,
    TT_PARAM_AMPLITUDE,
    TT_PARAM_DURATION_MS,
    TT_PARAM_COUNT,
} TestToneParam;

static const char *tt_param_labels[TT_PARAM_COUNT] = {
    "Frequency (Hz)",
    "Amplitude",
    "Duration (ms)",
};

/* File-scoped state (Step 5 encapsulation). Amplitude is 0..127 and
 * gets mapped to Q15 at generate time; duration is in ms. */
static struct {
    int param_row;
    int freq_hz;
    int amplitude;
    int duration_ms;
} tt = {
    .freq_hz     = 440,
    .amplitude   = 112,   /* ~0x7000 in Q15 */
    .duration_ms = 1000,
};

static void tt_generate(void)
{
    dbg_log("tt_generate hz=%d amp=%d dur=%d",
            tt.freq_hz, tt.amplitude, tt.duration_ms);

    const uint32_t rate   = 32000u;
    const uint32_t length = (uint32_t)tt.duration_ms * rate / 1000u;
    if (length == 0) {
        dbg_set_last_error("tt: zero length");
        snprintf(wv.status, sizeof(wv.status), "Duration too short");
        wv.status_timer = 180;
        return;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)length * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("tt: malloc failed (%lu bytes)",
                           (unsigned long)(length * sizeof(int16_t)));
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer out = {
        .data   = pcm,
        .length = length,
        .rate   = LFE_RATE_32000,
    };
    lfe_test_tone_params p = {
        .freq_hz_q8    = (uint32_t)tt.freq_hz << 8,
        .amplitude_q15 = (uint16_t)((tt.amplitude * LFE_Q15_ONE) / 127),
    };

    lfe_status rc = lfe_gen_test_tone(&out, &p);
    if (rc != LFE_OK) {
        free(pcm);
        dbg_set_last_error("tt: gen failed rc=%d", (int)rc);
        snprintf(wv.status, sizeof(wv.status),
                 "Generate failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    wv_write_draft_16(pcm, length, rate, length, 1, "Tone");

    snprintf(wv.status, sizeof(wv.status),
             "Generated %d ms @ %d Hz", tt.duration_ms, tt.freq_hz);
    wv.status_timer = 180;
}

static void tt_param_adjust(int param, int delta)
{
    switch (param) {
    case TT_PARAM_FREQ_HZ:
        tt.freq_hz += delta;
        if (tt.freq_hz < 20)    tt.freq_hz = 20;
        if (tt.freq_hz > 15000) tt.freq_hz = 15000;
        break;
    case TT_PARAM_AMPLITUDE:
        tt.amplitude += delta;
        if (tt.amplitude < 0)   tt.amplitude = 0;
        if (tt.amplitude > 127) tt.amplitude = 127;
        break;
    case TT_PARAM_DURATION_MS:
        tt.duration_ms += delta * 10;
        if (tt.duration_ms < 10)   tt.duration_ms = 10;
        if (tt.duration_ms > 5000) tt.duration_ms = 5000;
        break;
    default:
        break;
    }
}

static void tt_input(u32 down, u32 held)
{
    if (down & KEY_X) {
        tt_generate();
        return;
    }

    if (!(held & KEY_A)) {
        if (down & KEY_UP) {
            if (tt.param_row > 0) tt.param_row--;
            else                      tt.param_row = TT_PARAM_COUNT - 1;
        }
        if (down & KEY_DOWN) {
            if (tt.param_row < TT_PARAM_COUNT - 1) tt.param_row++;
            else                                       tt.param_row = 0;
        }
    } else {
        u32 rep = keysDownRepeat();
        if (rep & KEY_LEFT)  tt_param_adjust(tt.param_row,  -1);
        if (rep & KEY_RIGHT) tt_param_adjust(tt.param_row,  +1);
        if (rep & KEY_DOWN)  tt_param_adjust(tt.param_row, -10);
        if (rep & KEY_UP)    tt_param_adjust(tt.param_row, +10);
    }
}

static void tt_draw(u8 *top_fb)
{
    font_puts(top_fb, 1, 4, "Generator: Test Tone", PAL_DIM);

    int row_y = 7;
    for (int i = 0; i < TT_PARAM_COUNT; i++) {
        u8 color = (i == tt.param_row) ? PAL_WHITE : PAL_GRAY;
        const char *marker = (i == tt.param_row) ? ">" : " ";
        font_printf(top_fb, 1, row_y + i * 2, color, "%s %s",
                    marker, tt_param_labels[i]);

        char val[32];
        switch (i) {
        case TT_PARAM_FREQ_HZ:
            snprintf(val, sizeof(val), "%5d", tt.freq_hz);
            break;
        case TT_PARAM_AMPLITUDE:
            snprintf(val, sizeof(val), "%3d / 127", tt.amplitude);
            break;
        case TT_PARAM_DURATION_MS:
            snprintf(val, sizeof(val), "%4d ms", tt.duration_ms);
            break;
        }
        font_puts(top_fb, font_scale_col(30), row_y + i * 2, val, color);
    }

    font_puts(top_fb, 0, font_scale_row(25),
              "UP/DN:row  A+L/R:-+1  A+UP/DN:+-10",
              PAL_DIM);
    font_puts(top_fb, 0, font_scale_row(26),
              "X: generate into draft",
              PAL_WHITE);
}

const wv_tab wv_tab_tone = {
    .name        = "Tone",
    .input       = tt_input,
    .draw_params = tt_draw,
};

#endif /* MAXTRACKER_LFE && MAXTRACKER_LFE_ENABLE_TESTTONE */
