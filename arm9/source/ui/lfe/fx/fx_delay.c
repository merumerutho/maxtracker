/*
 * fx_delay.c — Delay effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Needs a scratch buffer sized to the delay line — allocated on every
 * apply() because the time parameter can change between invocations,
 * and keeping a max-size scratch around all the time would waste RAM.
 */

#ifdef MAXTRACKER_LFE

#include "fx_delay.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>
#include <stdlib.h>

#define DELAY_PARAMS 3   /* time, feedback, mix */

static struct {
    int ms;        /* 1..500 */
    int feedback;  /* 0..127 */
    int mix;       /* 0..127 */
} st;

static void fx_delay_reset(void)
{
    st.ms       = 100;
    st.feedback = 48;
    st.mix      = 64;
}

static void fx_delay_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    u32 delay_samples = (u32)st.ms * buf->rate / 1000;
    if (delay_samples == 0) delay_samples = 1;

    lfe_sample_t *scratch = (lfe_sample_t *)malloc(
        delay_samples * sizeof(lfe_sample_t));
    if (!scratch) {
        wv_fx_set_status("Out of memory");
        return;
    }

    lfe_fx_delay_params p = {
        .delay_ms       = (uint32_t)st.ms,
        .feedback       = wv_fx_to_q15(st.feedback),
        .mix            = wv_fx_to_q15(st.mix),
        .scratch        = scratch,
        .scratch_length = delay_samples,
    };
    lfe_status rc = lfe_fx_delay(buf, range, &p);
    free(scratch);
    wv_fx_report_apply("DELAY", rc, range);
}

static void fx_delay_adjust(int row, int d, int big)
{
    (void)d;
    switch (row) {
    case 0: st.ms       = wv_fx_clamp_i(st.ms + big * 5,  1, 500); break;
    case 1: st.feedback = wv_fx_clamp_i(st.feedback + big, 0, 127); break;
    case 2: st.mix      = wv_fx_clamp_i(st.mix + big,      0, 127); break;
    }
}

static void fx_delay_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_fader_row(fb, &row, cursor, 0,
                         st.ms, 500, PAL_ORANGE,
                         "Time      %3dms", st.ms);
    wv_fx_draw_fader_row(fb, &row, cursor, 1,
                         st.feedback, 127, PAL_EFFECT,
                         "Feedback  %3d", st.feedback);
    wv_fx_draw_fader_row(fb, &row, cursor, 2,
                         st.mix, 127, PAL_PLAY,
                         "Mix       %3d", st.mix);
}

const wv_fx wv_fx_delay = {
    .name        = "DELAY",
    .param_count = DELAY_PARAMS,
    .on_reset    = fx_delay_reset,
    .apply       = fx_delay_apply,
    .adjust      = fx_delay_adjust,
    .draw        = fx_delay_draw,
};

#endif /* MAXTRACKER_LFE */
