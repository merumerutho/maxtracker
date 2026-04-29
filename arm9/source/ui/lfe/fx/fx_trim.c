/*
 * fx_trim.c — TRIM effect for the LFE FX room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replaces the entire sample with only the selected range, shrinking the
 * buffer. UI-only — no library DSP function needed. Directly reallocates
 * the draft snapshot's PCM buffer.
 */

#ifdef MAXTRACKER_LFE

#include "fx_trim.h"

#include "font.h"
#include "screen.h"
#include "wv_draft.h"

#include "lfe.h"

#include <stdlib.h>
#include <string.h>

#define TRIM_PARAMS 0

static void fx_trim_apply(lfe_buffer *buf, const lfe_fx_range *range)
{
    if (!buf || !buf->data || !range) {
        wv_fx_set_status("TRIM: no data");
        return;
    }

    u32 start = range->start;
    u32 end   = range->end;
    u32 len   = buf->length;

    if (start >= len || end > len || end <= start) {
        wv_fx_set_status("TRIM: bad range");
        return;
    }

    u32 new_len = end - start;
    if (new_len == len) {
        wv_fx_set_status("TRIM: already full range");
        return;
    }

    u32 bps       = (wv_draft.meta.bits == 16) ? 2 : 1;
    u32 new_bytes = new_len * bps;

    u8 *new_pcm = (u8 *)malloc(new_bytes);
    if (!new_pcm) {
        wv_fx_set_status("TRIM: malloc failed");
        return;
    }

    memcpy(new_pcm, wv_draft.pcm + start * bps, new_bytes);

    free(wv_draft.pcm);
    wv_draft.pcm       = new_pcm;
    wv_draft.pcm_bytes = new_bytes;
    wv_draft.meta.length = new_len;

    buf->data   = (lfe_sample_t *)new_pcm;
    buf->length = new_len;

    wv_fx_set_status("TRIM [%04X-%04X] -> %u smp",
                     (unsigned)start, (unsigned)end, (unsigned)new_len);
}

static void fx_trim_adjust(int row, int d, int big)
{
    (void)row; (void)d; (void)big;
}

static void fx_trim_draw(u8 *fb, int row, int cursor)
{
    (void)cursor;
    font_puts(fb, 1, row++, "Crop sample to selection range.", PAL_DIM);
    row++;
    font_puts(fb, 1, row++, "Touch or D-pad to set range,",   PAL_DIM);
    font_puts(fb, 1, row++, "then press X to trim.",           PAL_DIM);
}

const wv_fx wv_fx_trim = {
    .name        = "TRIM",
    .param_count = TRIM_PARAMS,
    .on_reset    = NULL,
    .apply       = fx_trim_apply,
    .adjust      = fx_trim_adjust,
    .draw        = fx_trim_draw,
};

#endif /* MAXTRACKER_LFE */
