/*
 * fx_normalize.c — DC-remove + peak-normalize effect for the FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef MAXTRACKER_LFE

#include "fx_normalize.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define NORM_PARAMS 1

static struct {
    int target;  /* 0..127 — % of full scale */
} st;

static void fx_normalize_reset(void)
{
    st.target = 127;
}

static void fx_normalize_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    lfe_fx_normalize_params p = {
        .target_peak = wv_fx_to_q15(st.target),
    };
    wv_fx_report_apply("NORMALIZE", lfe_fx_normalize(buf, range, &p), range);
}

static void fx_normalize_adjust(int row, int d, int big)
{
    (void)d;
    if (row == 0) st.target = wv_fx_clamp_i(st.target + big, 0, 127);
}

static void fx_normalize_draw(u8 *fb, int row, int cursor)
{
    int pct = (st.target * 100) / 127;
    wv_fx_draw_fader_row(fb, &row, cursor, 0,
                         st.target, 127, PAL_PLAY,
                         "Target    %3d%%", pct);
    row++;
    font_puts(fb, 1, row++, "DC removed + peak normalized.", PAL_DIM);
    font_puts(fb, 1, row++, "Target=100%% reaches full scale.", PAL_DIM);
}

const wv_fx wv_fx_normalize = {
    .name        = "NORMALIZE",
    .param_count = NORM_PARAMS,
    .on_reset    = fx_normalize_reset,
    .apply       = fx_normalize_apply,
    .adjust      = fx_normalize_adjust,
    .draw        = fx_normalize_draw,
};

#endif /* MAXTRACKER_LFE */
