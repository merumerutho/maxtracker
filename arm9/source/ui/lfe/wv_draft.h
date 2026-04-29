/*
 * wv_draft.h — snapshot / draft data layer for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LFE edits are non-destructive by design: the live sample slot
 * (song.samples[idx]) is NEVER touched by a generator run. Instead
 * generators write into `wv_draft`, the top-screen scope shows the
 * draft, and the user explicitly commits via SELECT+A.
 *
 *   wv_original — pristine backup captured ONCE at open, restored on
 *                 SELECT+B. Not affected by commits.
 *   wv_draft    — scratch buffer. Generators rewrite it on every X
 *                 press. Surviving B-restore means work-in-progress
 *                 isn't lost when the user rolls the sample back.
 *
 * This module owns the data types and buffer lifecycle. Action-level
 * glue (status messages, playback rebuild, sample_idx bookkeeping)
 * stays in waveform_view.c and reaches in via the exported globals and
 * snapshot helpers.
 */

#ifndef WV_DRAFT_H
#define WV_DRAFT_H

#ifdef MAXTRACKER_LFE

#include <nds.h>
#include <stdbool.h>
#include <stdint.h>

#include "song.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A deep copy of an MT_Sample. `meta` holds the scalar fields with
 * `meta.pcm_data` intentionally NULL — `pcm` below is the authoritative
 * buffer. Two independent snapshots (original + draft) are the backbone
 * of the non-destructive editing flow.
 */
typedef struct {
    bool      valid;
    MT_Sample meta;
    u8       *pcm;
    u32       pcm_bytes;
} WaveformSnapshot;

/* The two well-known snapshots — defined in wv_draft.c, read/written by
 * waveform_view.c and lfe_fx_view.c via the action glue. */
extern WaveformSnapshot wv_original;
extern WaveformSnapshot wv_draft;

/* Byte size of a sample's PCM given length × bits. Returns 0 if the
 * sample has no PCM or zero length. */
u32 wv_sample_pcm_bytes(const MT_Sample *s);

/* Release any PCM held by the snapshot and mark it invalid. Safe on an
 * uninitialised or already-freed snapshot. */
void wv_snapshot_free(WaveformSnapshot *snap);

/* Deep-copy `src` into `snap`. Allocates a fresh PCM buffer and memcpy
 * into it. Empty slots copy successfully with pcm==NULL + pcm_bytes==0.
 * Returns false on malloc failure (snap left invalid). */
bool wv_snapshot_capture(WaveformSnapshot *snap, const MT_Sample *src);

/* Overwrite `dst` with the snapshot's contents. Frees dst's current
 * pcm_data, allocates a fresh copy of snap's PCM into dst. The snapshot
 * keeps its own buffer, so multiple restores from the same snapshot
 * are supported. Returns false on malloc failure. */
bool wv_snapshot_restore(WaveformSnapshot *snap, MT_Sample *dst);

#ifdef __cplusplus
}
#endif

#endif /* MAXTRACKER_LFE */

#endif /* WV_DRAFT_H */
