/*
 * pattern_view.h — Pattern grid renderer for the top screen.
 *
 * The shared EditorCursor lives in editor_state.h. Other views that only
 * need cursor access should include that header directly rather than
 * pulling in the full pattern view.
 */

#ifndef MT_PATTERN_VIEW_H
#define MT_PATTERN_VIEW_H

#include <nds.h>
#include <stdbool.h>
#include "editor_state.h"

/* Handle all input while the pattern view owns the screen.
 * Runs navigation_handle_shift internally; returns if navigation consumed
 * the input. START toggles pattern loop (overview=all, inside=solo). */
void pattern_view_input(u32 kd, u32 kh);

/* Draw the full pattern view on the top screen */
void pattern_view_draw(u8 *fb);

/* Draw the bottom-screen panel for pattern mode: instrument list,
 * channel mute indicators, playback status, memory bar, transport.
 * Skips redraw when no tracked state has changed since last call. */
void pattern_view_draw_bottom(u8 *fb);

/* Force the next pattern_view_draw_bottom() to do a full redraw.
 * Call this on screen transitions or after loading a new song. */
void pattern_view_invalidate_bottom(void);

/* Draw just the status bar (row 0) */
void pattern_view_draw_status(u8 *fb);

/* Draw just the transport bar (row 31) */
void pattern_view_draw_transport(u8 *fb);

/* How many channels are visible per overview group at the current font
 * size. Input handlers use this to scroll ch_group in chunks that match
 * the on-screen group size. */
int pattern_view_overview_visible_channels(void);

#endif /* MT_PATTERN_VIEW_H */
