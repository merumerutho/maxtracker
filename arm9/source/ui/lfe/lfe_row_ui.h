/*
 * lfe_row_ui.h — shared row-list UI helpers for the LFE (Waveform
 * Editor) tabs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every LFE generator tab (tone, drum, synth, fm4, braids) renders a
 * vertical list of labeled rows where each row has a value column and
 * an "adjust" behavior invoked by the A-held d-pad. The lists are
 * declared with an X-macro (see SYNTH_PARAMS / DRUM_PARAMS / etc.) so
 * that enum tags, label tables, and dispatch arrays all derive from a
 * single source.
 *
 * This header provides the contract each tab's X-macro uses:
 *   - `wv_fmt_fn` / `wv_adj_fn` — signatures for the per-row functions,
 *   - `wv_db_step` / `wv_int_clamped` / `wv_cycle` — step helpers used
 *     by the vast majority of row adjusts,
 *   - `wv_draw_rows` — the one renderer every tab calls.
 *
 * Not LFE-library-specific. Lives under arm9/source/ui/lfe/ because
 * only the LFE tabs need it, but could be reused by any similarly
 * structured screen.
 */

#ifndef LFE_ROW_UI_H
#define LFE_ROW_UI_H

#include <nds.h>
#include <stdint.h>

#include "scroll_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-row function signatures.
 *
 *   wv_fmt_fn: write the value column into `buf` (up to `n` bytes).
 *   wv_adj_fn: mutate state by `delta`. Callers pass ±1 for A+L/R and
 *              ±10 for A+UP/DN; each function scales that by its own
 *              step size (see wv_int_clamped).
 */
typedef void (*wv_fmt_fn)(char *buf, int n);
typedef void (*wv_adj_fn)(int delta);

/*
 * Step a Q8.8 dB shadow and sync the matching Q15 gain field.
 *
 *   dB step = 256 Q8.8 units per unit of delta (= 1 dB).
 *   Clamped to [LFE_DB_FLOOR_Q8_8, 0].
 *
 * Why shadows: lfe_db_to_q15 is a LUT and the round-trip dB→Q15→dB is
 * only accurate to ±0.5 dB. Keeping the shadow lets the user dial
 * repeatedly without drift.
 */
void wv_db_step(int *shadow_db_q8_8, uint16_t *q15_field, int delta);

/*
 * `*v += delta * step`, clamped to [lo, hi]. Handles all the "1 Hz per
 * unit, A+UP/DN = 10 Hz" cases for integer fields.
 */
void wv_int_clamped(int *v, int delta, int step, int lo, int hi);

/*
 * Cycle `*v` by `delta` through [0, n). Used for picker rows (preset,
 * waveform, filter mode, ...).
 */
void wv_cycle(int *v, int delta, int n);

/*
 * Render a vertical row list into a scrolling viewport. Each row is
 * drawn as "<marker> <label>" at `label_col`, and the formatted value
 * at `value_col`. The row at sv->cursor is highlighted.
 *
 * Before calling, the caller populates:
 *   sv->row_y       — top grid row of viewport
 *   sv->row_height  — viewport height in grid rows
 *   sv->total       — row count (number of entries in labels[] / fmts[])
 *   sv->cursor      — highlighted row (caller's persisted param_row)
 *   sv->margin      — rows of context above/below cursor (typical: 2)
 *
 * The function calls scroll_view_follow(), draws only the visible range,
 * and renders a scrollbar in the rightmost grid column when the content
 * exceeds the viewport.
 */
void wv_draw_rows(u8 *top_fb,
                  ScrollView *sv,
                  int label_col, int value_col,
                  const char * const *labels,
                  const wv_fmt_fn *fmts);

/*
 * Per-row fader descriptor. Built as a stack array in the tab's draw
 * function and passed to wv_draw_rows_ex(). `color == 0` (PAL_BG)
 * means "no fader for this row" — use for enum/picker rows.
 */
typedef struct {
    int val;
    int val_max;
    u8  color;
} wv_fader_info;

/*
 * Like wv_draw_rows but appends a colored fader bar to rows that have
 * one. `faders` is parallel to labels/fmts (one entry per row). Pass
 * NULL to get identical behavior to wv_draw_rows (no faders at all).
 */
void wv_draw_rows_ex(u8 *top_fb,
                     ScrollView *sv,
                     int label_col, int value_col,
                     const char * const *labels,
                     const wv_fmt_fn *fmts,
                     const wv_fader_info *faders);

/*
 * LSDJ-style A-held row input. Plain UP/DOWN walks `*row` through
 * [0, row_count); A-held L/R step the active adjust fn by ±1 and
 * A-held UP/DN step by ±10 (both repeat-aware via keysDownRepeat).
 *
 * The canonical input handler for every X-macro-driven tab — replace
 * the hand-rolled `if (!(held & KEY_A)) { nav } else { adjust }` block
 * with a single call.
 */
void wv_handle_row_input(int *row, int row_count,
                         const wv_adj_fn *adjs,
                         u32 down, u32 held);

/* ------------------------------------------------------------------ */
/* Dispatch-array boilerplate                                          */
/* ------------------------------------------------------------------ */

/*
 * Generate `<prefix>_fmts[]` and `<prefix>_adjs[]` static const arrays
 * from a row-list X-macro. LIST must be a macro taking one arg X; each
 * expansion of X receives (name, label, fmt_fn, adj_fn) per row.
 *
 * Example:
 *   #define SYNTH_PARAMS(X) \
 *       X(PRESET,  "Preset", sfmt_preset, sadj_preset) \
 *       X(PITCH,   "Pitch",  sfmt_pitch,  sadj_pitch)  \
 *       ...
 *   WV_ROW_DISPATCH(synth, SYNTH_PARAMS, SYNTH_PARAM_COUNT)
 *
 * Expands to:
 *   static const wv_fmt_fn synth_fmts[SYNTH_PARAM_COUNT] = { sfmt_preset, sfmt_pitch, ... };
 *   static const wv_adj_fn synth_adjs[SYNTH_PARAM_COUNT] = { sadj_preset, sadj_pitch, ... };
 */
#define WV_ROW_DISPATCH_FMT_ENTRY_(name, label, fmt, adj)  fmt,
#define WV_ROW_DISPATCH_ADJ_ENTRY_(name, label, fmt, adj)  adj,
#define WV_ROW_DISPATCH(prefix, LIST, COUNT)                                \
    static const wv_fmt_fn prefix##_fmts[COUNT] = {                         \
        LIST(WV_ROW_DISPATCH_FMT_ENTRY_)                                    \
    };                                                                      \
    static const wv_adj_fn prefix##_adjs[COUNT] = {                         \
        LIST(WV_ROW_DISPATCH_ADJ_ENTRY_)                                    \
    }

#ifdef __cplusplus
}
#endif

#endif /* LFE_ROW_UI_H */
