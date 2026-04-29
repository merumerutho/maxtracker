/*
 * wv_fx.h — contract between the FX-room dispatcher (lfe_fx_view.c)
 * and each effect module (fx_<name>.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mirrors the wv_tab pattern used by the generator rooms: each effect
 * exports a descriptor with lifecycle hooks, and the dispatcher owns a
 * registry array of pointers to those descriptors. Adding a new effect
 * = write fx_<name>.c, declare it in a new header, and add one entry
 * to the registry in lfe_fx_view.c.
 *
 * Shared helpers (to_q15, clamp_i, set_status) are provided for the
 * effects to avoid every module rolling its own.
 */

#ifndef WV_FX_H
#define WV_FX_H

#ifdef MAXTRACKER_LFE

#include <nds.h>
#include <stdint.h>

#include "lfe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Name shown in the header + registry order index. Short. */
    const char *name;

    /* Number of parameter rows the effect exposes. The dispatcher uses
     * this to clamp the cursor when the user navigates with UP/DOWN. */
    int param_count;

    /* Called on FX-room open — reset this effect's state to a sane
     * default. Nullable. */
    void (*on_reset)(void);

    /* Apply the effect destructively to `buf` over `range`. Report the
     * outcome via wv_fx_set_status(). */
    void (*apply)(lfe_buffer *buf, const lfe_fx_range *range);

    /* Adjust parameter at `row` by `small_delta` (A+L/R, ±1) or
     * `big_delta` (A+UP/DN, ±16). Which one to use depends on the
     * parameter type — picker rows use small_delta, knobs use
     * big_delta. */
    void (*adjust)(int row, int small_delta, int big_delta);

    /* Render the effect's parameter rows starting at `start_row` of
     * bot_fb. `cursor_row` is the currently-highlighted row (0-indexed
     * within this effect). */
    void (*draw)(u8 *bot_fb, int start_row, int cursor_row);
} wv_fx;

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* Map 0..127 → 0..Q15_ONE. Saturates outside the range. */
uint16_t wv_fx_to_q15(int val);

/* Integer clamp. Inline-friendly. */
static inline int wv_fx_clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Write a formatted status string to the FX-room status line. Displayed
 * for ~3 seconds (status_timer ticks). Implementation lives in
 * lfe_fx_view.c so it can write directly into the dispatcher's fxv
 * struct. */
void wv_fx_set_status(const char *fmt, ...);

/* Canonical apply-result reporter. On success:
 *   "<NAME> applied [<start>-<end>]"
 * On any lfe_fx_* library failure:
 *   "FX error <rc>"
 *
 * Every effect module's `apply()` ends with one call to this. */
void wv_fx_report_apply(const char *name,
                        lfe_status rc,
                        const lfe_fx_range *range);

/* Draw a single parameter row into the bottom-screen param panel.
 *
 *   - Highlights the row background when `index == cursor`.
 *   - Picks PAL_WHITE vs PAL_GRAY for the text color accordingly.
 *   - Writes the printf-formatted content at column 1, then advances
 *     `*row` by one so consecutive calls stack vertically.
 *
 * The full row text (label + value) is passed as one format string so
 * each effect controls its own column layout. Example:
 *
 *   int row = start_row;
 *   wv_fx_draw_row(fb, &row, cursor, 0, "Mode      %s", names[mode]);
 *   wv_fx_draw_row(fb, &row, cursor, 1, "Drive     %3d", st.drive);
 */
void wv_fx_draw_row(u8 *fb, int *row, int cursor, int index,
                    const char *fmt, ...);

/* Draw a parameter row with a colored horizontal fader bar after the
 * text. Same text layout as wv_fx_draw_row, plus a pixel-level bar
 * spanning the remaining row width. `val` in [0, val_max] controls the
 * fill proportion; `bar_color` is a PAL_* index for the filled portion.
 *
 * Typical usage — replace wv_fx_draw_row calls for knob-type params:
 *
 *   wv_fx_draw_fader_row(fb, &row, cursor, 1,
 *                        st.drive, 127, PAL_RED,
 *                        "Drive     %3d", st.drive);
 */
void wv_fx_draw_fader_row(u8 *fb, int *row, int cursor, int index,
                          int val, int val_max, u8 bar_color,
                          const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* MAXTRACKER_LFE */

#endif /* WV_FX_H */
