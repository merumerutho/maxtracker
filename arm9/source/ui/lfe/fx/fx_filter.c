/*
 * fx_filter.c — Filter effect (LP/HP/BP/Notch) for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef MAXTRACKER_LFE

#include "fx_filter.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define FILT_PARAMS 4   /* mode, cutoff, Q, mix */

static const char *filt_mode_names[] = { "LP", "HP", "BP", "NOTCH" };

static struct {
    int mode;    /* lfe_drum_filter_mode */
    int cutoff;  /* Hz, 20..16000 */
    int q;       /* 0..127 */
    int mix;     /* 0..127 */
} st;

static void fx_filter_reset(void)
{
    st.mode   = LFE_DRUM_FILTER_LP;
    st.cutoff = 4000;
    st.q      = 64;
    st.mix    = 127;
}

static void fx_filter_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    lfe_fx_filter_params p = {
        .mode      = (lfe_drum_filter_mode)st.mode,
        .cutoff_hz = (uint32_t)st.cutoff,
        .q         = wv_fx_to_q15(st.q),
        .mix       = wv_fx_to_q15(st.mix),
    };
    wv_fx_report_apply("FILTER", lfe_fx_filter(buf, range, &p), range);
}

static void fx_filter_adjust(int row, int d, int big)
{
    switch (row) {
    case 0: st.mode   = (st.mode + d + 4) % 4;                 break;
    case 1: st.cutoff = wv_fx_clamp_i(st.cutoff + big * 10, 20, 16000); break;
    case 2: st.q      = wv_fx_clamp_i(st.q + big,          0,   127); break;
    case 3: st.mix    = wv_fx_clamp_i(st.mix + big,        0,   127); break;
    }
}

static void fx_filter_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_row(fb, &row, cursor, 0, "Mode      %s", filt_mode_names[st.mode]);
    wv_fx_draw_row(fb, &row, cursor, 1, "Cutoff    %5dHz", st.cutoff);
    wv_fx_draw_fader_row(fb, &row, cursor, 2,
                         st.q, 127, PAL_EFFECT,
                         "Q         %3d", st.q);
    wv_fx_draw_fader_row(fb, &row, cursor, 3,
                         st.mix, 127, PAL_PLAY,
                         "Mix       %3d", st.mix);
}

const wv_fx wv_fx_filter = {
    .name        = "FILTER",
    .param_count = FILT_PARAMS,
    .on_reset    = fx_filter_reset,
    .apply       = fx_filter_apply,
    .adjust      = fx_filter_adjust,
    .draw        = fx_filter_draw,
};

#endif /* MAXTRACKER_LFE */
