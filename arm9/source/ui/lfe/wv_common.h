/*
 * wv_common.h — shared contract between the LFE dispatcher
 * (waveform_view.c) and each tab module (tab_*.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each LFE tab (tone, drawn, drum, synth, fm4, braids) exports a
 * `wv_tab` descriptor. The dispatcher owns a static array of pointers
 * to these descriptors and calls the right slot without knowing what
 * kind of generator is behind it. Adding a new tab means writing a
 * tab_<name>.c, declaring its descriptor, and adding one entry to the
 * registry in waveform_view.c.
 */

#ifndef WV_COMMON_H
#define WV_COMMON_H

#ifdef MAXTRACKER_LFE

#include <nds.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Name shown in the tab strip. Short — ≤8 chars. */
    const char *name;

    /* Lifecycle. `on_open` is called from waveform_view_open() each
     * time the editor opens; use it to (re)seed the tab's state from
     * the current preset selection. Nullable. */
    void (*on_open)(void);

    /* Input. `down` and `held` are the usual libnds key masks. A tab
     * that manages its own subpages may steal keys that the parent
     * would otherwise claim (see `subpage_open`). */
    void (*input)(u32 down, u32 held);

    /* True if the tab is currently showing a subpage that wants to own
     * L/R/Y/B instead of letting the parent handle them (tab switch,
     * scope zoom, screen exit). Nullable — null == never. */
    bool (*subpage_open)(void);

    /* Called by the parent when B is pressed while a subpage is open,
     * to return to the tab's main page. Must be paired with
     * subpage_open — null otherwise. */
    void (*close_subpage)(void);

    /* Top-screen rendering. Nullable — when null, the dispatcher
     * renders the default scope. Only the Drawn tab takes over the
     * top screen today (to draw its canvas scope). */
    void (*draw_top)(u8 *top_fb);

    /* Bottom-screen rendering. Exactly one of `draw_params` or
     * `draw_bot` must be non-null.
     *
     *   draw_params — tab contributes only the parameter panel; the
     *                 dispatcher draws the header strip, status line,
     *                 and footer around it.
     *   draw_bot    — tab owns the entire bottom screen (canvas,
     *                 keyboard, custom UI). The dispatcher draws
     *                 nothing around it. */
    void (*draw_params)(u8 *bot_fb);
    void (*draw_bot)(u8 *bot_fb);
} wv_tab;

#ifdef __cplusplus
}
#endif

#endif /* MAXTRACKER_LFE */

#endif /* WV_COMMON_H */
