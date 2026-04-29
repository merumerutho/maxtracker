/*
 * debug_view.h — Tier 1 on-screen debug overlay.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small, always-on overlay that renders into the bottom 5 rows of the
 * top screen. Provides:
 *
 *   - a ring-buffered log of the last 8 dbg_log() calls
 *   - a 32-bit frame counter incremented each vblank
 *   - a "heap used" number derived from sbrk() delta since boot
 *   - a single "last error" slot, updated separately from the log so
 *     it persists after subsequent log lines scroll it out
 *   - a visibility toggle (dbg_toggle) so the overlay can be hidden
 *     when it's in the way
 *
 * All APIs are safe to call from anywhere in the arm9 code — no locks,
 * no allocations, no assumptions about the current view. dbg_log() is
 * varargs printf-style and will truncate at 63 characters per line.
 *
 * The overlay is intentionally tiny and independent: if any of the
 * other UI machinery breaks (screen corruption, crashed view), this
 * module should still be able to surface its state.
 */

#ifndef MT_DEBUG_VIEW_H
#define MT_DEBUG_VIEW_H

#include <nds.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the debug view. Call once at boot, before any dbg_log. */
void dbg_init(void);

/*
 * Append a line to the ring buffer. Printf-style varargs. Lines are
 * silently truncated at 63 characters. The ring holds the last 8
 * lines; older entries are overwritten.
 */
void dbg_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Update the "last error" slot. Unlike dbg_log, this overwrites a
 * single persistent string rather than appending to the ring. Use it
 * for sticky error messages that shouldn't scroll away.
 */
void dbg_set_last_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Increment the frame counter. Call once per vblank. */
void dbg_frame_tick(void);

/* Toggle overlay visibility. */
void dbg_toggle(void);

/* Query current overlay visibility. */
bool dbg_is_visible(void);

/*
 * Render the overlay onto the top-screen framebuffer. Safe to call
 * even when invisible (returns immediately). Occupies rows 27-31.
 */
void dbg_draw_overlay(u8 *top_fb);

#ifdef __cplusplus
}
#endif

#endif /* MT_DEBUG_VIEW_H */
