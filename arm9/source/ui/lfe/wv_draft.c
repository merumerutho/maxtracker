/*
 * wv_draft.c — see wv_draft.h for the contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef MAXTRACKER_LFE

#include "wv_draft.h"

#include "debug_view.h"

#include <stdlib.h>
#include <string.h>

WaveformSnapshot wv_original;
WaveformSnapshot wv_draft;

u32 wv_sample_pcm_bytes(const MT_Sample *s)
{
    if (!s || !s->pcm_data || s->length == 0) return 0;
    return s->length * (s->bits == 16 ? 2 : 1);
}

void wv_snapshot_free(WaveformSnapshot *snap)
{
    if (!snap) return;
    if (snap->pcm) {
        free(snap->pcm);
        snap->pcm = NULL;
    }
    snap->pcm_bytes = 0;
    snap->valid     = false;
    memset(&snap->meta, 0, sizeof(snap->meta));
}

bool wv_snapshot_capture(WaveformSnapshot *snap, const MT_Sample *src)
{
    if (!snap || !src) return false;

    wv_snapshot_free(snap);

    snap->meta = *src;
    snap->meta.pcm_data = NULL;

    u32 bytes = wv_sample_pcm_bytes(src);
    if (bytes == 0) {
        snap->pcm       = NULL;
        snap->pcm_bytes = 0;
        snap->valid     = true;
        return true;
    }

    snap->pcm = (u8 *)malloc(bytes);
    if (!snap->pcm) {
        dbg_set_last_error("wv snapshot: malloc %lu failed",
                           (unsigned long)bytes);
        snap->valid = false;
        return false;
    }
    memcpy(snap->pcm, src->pcm_data, bytes);
    snap->pcm_bytes = bytes;
    snap->valid     = true;
    return true;
}

bool wv_snapshot_restore(WaveformSnapshot *snap, MT_Sample *dst)
{
    if (!snap || !snap->valid || !dst) return false;

    if (dst->pcm_data) {
        free(dst->pcm_data);
        dst->pcm_data = NULL;
    }

    *dst = snap->meta;
    dst->pcm_data = NULL;

    if (snap->pcm_bytes == 0) return true;

    u8 *copy = (u8 *)malloc(snap->pcm_bytes);
    if (!copy) {
        dbg_set_last_error("wv restore: malloc %lu failed",
                           (unsigned long)snap->pcm_bytes);
        return false;
    }
    memcpy(copy, snap->pcm, snap->pcm_bytes);
    dst->pcm_data = copy;
    return true;
}

#endif /* MAXTRACKER_LFE */
