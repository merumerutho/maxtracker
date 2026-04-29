/*
 * fx_bitcrush.c — BITCRUSH effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bit depth reduction + virtual sample rate reduction + optional TPDF
 * dither. Three knobs (bits, rate divisor, mix) plus a dither toggle.
 */

#ifdef MAXTRACKER_LFE

#include "fx_bitcrush.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define CRUSH_PARAMS 4

static struct {
    int bits;      /* 1..15 */
    int rate_div;  /* 1..64 */
    int dither;    /* 0 or 1 */
    int mix;       /* 0..127 */
} st;

static void fx_bitcrush_reset(void)
{
    st.bits     = 8;
    st.rate_div = 1;
    st.dither   = 1;
    st.mix      = 127;
}

static void fx_bitcrush_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    lfe_fx_bitcrush_params p = {
        .bit_depth = (uint8_t)st.bits,
        .rate_div  = (uint8_t)st.rate_div,
        .dither    = (uint8_t)st.dither,
        .mix       = wv_fx_to_q15(st.mix),
    };
    wv_fx_report_apply("BITCRUSH", lfe_fx_bitcrush(buf, range, &p), range);
}

static void fx_bitcrush_adjust(int row, int d, int big)
{
    switch (row) {
    case 0: st.bits     = wv_fx_clamp_i(st.bits + big,     1, 15); break;
    case 1: st.rate_div = wv_fx_clamp_i(st.rate_div + big, 1, 64); break;
    case 2: st.dither   = (st.dither + d + 2) % 2;                 break;
    case 3: st.mix      = wv_fx_clamp_i(st.mix + big,      0, 127); break;
    }
}

static const char *dither_names[] = { "OFF", "TPDF" };

static void fx_bitcrush_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_fader_row(fb, &row, cursor, 0,
                         st.bits, 15, PAL_RED,
                         "Bits      %3d", st.bits);
    wv_fx_draw_fader_row(fb, &row, cursor, 1,
                         st.rate_div, 64, PAL_ORANGE,
                         "Rate /    %3d", st.rate_div);
    wv_fx_draw_row(fb, &row, cursor, 2,
                   "Dither    %s", dither_names[st.dither]);
    wv_fx_draw_fader_row(fb, &row, cursor, 3,
                         st.mix, 127, PAL_PLAY,
                         "Mix       %3d", st.mix);
}

const wv_fx wv_fx_bitcrush = {
    .name        = "BITCRUSH",
    .param_count = CRUSH_PARAMS,
    .on_reset    = fx_bitcrush_reset,
    .apply       = fx_bitcrush_apply,
    .adjust      = fx_bitcrush_adjust,
    .draw        = fx_bitcrush_draw,
};

#endif /* MAXTRACKER_LFE */
