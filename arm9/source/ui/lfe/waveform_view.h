/*
 * waveform_view.h — Waveform Editor menu view (Phase 0 stub).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modal sub-mode of the sample view. Opened from sample_view.c via a
 * Y+A combo. Lets the user invoke lfe library generators on the
 * currently selected sample slot.
 *
 * The entire file is conditionally compiled — when MAXTRACKER_LFE is
 * not defined (the *-nosynth build variants), waveform_view.c becomes
 * empty and sample_view.c does not expose the menu.
 */

#ifndef MT_WAVEFORM_VIEW_H
#define MT_WAVEFORM_VIEW_H

#ifdef MAXTRACKER_LFE

#include <nds.h>
#include <stdbool.h>

/*
 * Open the waveform editor for sample slot `sample_idx`. Called by
 * handle_shift when navigating SELECT+RIGHT from SCREEN_SAMPLE.
 * Sets a file-static "active" flag so the draw/input functions know
 * they're in the editor and which sample slot they're working on.
 */
void waveform_view_open(int sample_idx);

/*
 * Close the menu and return to the sample view. Idempotent.
 */
void waveform_view_close(void);

/*
 * True while the menu is active. Sample view checks this in its draw
 * and input paths to defer to the menu when it's open.
 */
bool waveform_view_is_active(void);

/*
 * Process button input. Called by sample_view_input when the menu is
 * active.
 */
void waveform_view_input(u32 down, u32 held);

/*
 * Render both screens. Called by sample_view_draw when the menu is
 * active. Top screen shows the menu UI; bottom screen shows status
 * and currently selected generator parameters.
 */
void waveform_view_draw(u8 *top_fb, u8 *bot_fb);

/*
 * Draft accessors for the FX room. The FX view operates on the same
 * shared draft buffer — effects mutate it in-place, and the user
 * commits/restores via the same SELECT+A / SELECT+B flow.
 */

/* Get a lfe_buffer view of the current draft. Returns false if no
 * draft is loaded. Caller must not free buf->data. */
bool wv_get_draft_buffer(void *buf_out);

/* Mark the draft as dirty (effect was applied). */
void wv_mark_draft_dirty(void);

/* Get the draft sample rate for FX parameter calculations. */
u32 wv_get_draft_rate(void);

/* Get draft sample count. Returns 0 if no draft loaded. */
u32 wv_get_draft_length(void);

/* Set a status message visible on the LFE status line. */
void wv_set_status(const char *msg);

/* Commit the current draft into the live sample slot. */
void wv_commit_draft(void);

/* Restore the pristine original into the live sample slot. */
void wv_restore_original(void);

#endif /* MAXTRACKER_LFE */

#endif /* MT_WAVEFORM_VIEW_H */
