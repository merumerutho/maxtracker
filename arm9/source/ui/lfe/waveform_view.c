/*
 * waveform_view.c — Waveform Editor menu (Phases 0-4).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modal sub-mode of the sample view, opened from sample_view.c via Y+A.
 * The editor presents a tabbed UI; each tab is a different generator
 * exposed by the lfe library. Tabs are switched with the L/R shoulder
 * buttons. The current generator's input and draw functions are routed
 * via dispatch helpers.
 *
 *   Phase 0 (Test Tone): a sine wave generator with frequency,
 *     amplitude, and duration parameters. Press START to generate.
 *
 *   Phase 2 (Draw): a touchscreen canvas for free-hand waveform
 *     drawing, with preset buttons (sine, saw, square, triangle,
 *     noise) along the bottom row. Press START or A to commit the
 *     drawn waveform as a 256-point looped sample.
 *
 *   Phase 3 (Drum): a preset-based percussion generator. The user
 *     picks one of the library's drum presets (kick, snare, hat,
 *     tom, clap) and a duration in milliseconds; pressing START or
 *     A commits a one-shot percussion sample to the current slot.
 *
 *   Phase 4 (Synth): a preset-based subtractive synth. The user
 *     picks one of four voice presets (lead/pad/pluck/bass), a base
 *     pitch in Hz, and a sample length. The preset's note_off_sample
 *     is scaled proportionally to the user length so release behavior
 *     stays musically correct. Commits as a one-shot.
 *
 * The whole file is wrapped in #ifdef MAXTRACKER_LFE so the *-nosynth
 * build variants compile it as an empty translation unit.
 */

#ifdef MAXTRACKER_LFE

#include "waveform_view.h"

#include "screen.h"
#include "font.h"
#include "draw_util.h"
#include "waveform_render.h"
#include "debug_view.h"
#include "song.h"
#include "editor_state.h"
#include "playback.h"

#include "lfe.h"
#include "lfe_dbmath.h"
#include "lfe_row_ui.h"
#include "wv_draft.h"
#include "wv_common.h"
#include "wv_internal.h"
#include "filebrowser.h"
#include "wav_save.h"

#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Generator tabs                                                     */
/* ================================================================== */

/* wv_gen enum moved to wv_internal.h so tab modules can reference it. */

/* Tab labels — keyed by wv_gen so the display stays coupled to the
 * enum even if the enum is reordered. Positional init was the source
 * of an off-by-one where the displayed tab name lagged behind the
 * actually-selected generator. */
static const char *wv_gen_names[WV_GEN_COUNT] = {
#ifdef MAXTRACKER_LFE_ENABLE_TESTTONE
    [WV_GEN_TEST_TONE] = "Tone",
#endif
    [WV_GEN_DRUM]   = "Drum",
    [WV_GEN_SYNTH]  = "Synth",
    [WV_GEN_FM4]    = "FM4",
    [WV_GEN_BRAIDS] = "BRAIDS",
    [WV_GEN_DRAWN]  = "Draw",
};

/* Test Tone tab lives in lfe/tab_tone.{h,c}. */

/* ================================================================== */
/* Drum parameters                                                    */
/* ================================================================== */

/* Drum parameter lists live in lfe/tab_drum.c. */

/* Synth parameter lists moved to lfe/tab_synth.c. */

/* FM4 parameter lists moved to lfe/tab_fm4.c. */

/* Braids parameter lists moved to lfe/tab_braids.c. */

/* Drawn-canvas geometry lives inside lfe/tab_drawn.c (nobody else
 * needs it). WV_CANVAS_LEN itself is in wv_internal.h because the
 * shared `wv` struct embeds a draw_buf of that size. */

/* ================================================================== */
/* View state                                                         */
/* ================================================================== */

/* WaveformViewState is declared in wv_internal.h. */


WaveformViewState wv = {
    .active         = false,
    .sample_idx     = 0,
    .current_gen    = WV_GEN_DEFAULT,
    .vis_zoom       = 0,        /* start at FIT */
    /* Per-tab state lives inside each tab_<name>.c module. */
    .draft_dirty    = false,
    .exit_pending   = false,
    .status         = "",
    .status_timer   = 0,
};

/* Zoom levels now live in waveform_render.h (shared across all three
 * scopes). WV_VIS_ZOOM_LEVELS is kept as an alias for the existing
 * cycle-modulo logic below. */
#define WV_VIS_ZOOM_LEVELS WAVEFORM_ZOOM_COUNT

/* ================================================================== */
/* Draft/commit architecture                                           */
/*                                                                     */
/* LFE edits are non-destructive by design: the live sample slot       */
/* (song.samples[wv.sample_idx], aka sample_buf) is NEVER touched by   */
/* a generator run. Instead, generators write into wv_draft (lfe_buf), */
/* the top-screen scope shows wv_draft, and the user explicitly        */
/* commits the draft to the sample slot via SELECT+A.                  */
/*                                                                     */
/*   wv_original — og_buf. Captured ONCE at waveform_view_open,        */
/*                 never updated until waveform_view_close. The user's */
/*                 escape hatch: no matter how many commits happen,    */
/*                 SELECT+B restores the pristine pre-LFE state of    */
/*                 the sample slot. Not touched by SELECT+A.           */
/*                                                                     */
/*   wv_draft    — lfe_buf. The user's scratch buffer. At open it's    */
/*                 initialised as a copy of the current sample so the  */
/*                 scope shows meaningful content and the drawn canvas */
/*                 can preload from it. Every generator's START press  */
/*                 rewrites wv_draft. SELECT+A copies wv_draft into    */
/*                 sample_buf (and rebuilds MAS if playing). SELECT+B  */
/*                 does NOT touch wv_draft, so work-in-progress        */
/*                 survives restores.                                  */
/*                                                                     */
/* The "dirty" flag tracks whether wv_draft differs from the last      */
/* committed state, used by the close-with-confirmation path so the    */
/* user doesn't accidentally B-out and lose their scratch work.        */
/* ================================================================== */

/* Snapshot data layer (WaveformSnapshot, wv_original, wv_draft,
 * wv_snapshot_{free,capture,restore}, wv_sample_pcm_bytes) lives in
 * lfe/wv_draft.{h,c}. The action glue that stitches the snapshot API
 * to status messages, sample_idx, and playback rebuilds stays here. */

/* ================================================================== */
/* Lifecycle                                                          */
/* ================================================================== */

/*
 * Preload the drawn canvas from an existing sample in the current
 * slot. The canvas is a fixed 256-point int8 buffer, so any existing
 * sample gets resampled into that shape via nearest-neighbor indexing,
 * and 16-bit PCM is quantized to int8 by taking the high byte. If the
 * slot is empty the canvas is zeroed as before.
 *
 * This lets the user open the Draw tab on an existing sample and
 * *edit* it, rather than always starting from a blank canvas — which
 * was the behavior before and is useless for touch-up work.
 */
static void wv_call_on_open_for_all_tabs(void);

/* Defined alongside the Save-WAV modal state further down — forward-
 * declared here so waveform_view_open/close can defensively close the
 * modal without needing the WvSaveState typedef to be in scope yet. */
static void wv_save_force_close(void);

/* Canvas preload from the current sample slot moved to the drawn
 * tab's on_open handler — see lfe/tab_drawn.c. */

void waveform_view_open(int sample_idx)
{
    if (sample_idx < 0)               sample_idx = 0;
    if (sample_idx >= MT_MAX_SAMPLES) sample_idx = MT_MAX_SAMPLES - 1;

    wv.active       = true;
    wv.sample_idx   = sample_idx;
    wv.current_gen  = WV_GEN_DEFAULT;
    wv.exit_pending = false;
    wv.draft_dirty  = false;

    /* Shared sample duration — retained across tab switches. Seed only
     * on first open (length_ms == 0 after BSS init). Subsequent opens
     * keep the last-used value so a user tweaking back and forth
     * between LFE and other screens doesn't lose their settings. */
    if (wv.length_ms <= 0) wv.length_ms = 1000;

    /* Defensive: ensure the Save-WAV modal starts closed. waveform_view_close()
     * already clears this when LFE exits normally, but a symmetric reset here
     * protects against any path that leaves the flag true. */
    wv_save_force_close();

    /* Capture BOTH snapshots from the current sample state:
     *   wv_original — pristine backup, never touched until close
     *   wv_draft    — initial draft, same content as sample_buf
     *                 so the scope and drawn-canvas preload show
     *                 meaningful content immediately.
     * Both are independent copies. Failure to malloc either one is
     * non-fatal — the editor still opens, but SELECT+B / scope
     * behavior is degraded. */
    bool og_ok    = wv_snapshot_capture(&wv_original,
                                        &song.samples[sample_idx]);
    bool draft_ok = wv_snapshot_capture(&wv_draft,
                                        &song.samples[sample_idx]);

    /* Let every tab seed its state from the current preset selection.
     * synth/drum/fm4 run fill_preset + refresh dB shadows; the drawn
     * tab preloads its canvas from the current sample slot; tabs
     * without an on_open callback are skipped. */
    wv_call_on_open_for_all_tabs();

    if (og_ok && draft_ok) {
        snprintf(wv.status, sizeof(wv.status),
                 "Editing sample %02X", sample_idx + 1);
    } else {
        snprintf(wv.status, sizeof(wv.status),
                 "Editing sample %02X (snapshot degraded)",
                 sample_idx + 1);
    }
    wv.status_timer = 180;

    dbg_log("wv_open idx=%d og=%d draft=%d",
            sample_idx, (int)og_ok, (int)draft_ok);
}

void waveform_view_close(void)
{
    /* If the draft holds uncommitted work, don't close on the first B.
     * Set an exit_pending flag, show a status prompt, and return. A
     * second B press (with exit_pending still true) runs the real
     * close path below. Any other input resets exit_pending — see
     * waveform_view_input. */
    if (wv.draft_dirty && !wv.exit_pending) {
        wv.exit_pending = true;
        snprintf(wv.status, sizeof(wv.status),
                 "Uncommitted draft! B again = discard");
        wv.status_timer = 240;
        return;
    }

    wv.active       = false;
    wv.exit_pending = false;
    wv.draft_dirty  = false;

    /* Free both snapshot buffers so we don't leak across sessions.
     * The user's committed state stays in song.samples[] as normal;
     * only the backup / draft copies are released here. */
    wv_snapshot_free(&wv_original);
    wv_snapshot_free(&wv_draft);

    /* If the Save-WAV modal was open when LFE exited (e.g. the user
     * pressed SELECT+LEFT to leave the editor without first closing
     * the dialog), the modal's `active` flag would persist. On the
     * next waveform_view_open(), input+draw would then route straight
     * into a stale FileBrowser struct whose entry list / path / cursor
     * all point at state from the previous session — typically a
     * crash when navigating or drawing. Clear the modal state here so
     * the next open starts clean. */
    wv_save_force_close();
}

bool waveform_view_is_active(void)
{
    return wv.active;
}

/* ================================================================== */
/* Action: restore the pristine original (SELECT+B)                   */
/* ================================================================== */

/* Called when the user presses SELECT+B. Reverts the current sample
 * slot to the state it had when waveform_view_open() was called, no
 * matter how many commits happened in between. The wv_original
 * snapshot is left intact so the user can restore again after more
 * edits. */
static void wv_restore_original_action(void)
{
    if (!wv_original.valid) {
        snprintf(wv.status, sizeof(wv.status),
                 "No snapshot to restore");
        wv.status_timer = 180;
        return;
    }
    if (wv.sample_idx < 0 || wv.sample_idx >= MT_MAX_SAMPLES) return;

    MT_Sample *s = &song.samples[wv.sample_idx];
    if (!wv_snapshot_restore(&wv_original, s)) {
        snprintf(wv.status, sizeof(wv.status),
                 "Restore failed (malloc)");
        wv.status_timer = 240;
        return;
    }

    /* Mirror the commit path: if a song is playing, rebuild the MAS
     * buffer so the restored PCM is audible on the next note trigger. */
    if (playback_is_playing())
        playback_rebuild_mas();

    snprintf(wv.status, sizeof(wv.status),
             "Sample %02X restored (draft kept)", wv.sample_idx + 1);
    wv.status_timer = 180;
    dbg_log("wv restore_original idx=%d", wv.sample_idx);
}

/* ================================================================== */
/* Common: write a generated buffer into wv_draft                      */
/* ================================================================== */

/*
 * Take ownership of `pcm` and store it in wv_draft, freeing any
 * previous draft PCM first. The library produces 16-bit mono signed
 * samples; the maxtracker model has a single PCM pointer for both
 * 8-bit and 16-bit, gated by the bits/format fields.
 *
 * `loop_length` and `loop_type` together control how the playback
 * engine will treat the sample AFTER commit:
 *   - Sustained / periodic samples: loop_length > 0, loop_type = 1.
 *     For sustained tones pass the full length; for single-cycle
 *     drawn waveforms pass the cycle length.
 *   - One-shot / percussion samples: loop_length = 0, loop_type = 0.
 *
 * `name_prefix` is the leading word of the sample name (e.g. "Tone",
 * "Drawn", "Drum"). The slot index is appended automatically.
 *
 * This function does NOT touch song.samples[] or the MAS buffer — it
 * only populates wv_draft. The user has to SELECT+A to actually
 * promote the draft into the live sample slot.
 */
/* Declared in wv_internal.h so tab modules can call it directly.
 *
 * Renders the shared length_ms alongside its resulting 16-bit-mono
 * byte-size estimate. At 32 kHz: byte_size = length_ms × 64. Shown in
 * KiB (1024-based) for readability; very short samples (< 1 KiB) fall
 * back to raw bytes. Rate is taken explicitly so a future low-rate tab
 * gets an accurate readout without edit churn here. */
void wv_format_length_with_size(char *buf, int n, int length_ms, uint32_t rate_hz)
{
    if (!buf || n <= 0) return;
    uint32_t bytes = (uint32_t)length_ms * rate_hz / 1000u * 2u;  /* int16 = 2 bytes/sample */
    if (bytes < 1024u) {
        snprintf(buf, n, "%4d (%luB)", length_ms, (unsigned long)bytes);
    } else {
        unsigned kib = bytes / 1024u;
        snprintf(buf, n, "%4d (%uK)", length_ms, kib);
    }
}

void wv_write_draft_16(int16_t *pcm,
                       uint32_t length,
                       uint32_t rate,
                       uint32_t loop_length,
                       u8 loop_type,
                       const char *name_prefix)
{
    dbg_log("draft write len=%lu rate=%lu loop=%lu/%d",
            (unsigned long)length, (unsigned long)rate,
            (unsigned long)loop_length, (int)loop_type);

    if (wv.sample_idx < 0 || wv.sample_idx >= MT_MAX_SAMPLES) {
        free(pcm);
        dbg_set_last_error("draft: bad idx %d", wv.sample_idx);
        snprintf(wv.status, sizeof(wv.status), "Bad sample index");
        wv.status_timer = 180;
        return;
    }

    /* Drop any previous draft PCM before replacing it. */
    if (wv_draft.pcm) {
        free(wv_draft.pcm);
        wv_draft.pcm = NULL;
    }

    /* Take ownership of the caller's malloc'd buffer directly — no
     * extra memcpy. The WaveformSnapshot struct is happy to hold any
     * u8* as its pcm field; the rest of the code treats wv_draft.pcm
     * and wv_draft.pcm_bytes as the authoritative draft data. */
    wv_draft.pcm       = (u8 *)pcm;
    wv_draft.pcm_bytes = length * sizeof(int16_t);

    MT_Sample *m = &wv_draft.meta;
    memset(m, 0, sizeof(*m));
    m->active         = true;
    m->pcm_data       = NULL;      /* invariant: snapshot meta.pcm_data is NULL */
    m->length         = length;
    m->bits           = 16;
    m->format         = 1;         /* 16-bit signed */
    m->base_freq      = rate;
    m->default_volume = 64;
    m->panning        = 128;
    m->global_volume  = 64;
    m->loop_start     = 0;
    m->loop_length    = loop_length;
    m->loop_type      = loop_type;
    m->drawn          = false;

    snprintf(m->name, sizeof(m->name), "%s %02X",
             name_prefix, wv.sample_idx + 1);

    wv_draft.valid  = true;
    wv.draft_dirty  = true;

    snprintf(wv.status, sizeof(wv.status),
             "Draft ready (SEL+A commit)");
    wv.status_timer = 180;

    dbg_log("draft OK name=\"%s\"", m->name);
}

/* ================================================================== */
/* Action: commit draft into sample slot (SELECT+A)                    */
/* ================================================================== */

static void wv_commit_draft_action(void)
{
    if (!wv_draft.valid) {
        snprintf(wv.status, sizeof(wv.status),
                 "Draft is empty (press START first)");
        wv.status_timer = 180;
        return;
    }
    if (wv.sample_idx < 0 || wv.sample_idx >= MT_MAX_SAMPLES) return;

    MT_Sample *s = &song.samples[wv.sample_idx];

    /* wv_snapshot_restore copies the draft's meta over and allocates a
     * FRESH PCM copy into the target — the draft buffer stays intact so
     * the user can keep tweaking and re-committing. */
    if (!wv_snapshot_restore(&wv_draft, s)) {
        snprintf(wv.status, sizeof(wv.status),
                 "Commit failed (malloc)");
        wv.status_timer = 240;
        return;
    }

    if ((int)(wv.sample_idx + 1) > song.samp_count)
        song.samp_count = wv.sample_idx + 1;

    /* Now that song.samples[idx] has changed, rebuild the MAS buffer
     * so a running song picks up the new sample on the next note. */
    if (playback_is_playing())
        playback_rebuild_mas();

    wv.draft_dirty = false;
    snprintf(wv.status, sizeof(wv.status),
             "Committed to sample %02X", wv.sample_idx + 1);
    wv.status_timer = 180;
    dbg_log("wv commit_draft idx=%d", wv.sample_idx);
}

/* ================================================================== */
/* Draft accessors (used by lfe_fx_view)                              */
/* ================================================================== */

bool wv_get_draft_buffer(void *buf_out)
{
    if (!wv_draft.valid || !wv_draft.pcm) return false;
    lfe_buffer *b = (lfe_buffer *)buf_out;
    b->data   = (lfe_sample_t *)wv_draft.pcm;
    b->length = wv_draft.meta.length;
    b->rate   = wv_draft.meta.base_freq;
    return true;
}

void wv_mark_draft_dirty(void)
{
    wv.draft_dirty = true;
}

u32 wv_get_draft_rate(void)
{
    return wv_draft.valid ? wv_draft.meta.base_freq : 32000;
}

u32 wv_get_draft_length(void)
{
    return wv_draft.valid ? wv_draft.meta.length : 0;
}

void wv_set_status(const char *msg)
{
    snprintf(wv.status, sizeof(wv.status), "%s", msg);
    wv.status_timer = 180;
}

void wv_commit_draft(void)
{
    wv_commit_draft_action();
}

void wv_restore_original(void)
{
    wv_restore_original_action();
}

/* Test Tone generator moved to lfe/tab_tone.c. */

/* Drawn generator moved to lfe/tab_drawn.c. */


/* Drum generator moved to lfe/tab_drum.c. */


/* Synth generator moved to lfe/tab_synth.c. */


/* FM4 generator moved to lfe/tab_fm4.c. */


/* Braids generator moved to lfe/tab_braids.c. */


/* ================================================================== */
/* Save-as-WAV dialog                                                  */
/*                                                                     */
/* A modal sub-mode of LFE that composes the existing FileBrowser for  */
/* folder navigation and adds a filename editor with LSDJ-style       */
/* character cycling. Zero modifications to filebrowser.{h,c}: this   */
/* code wraps a FileBrowser instance and calls its init/draw/input    */
/* directly. The WAV writer in wav_save.{h,c} does the actual I/O.    */
/* ================================================================== */

static const char save_charset[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
#define SAVE_CHARSET_LEN  ((int)(sizeof(save_charset) - 1))
#define SAVE_NAME_MAX     20   /* editable name (no extension) */

typedef struct {
    bool         active;
    FileBrowser  fb;
    char         filename[SAVE_NAME_MAX + 1];
    int          name_cursor;    /* 0..strlen(filename)-1 */
    bool         name_editing;   /* X toggles: UP/DN cycles chars vs browse */
} WvSaveState;

static WvSaveState wv_save;

/* Defensive reset used by waveform_view_open / waveform_view_close to
 * prevent stale Save-WAV state (FileBrowser path / cursor / entries)
 * carrying across LFE sessions. Forward-declared above those callers. */
static void wv_save_force_close(void)
{
    wv_save.active       = false;
    wv_save.name_editing = false;
}

static int save_charset_index(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;  /* case-insensitive */
    for (int i = 0; i < SAVE_CHARSET_LEN; i++) {
        if (save_charset[i] == c) return i;
    }
    return 0;
}

static void wv_save_open(void)
{
    filebrowser_init(&wv_save.fb, fs_browse_root);

    /* Default filename based on sample slot: LFE_01 */
    snprintf(wv_save.filename, sizeof(wv_save.filename),
             "LFE_%02X", wv.sample_idx + 1);

    wv_save.name_cursor  = 0;
    wv_save.name_editing = false;
    wv_save.active       = true;
}

static void wv_save_close(void)
{
    wv_save.active       = false;
    wv_save.name_editing = false;
}

static void wv_save_do_save(void)
{
    if (!wv_draft.valid || !wv_draft.pcm || wv_draft.meta.length == 0) {
        snprintf(wv.status, sizeof(wv.status),
                 "Draft is empty, nothing to save");
        wv.status_timer = 180;
        wv_save_close();
        return;
    }

    /* Build path: current folder + filename + ".wav" */
    char path[FB_PATH_LEN + SAVE_NAME_MAX + 8];
    int dlen = strlen(wv_save.fb.path);
    if (dlen > 0 && wv_save.fb.path[dlen - 1] == '/') {
        snprintf(path, sizeof(path), "%s%s.wav",
                 wv_save.fb.path, wv_save.filename);
    } else {
        snprintf(path, sizeof(path), "%s/%s.wav",
                 wv_save.fb.path, wv_save.filename);
    }

    dbg_log("wav_save path=%s len=%lu",
            path, (unsigned long)wv_draft.meta.length);

    int rc = wav_save_mono16(path,
                             (const s16 *)wv_draft.pcm,
                             wv_draft.meta.length,
                             wv_draft.meta.base_freq);
    if (rc == WAV_SAVE_OK) {
        snprintf(wv.status, sizeof(wv.status),
                 "Saved %s.wav", wv_save.filename);
        wv.status_timer = 240;
    } else {
        snprintf(wv.status, sizeof(wv.status),
                 "Save failed (err %d)", rc);
        wv.status_timer = 300;
        dbg_set_last_error("wav_save rc=%d path=%s", rc, path);
    }

    wv_save_close();
}

static void wv_save_input(u32 down, u32 held)
{
    (void)held;

    /* B: cancel save dialog, return to LFE. */
    if (down & KEY_B) {
        wv_save_close();
        return;
    }

    /* START: save with current folder + filename. */
    if (down & KEY_START) {
        wv_save_do_save();
        return;
    }

    /* X: toggle name-editing mode. */
    if (down & KEY_X) {
        wv_save.name_editing = !wv_save.name_editing;
        return;
    }

    if (wv_save.name_editing) {
        /* In name-edit mode: UP/DN cycles character, L/R moves cursor. */
        int len = strlen(wv_save.filename);
        if (len == 0) {
            wv_save.filename[0] = 'A';
            wv_save.filename[1] = '\0';
            len = 1;
        }
        if (wv_save.name_cursor >= len)
            wv_save.name_cursor = len - 1;

        if (down & KEY_UP) {
            int ci = save_charset_index(wv_save.filename[wv_save.name_cursor]);
            ci = (ci + 1) % SAVE_CHARSET_LEN;
            wv_save.filename[wv_save.name_cursor] = save_charset[ci];
        }
        if (down & KEY_DOWN) {
            int ci = save_charset_index(wv_save.filename[wv_save.name_cursor]);
            ci = (ci - 1 + SAVE_CHARSET_LEN) % SAVE_CHARSET_LEN;
            wv_save.filename[wv_save.name_cursor] = save_charset[ci];
        }
        if (down & KEY_LEFT) {
            if (wv_save.name_cursor > 0) wv_save.name_cursor--;
        }
        if (down & KEY_RIGHT) {
            if (wv_save.name_cursor < len - 1) {
                wv_save.name_cursor++;
            } else if (len < SAVE_NAME_MAX) {
                /* Extend the name by one character when pressing RIGHT
                 * past the end. */
                wv_save.filename[len] = 'A';
                wv_save.filename[len + 1] = '\0';
                wv_save.name_cursor = len;
            }
        }
        /* A in name-edit mode: delete character at cursor (backspace). */
        if (down & KEY_A) {
            if (len > 1) {
                memmove(&wv_save.filename[wv_save.name_cursor],
                        &wv_save.filename[wv_save.name_cursor + 1],
                        len - wv_save.name_cursor);
                if (wv_save.name_cursor >= (int)strlen(wv_save.filename))
                    wv_save.name_cursor = strlen(wv_save.filename) - 1;
            }
        }
    } else {
        /* In folder-browse mode: delegate to filebrowser for UP/DOWN/
         * LEFT/RIGHT/A (enter dir) / B (up one level — already handled
         * above). filebrowser_input returns true if user pressed A on a
         * FILE (not a dir) — in browse mode we treat that as "navigate
         * into" behavior for directories and ignore file selection
         * (there's nothing to "select" in save mode). */
        filebrowser_input(&wv_save.fb, down);
    }
}

static void wv_save_draw(u8 *top_fb, u8 *bot_fb)
{
    /* Top screen: folder listing via filebrowser_draw, with header
     * override to say "SAVE WAV" instead of "FILE BROWSER". We clear
     * and draw normally, then overwrite the header row. */
    filebrowser_draw(&wv_save.fb, top_fb);
    font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 1, 0, "SAVE WAV", PAL_WHITE);
    font_printf(top_fb, 20, 0, PAL_GRAY, "Dir: %s", wv_save.fb.path);

    /* Bottom screen: filename editor + hints. */
    font_clear(bot_fb, PAL_BG);

    font_fill_row(bot_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 0, 0, "FILENAME", PAL_TEXT);

    /* Show the editable filename with cursor highlight. */
    int len = strlen(wv_save.filename);
    for (int i = 0; i < len; i++) {
        u8 color;
        if (wv_save.name_editing && i == wv_save.name_cursor)
            color = PAL_WHITE;
        else
            color = PAL_NOTE;
        font_putc(bot_fb, 2 + i, 3, wv_save.filename[i], color);
    }
    font_puts(bot_fb, 2 + len, 3, ".wav", PAL_DIM);

    /* Cursor underline in name-edit mode. */
    if (wv_save.name_editing && wv_save.name_cursor < len) {
        font_putc(bot_fb, 2 + wv_save.name_cursor, 4, '^', PAL_WHITE);
    }

    /* Mode indicator */
    font_fill_row(bot_fb, 7, 0, FONT_COLS, PAL_BG);
    if (wv_save.name_editing) {
        font_puts(bot_fb, 1, 7, "[NAME EDIT]", PAL_ORANGE);
        font_puts(bot_fb, 1, 9,
                  "UP/DN: cycle char   L/R: move cursor",
                  PAL_DIM);
        font_puts(bot_fb, 1, 10,
                  "RIGHT past end: extend   A: delete",
                  PAL_DIM);
        font_puts(bot_fb, 1, 11,
                  "X: exit name edit",
                  PAL_DIM);
    } else {
        font_puts(bot_fb, 1, 7, "[FOLDER BROWSE]", PAL_PLAY);
        font_puts(bot_fb, 1, 9,
                  "UP/DN: navigate   A: enter folder",
                  PAL_DIM);
        font_puts(bot_fb, 1, 10,
                  "B: up one level / cancel",
                  PAL_DIM);
        font_puts(bot_fb, 1, 11,
                  "X: edit filename",
                  PAL_DIM);
    }

    font_fill_row(bot_fb, font_scale_row(30), 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 0, font_scale_row(30),
              "START: save   B: cancel   X: toggle mode",
              PAL_GRAY);
    font_fill_row(bot_fb, font_scale_row(31), 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(bot_fb, 0, font_scale_row(31), PAL_DIM,
                "Saving draft of sample %02X",
                wv.sample_idx + 1);
}

/* ================================================================== */
/* Top-level dispatch                                                 */
/* ================================================================== */

/* ================================================================== */
/* Tab registry                                                        */
/* ================================================================== */

/* Each tab_*.c defines its own subpage_open predicate; the parent's
 * only job is to pick the right tab descriptor from the registry. */

#ifdef MAXTRACKER_LFE_ENABLE_TESTTONE
#  include "tab_tone.h"  /* wv_tab_tone (dev build only) */
#endif
#include "tab_drawn.h"    /* wv_tab_drawn */
#include "tab_drum.h"     /* wv_tab_drum */
#include "tab_synth.h"    /* wv_tab_synth */
#include "tab_fm4.h"      /* wv_tab_fm4 */
#include "tab_braids.h"   /* wv_tab_braids */

static const wv_tab * const wv_tabs[WV_GEN_COUNT] = {
#ifdef MAXTRACKER_LFE_ENABLE_TESTTONE
    [WV_GEN_TEST_TONE] = &wv_tab_tone,
#endif
    [WV_GEN_DRUM]      = &wv_tab_drum,
    [WV_GEN_SYNTH]     = &wv_tab_synth,
    [WV_GEN_FM4]       = &wv_tab_fm4,
    [WV_GEN_BRAIDS]    = &wv_tab_braids,
    [WV_GEN_DRAWN]     = &wv_tab_drawn,
};

static const wv_tab *wv_cur_tab(void)
{
    int g = wv.current_gen;
    if (g < 0 || g >= WV_GEN_COUNT) return wv_tabs[WV_GEN_DEFAULT];
    return wv_tabs[g];
}

static void wv_call_on_open_for_all_tabs(void)
{
    for (int i = 0; i < WV_GEN_COUNT; i++) {
        if (wv_tabs[i] && wv_tabs[i]->on_open) wv_tabs[i]->on_open();
    }
}

void waveform_view_input(u32 down, u32 held)
{
    if (!wv.active) return;

    /* Save dialog is a modal that takes over all input/draw while open. */
    if (wv_save.active) {
        wv_save_input(down, held);
        return;
    }

    /* Log only "meaningful" key-down events so we can verify the input
     * dispatch reaches us, without flooding the ring buffer. Skip pure
     * D-pad and X-held parameter nudging (those spam quickly). */
    u32 log_keys = down & (KEY_A | KEY_B | KEY_START | KEY_L | KEY_R | KEY_TOUCH);
    if (log_keys) {
        dbg_log("wv_input gen=%d down=0x%08lx",
                (int)wv.current_gen, (unsigned long)down);
    }

    /* SELECT+B: restore the pristine original into the sample slot,
     * no matter how many commits have happened. Does NOT touch
     * wv_draft — the user's scratch work is preserved. Must fire
     * BEFORE the plain-B back handler below. */
    if ((held & KEY_SELECT) && (down & KEY_B)) {
        wv.exit_pending = false;  /* explicit action clears pending */
        wv_restore_original_action();
        return;
    }

    /* SELECT+A: commit the current wv_draft into the live sample
     * slot, and rebuild MAS so a running song picks it up. */
    if ((held & KEY_SELECT) && (down & KEY_A)) {
        wv.exit_pending = false;
        wv_commit_draft_action();
        return;
    }

    /* SELECT+X: open the save-as-WAV dialog (folder navigation +
     * filename editor). Exports the current wv_draft to a .wav file
     * on the SD card without touching the song at all. START and
     * SELECT+START are reserved for playback control (pattern loop
     * and song playback respectively). */
    if ((held & KEY_SELECT) && (down & KEY_X)) {
        wv.exit_pending = false;
        wv_save_open();
        return;
    }

    /* Subpage gate — when an in-screen subpage is open, B / L / R / Y
     * are claimed by the subpage instead of falling through to the
     * editor-level handlers (back, tab switch, scope zoom). Currently
     * only the synth mod-slot editor uses this. */
    const wv_tab *tab = wv_cur_tab();
    bool subpage_open = tab->subpage_open && tab->subpage_open();

    /* Top-level: B is reserved for subpage "back". Exiting the editor
     * is SELECT+LEFT only (handled by the global navigation in
     * navigation.c, which calls waveform_view_close()). When no subpage
     * is open, B falls through to the per-generator handler. */
    if ((down & KEY_B) && subpage_open) {
        if (tab->close_subpage) tab->close_subpage();
        return;
    }

    /* Any other keypress cancels a pending exit confirmation — the
     * user decided to keep editing instead of discarding. */
    if (down && wv.exit_pending) {
        wv.exit_pending = false;
        wv.status[0]    = '\0';
        wv.status_timer = 0;
    }

    /* Top-level: L/R shoulder buttons cycle generator tabs. */
    if (!subpage_open && (down & KEY_L)) {
        if (wv.current_gen > 0) wv.current_gen--;
        else                     wv.current_gen = WV_GEN_COUNT - 1;
        return;
    }
    if (down & KEY_L) {
        return; /* swallowed: handled above when allowed */
    }
    if (!subpage_open && (down & KEY_R)) {
        if (wv.current_gen < WV_GEN_COUNT - 1) wv.current_gen++;
        else                                    wv.current_gen = 0;
        return;
    }
    if (down & KEY_R) {
        return;
    }

    /* Top-level: Y cycles the top-screen scope zoom (FIT → 4096 →
     * 1024 → 256 → FIT). The Draw tab has its own bottom-screen
     * layout and doesn't use the scope, so Y is a no-op there. X is
     * still the fast-adjust modifier for LEFT/RIGHT param nudging, so
     * we specifically pick Y here to avoid clashing. */
    if (!subpage_open && (down & KEY_Y) && wv.current_gen != WV_GEN_DRAWN) {
        wv.vis_zoom = (wv.vis_zoom + 1) % WV_VIS_ZOOM_LEVELS;
        return;
    }

    /* Dispatch the rest to the active generator. */
    if (tab->input) tab->input(down, held);
}

/* ================================================================== */
/* Top-screen waveform visualization                                  */
/*                                                                     */
/* Always-on scope of the current sample slot. Layout:                 */
/*                                                                     */
/*   Row 0       : LFE header (title, version, sample NN)              */
/*   Row 2       : tab strip                                           */
/*   Rows 3-25   : big waveform scope (23 font rows = 138 px)          */
/*   Row 26      : zoom indicator + scope footer                       */
/*   Rows 27-31  : reserved for debug overlay                          */
/*                                                                     */
/* Zoom semantics: wv.vis_zoom = 0 is FIT (whole sample in 256 px,     */
/* always available per the UX spec). Levels 1..3 map to a fixed       */
/* visible-sample count (4096 / 1024 / 256), each capped to s->length  */
/* so a short sample on a "zoom in" level just displays at its natural */
/* size. Y button cycles the level.                                    */
/* ================================================================== */

/* Scope region spans from the tab strip bottom (row 3) down to the
 * scope footer (row 26). Pixel bounds vary with font mode so the scope
 * never overlaps the rows above or below it. */
#define WV_VIS_X_WIDTH    256
static inline int wv_vis_y_top(void)    { return 3 * FONT_H; }
static inline int wv_vis_y_height(void) { return font_scale_row(26) * FONT_H - wv_vis_y_top(); }

static void wv_draw_top_visualization(u8 *top_fb)
{
    /* Header row — unchanged from the old layout. */
    font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 0, 0, "WAVEFORM EDITOR", PAL_TEXT);
    font_printf(top_fb, font_scale_col(24), 0, PAL_GRAY, "lfe %s", lfe_version());
    font_printf(top_fb, font_scale_col(50), 0, PAL_TEXT, "Sample %02X", wv.sample_idx + 1);

    /* Tab strip — unchanged. L/R still cycles tabs. B still exits. */
    int col = 0;
    for (int i = 0; i < WV_GEN_COUNT; i++) {
        u8 color = (i == (int)wv.current_gen) ? PAL_WHITE : PAL_GRAY;
        font_printf(top_fb, col, 2, color, "[%s]", wv_gen_names[i]);
        col += (int)strlen(wv_gen_names[i]) + 4;
    }
    font_puts(top_fb, col, 2, "L/R:tab B:back Y:zoom", PAL_DIM);

    /* ---- Pull the DRAFT state for the scope ---- */
    const MT_Sample *s      = wv_draft.valid ? &wv_draft.meta : NULL;
    const u8        *pcm    = wv_draft.pcm;
    uint32_t         length = s ? s->length : 0;

    /* Render through the shared helper so all three scopes (sample,
     * LFE, LFE FX) share styling. FIT = visible 0. */
    int z = wv.vis_zoom;
    if (z < 0) z = 0;
    if (z >= WAVEFORM_ZOOM_COUNT) z = WAVEFORM_ZOOM_COUNT - 1;

    WaveformRenderCfg cfg = {
        .fb           = top_fb,
        .y_top        = wv_vis_y_top(),
        .y_height     = wv_vis_y_height(),
        .pcm          = (s && pcm) ? pcm : NULL,
        .is_16bit     = (s && s->bits == 16),
        .length       = length,
        .visible      = waveform_zoom_levels[z],
        .scroll       = 0,
        .color_bg     = PAL_BG,
        .color_center = PAL_DIM,
        .color_wave   = PAL_PLAY,
    };
    waveform_render(&cfg);

    int scope_foot = font_scale_row(26);
    if (!s || !s->active || !pcm || length == 0) {
        font_puts(top_fb, font_scale_col(22), 13, "(empty draft)", PAL_DIM);
        font_fill_row(top_fb, scope_foot, 0, FONT_COLS, PAL_HEADER_BG);
        font_puts(top_fb, 0, scope_foot, "Zoom: FIT  (empty)", PAL_GRAY);
        return;
    }

    /* ---- Footer with zoom + draft state + sample length ---- */
    font_fill_row(top_fb, scope_foot, 0, FONT_COLS, PAL_HEADER_BG);
    const char *dirty_tag = wv.draft_dirty ? " *" : "";
    if (z == 0) {
        font_printf(top_fb, 0, scope_foot, PAL_GRAY,
                    "Zoom: FIT  Draft%s: %lu",
                    dirty_tag, (unsigned long)length);
    } else {
        font_printf(top_fb, 0, scope_foot, PAL_GRAY,
                    "Zoom: %lu smp  Draft%s: %lu",
                    (unsigned long)waveform_zoom_levels[z],
                    dirty_tag, (unsigned long)length);
    }
}

void waveform_view_draw(u8 *top_fb, u8 *bot_fb)
{
    if (!wv.active) return;

    /* Save dialog takes over both screens while open. */
    if (wv_save.active) {
        wv_save_draw(top_fb, bot_fb);
        return;
    }

    /* ---- Top screen ---- */
    font_clear(top_fb, PAL_BG);

    if (wv.current_gen == WV_GEN_DRAWN) {
        /* Draw tab is the designed exception: the canvas MUST stay on
         * the bottom screen because the stylus can only reach it there,
         * and the help/preset legend for drawing lives on top. */
        font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
        font_puts(top_fb, 0, 0, "WAVEFORM EDITOR", PAL_TEXT);
        font_printf(top_fb, font_scale_col(24), 0, PAL_GRAY, "lfe %s", lfe_version());
        font_printf(top_fb, font_scale_col(50), 0, PAL_TEXT, "Sample %02X",
                    wv.sample_idx + 1);

        int col = 0;
        for (int i = 0; i < WV_GEN_COUNT; i++) {
            u8 color = (i == (int)wv.current_gen) ? PAL_WHITE : PAL_GRAY;
            font_printf(top_fb, col, 2, color, "[%s]", wv_gen_names[i]);
            col += (int)strlen(wv_gen_names[i]) + 4;
        }
        font_puts(top_fb, col, 2, "L/R:tab  B:back", PAL_DIM);

        if (wv_cur_tab()->draw_top) wv_cur_tab()->draw_top(top_fb);
    } else {
        /* All other tabs: always-on scope of the sample slot. */
        if (wv_cur_tab()->draw_top) wv_cur_tab()->draw_top(top_fb);
        else                         wv_draw_top_visualization(top_fb);
    }

    /* Note: rows 27-31 of the top screen are reserved for the debug
     * overlay. wv_draw_top_visualization never writes past row 26. */

    /* ---- Bottom screen ---- */
    font_clear(bot_fb, PAL_BG);

    if (wv_cur_tab()->draw_bot) {
        /* Tab owns the entire bottom screen (canvas/keyboard/etc). */
        wv_cur_tab()->draw_bot(bot_fb);
    } else {
        /* Parameter panel for the active generator — content that used
         * to be on the top screen now lives here. The tt_draw / drum_draw
         * / synth_draw functions were written against a 32-row target
         * starting at row 4, which is the same on the bottom bitmap, so
         * we can reuse them unchanged by just passing bot_fb. Rows 0-3
         * of the bottom screen get a small header; rows 27-31 get a
         * status line + commit prompt. */
        font_fill_row(bot_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
        font_puts(bot_fb, 0, 0, "PARAMETERS", PAL_TEXT);
        font_printf(bot_fb, font_scale_col(50), 0, PAL_GRAY, "Sample %02X",
                    wv.sample_idx + 1);

        if (wv_cur_tab()->draw_params) wv_cur_tab()->draw_params(bot_fb);

        int foot = font_scale_row(30);
        int tport = font_scale_row(31);
        font_fill_row(bot_fb, foot, 0, FONT_COLS, PAL_BG);
        if (wv.status_timer > 0) {
            font_puts(bot_fb, 0, foot, wv.status, PAL_WHITE);
            wv.status_timer--;
        } else {
            font_puts(bot_fb, 0, foot,
                      "SEL+A:commit SEL+B:restore SEL+X:save",
                      PAL_DIM);
        }

        font_fill_row(bot_fb, tport, 0, FONT_COLS, PAL_HEADER_BG);
        font_puts(bot_fb, 0, tport, "L/R:tab  Y:zoom  Output:int16 32k",
                  PAL_GRAY);
    }
}

#endif /* MAXTRACKER_LFE */
