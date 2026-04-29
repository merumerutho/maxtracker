/*
 * fx_ott.c — OTT 3-band multiband compressor for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin wrapper over the library's lfe_fx_ott. 13 knobs: depth, time,
 * in/out gain, per-band downward/upward/makeup. All knobs are 0..127
 * and mapped linearly to Q15 via wv_fx_to_q15 — consistent with other
 * FX tabs. Upward and downward each have their own per-band knob so
 * users can e.g. run bass-heavy downward compression while leaving
 * mids and highs unexpanded.
 */

#ifdef MAXTRACKER_LFE

#include "fx_ott.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#include <stdio.h>

#define OTT_PARAMS 13

static struct {
    int depth;       /* 0..127 */
    int time;        /* 0..127 */
    int in_gain;     /* 0..127 */
    int out_gain;    /* 0..127 */
    int down_low, down_mid, down_high;   /* 0..127 */
    int up_low,   up_mid,   up_high;     /* 0..127 */
    int gain_low, gain_mid, gain_high;   /* 0..127 */
} st;

static void fx_ott_reset(void)
{
    st.depth     = 64;
    st.time      = 64;
    st.in_gain   = 127;
    st.out_gain  = 127;
    st.down_low  = st.down_mid  = st.down_high  = 64;
    st.up_low    = st.up_mid    = st.up_high    = 64;
    st.gain_low  = st.gain_mid  = st.gain_high  = 127;
}

static void fx_ott_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    lfe_fx_ott_params p = {
        .depth     = wv_fx_to_q15(st.depth),
        .time      = wv_fx_to_q15(st.time),
        .in_gain   = wv_fx_to_q15(st.in_gain),
        .out_gain  = wv_fx_to_q15(st.out_gain),
        .down_low  = wv_fx_to_q15(st.down_low),
        .down_mid  = wv_fx_to_q15(st.down_mid),
        .down_high = wv_fx_to_q15(st.down_high),
        .up_low    = wv_fx_to_q15(st.up_low),
        .up_mid    = wv_fx_to_q15(st.up_mid),
        .up_high   = wv_fx_to_q15(st.up_high),
        .gain_low  = wv_fx_to_q15(st.gain_low),
        .gain_mid  = wv_fx_to_q15(st.gain_mid),
        .gain_high = wv_fx_to_q15(st.gain_high),
    };
    wv_fx_report_apply("OTT", lfe_fx_ott(buf, range, &p), range);
}

static void fx_ott_adjust(int row, int d, int big)
{
    (void)d;
    int *knob;
    switch (row) {
    case  0: knob = &st.depth;     break;
    case  1: knob = &st.time;      break;
    case  2: knob = &st.in_gain;   break;
    case  3: knob = &st.out_gain;  break;
    case  4: knob = &st.down_low;  break;
    case  5: knob = &st.down_mid;  break;
    case  6: knob = &st.down_high; break;
    case  7: knob = &st.up_low;    break;
    case  8: knob = &st.up_mid;    break;
    case  9: knob = &st.up_high;   break;
    case 10: knob = &st.gain_low;  break;
    case 11: knob = &st.gain_mid;  break;
    case 12: knob = &st.gain_high; break;
    default: return;
    }
    *knob = wv_fx_clamp_i(*knob + big, 0, 127);
}

static void fx_ott_draw(u8 *fb, int row, int cursor)
{
    wv_fx_draw_fader_row(fb, &row, cursor,  0,
                         st.depth, 127, PAL_PARAM,
                         "Depth     %3d", st.depth);
    wv_fx_draw_fader_row(fb, &row, cursor,  1,
                         st.time, 127, PAL_ORANGE,
                         "Time      %3d", st.time);
    wv_fx_draw_fader_row(fb, &row, cursor,  2,
                         st.in_gain, 127, PAL_PLAY,
                         "In Gain   %3d", st.in_gain);
    wv_fx_draw_fader_row(fb, &row, cursor,  3,
                         st.out_gain, 127, PAL_PLAY,
                         "Out Gain  %3d", st.out_gain);
    wv_fx_draw_fader_row(fb, &row, cursor,  4,
                         st.down_low, 127, PAL_EFFECT,
                         "Down Low  %3d", st.down_low);
    wv_fx_draw_fader_row(fb, &row, cursor,  5,
                         st.down_mid, 127, PAL_EFFECT,
                         "Down Mid  %3d", st.down_mid);
    wv_fx_draw_fader_row(fb, &row, cursor,  6,
                         st.down_high, 127, PAL_EFFECT,
                         "Down High %3d", st.down_high);
    wv_fx_draw_fader_row(fb, &row, cursor,  7,
                         st.up_low, 127, PAL_NOTE,
                         "Up Low    %3d", st.up_low);
    wv_fx_draw_fader_row(fb, &row, cursor,  8,
                         st.up_mid, 127, PAL_NOTE,
                         "Up Mid    %3d", st.up_mid);
    wv_fx_draw_fader_row(fb, &row, cursor,  9,
                         st.up_high, 127, PAL_NOTE,
                         "Up High   %3d", st.up_high);
    wv_fx_draw_fader_row(fb, &row, cursor, 10,
                         st.gain_low, 127, PAL_PLAY,
                         "Gain Low  %3d", st.gain_low);
    wv_fx_draw_fader_row(fb, &row, cursor, 11,
                         st.gain_mid, 127, PAL_PLAY,
                         "Gain Mid  %3d", st.gain_mid);
    wv_fx_draw_fader_row(fb, &row, cursor, 12,
                         st.gain_high, 127, PAL_PLAY,
                         "Gain High %3d", st.gain_high);
}

const wv_fx wv_fx_ott = {
    .name        = "OTT",
    .param_count = OTT_PARAMS,
    .on_reset    = fx_ott_reset,
    .apply       = fx_ott_apply,
    .adjust      = fx_ott_adjust,
    .draw        = fx_ott_draw,
};

#endif /* MAXTRACKER_LFE */
