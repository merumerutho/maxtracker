/*
 * fx_distortion.c — Distortion effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four flavors: HARD (clip), SOFT (tanh-ish), FOLD (wrap), CRUSH
 * (bit-depth reduction, 1..15 bits). The underlying DSP lives in the
 * lfe library; this module only owns the UI state and formatting.
 */

#ifdef MAXTRACKER_LFE

#include "fx_distortion.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define DIST_PARAMS 4   /* mode, drive, threshold/bits, mix */

static const char *dist_mode_names[] = { "HARD", "SOFT", "FOLD", "CRUSH" };

static struct {
    int mode;        /* lfe_fx_distortion_mode */
    int drive;       /* 0..127 */
    int threshold;   /* 0..127 — or reused as bit depth source for CRUSH */
    int mix;         /* 0..127 */
} st;

static void fx_distortion_reset(void)
{
    st.mode      = LFE_FX_DIST_HARD;
    st.drive     = 64;
    st.threshold = 96;
    st.mix       = 127;
}

static void fx_distortion_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    lfe_fx_distortion_params p = {
        .mode      = (lfe_fx_distortion_mode)st.mode,
        .drive     = wv_fx_to_q15(st.drive),
        .threshold = wv_fx_to_q15(st.threshold),
        .mix       = wv_fx_to_q15(st.mix),
        .bit_depth = 8,
    };
    /* BITCRUSH repurposes the "threshold" slider as bit depth (1..15). */
    if (st.mode == LFE_FX_DIST_BITCRUSH)
        p.bit_depth = (st.threshold * 14 / 127) + 1;

    wv_fx_report_apply("DISTORTION", lfe_fx_distort(buf, range, &p), range);
}

static void fx_distortion_adjust(int row, int d, int big)
{
    switch (row) {
    case 0: st.mode      = (st.mode + d + 4) % 4;             break;
    case 1: st.drive     = wv_fx_clamp_i(st.drive + big,     0, 127); break;
    case 2: st.threshold = wv_fx_clamp_i(st.threshold + big, 0, 127); break;
    case 3: st.mix       = wv_fx_clamp_i(st.mix + big,       0, 127); break;
    }
}

static void fx_distortion_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_row(fb, &row, cursor, 0, "Mode      %s", dist_mode_names[st.mode]);
    wv_fx_draw_fader_row(fb, &row, cursor, 1,
                         st.drive, 127, PAL_RED,
                         "Drive     %3d", st.drive);
    if (st.mode == LFE_FX_DIST_BITCRUSH) {
        int bits = (st.threshold * 14 / 127) + 1;
        wv_fx_draw_fader_row(fb, &row, cursor, 2,
                             st.threshold, 127, PAL_RED,
                             "Bits      %3d", bits);
    } else {
        wv_fx_draw_fader_row(fb, &row, cursor, 2,
                             st.threshold, 127, PAL_RED,
                             "Threshold %3d", st.threshold);
    }
    wv_fx_draw_fader_row(fb, &row, cursor, 3,
                         st.mix, 127, PAL_PLAY,
                         "Mix       %3d", st.mix);
}

const wv_fx wv_fx_distortion = {
    .name        = "DISTORTION",
    .param_count = DIST_PARAMS,
    .on_reset    = fx_distortion_reset,
    .apply       = fx_distortion_apply,
    .adjust      = fx_distortion_adjust,
    .draw        = fx_distortion_draw,
};

#endif /* MAXTRACKER_LFE */
