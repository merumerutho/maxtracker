/*
 * fx_reverse.c — REVERSE effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reverses sample order within the selection range. No parameters.
 */

#ifdef MAXTRACKER_LFE

#include "fx_reverse.h"

#include "font.h"
#include "screen.h"

#include "lfe.h"

#define REVERSE_PARAMS 0

static void fx_reverse_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    wv_fx_report_apply("REVERSE", lfe_fx_reverse(buf, range), range);
}

static void fx_reverse_adjust(int row, int d, int big)
{
    (void)row; (void)d; (void)big;
}

static void fx_reverse_draw(u8 *fb, int row, int cursor)
{
    (void)cursor;
    font_puts(fb, 1, row++, "Reverse sample order in selection.", PAL_DIM);
    row++;
    font_puts(fb, 1, row++, "Touch to set range,",               PAL_DIM);
    font_puts(fb, 1, row++, "then press X to reverse.",           PAL_DIM);
}

const wv_fx wv_fx_reverse = {
    .name        = "REVERSE",
    .param_count = REVERSE_PARAMS,
    .on_reset    = NULL,
    .apply       = fx_reverse_apply,
    .adjust      = fx_reverse_adjust,
    .draw        = fx_reverse_draw,
};

#endif /* MAXTRACKER_LFE */
