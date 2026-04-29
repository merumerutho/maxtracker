/*
 * fx_envelope.c — Envelope-shaper effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Applies a preset envelope curve (FADE IN / FADE OUT / EXP decay etc)
 * as an amplitude modulation over the selected range.
 */

#ifdef MAXTRACKER_LFE

#include "fx_envelope.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define ENV_PARAMS        2
#define ENV_PRESET_COUNT  6
#define ENV_CANVAS_LEN    256

static const char *env_preset_names[ENV_PRESET_COUNT] = {
    "FADE IN", "FADE OUT", "EXP DEC", "EXP ATK", "TRIANGLE", "BELL",
};

static struct {
    int preset;  /* lfe_fx_env_preset */
    int mix;     /* 0..127 */
} st;

static void fx_envelope_reset(void)
{
    st.preset = LFE_FX_ENV_PRESET_FADE_IN;
    st.mix    = 127;
}

static void fx_envelope_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    uint16_t canvas[ENV_CANVAS_LEN];
    lfe_fx_env_fill_preset(canvas, ENV_CANVAS_LEN,
                           (lfe_fx_env_preset)st.preset);

    lfe_fx_env_shaper_params p = {
        .canvas        = canvas,
        .canvas_length = ENV_CANVAS_LEN,
        .mix           = wv_fx_to_q15(st.mix),
    };
    wv_fx_report_apply("ENVELOPE", lfe_fx_env_shaper(buf, range, &p), range);
}

static void fx_envelope_adjust(int row, int d, int big)
{
    switch (row) {
    case 0: st.preset = (st.preset + d + ENV_PRESET_COUNT) % ENV_PRESET_COUNT; break;
    case 1: st.mix    = wv_fx_clamp_i(st.mix + big, 0, 127); break;
    }
}

static void fx_envelope_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_row(fb, &row, cursor, 0, "Preset    %s", env_preset_names[st.preset]);
    wv_fx_draw_fader_row(fb, &row, cursor, 1,
                         st.mix, 127, PAL_PLAY,
                         "Mix       %3d", st.mix);
}

const wv_fx wv_fx_envelope = {
    .name        = "ENVELOPE",
    .param_count = ENV_PARAMS,
    .on_reset    = fx_envelope_reset,
    .apply       = fx_envelope_apply,
    .adjust      = fx_envelope_adjust,
    .draw        = fx_envelope_draw,
};

#endif /* MAXTRACKER_LFE */
