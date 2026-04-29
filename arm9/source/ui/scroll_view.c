/*
 * scroll_view.c — see scroll_view.h for the contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scroll_view.h"

#include "font.h"
#include "screen.h"

void scroll_view_follow(ScrollView *sv)
{
    if (!sv) return;
    if (sv->row_height <= 0) { sv->scroll = 0; return; }
    if (sv->total <= sv->row_height) { sv->scroll = 0; return; }

    int margin = sv->margin;
    /* Margin can't exceed half the viewport or the cursor would never
     * be satisfiable at both edges. */
    int max_margin = sv->row_height / 2;
    if (margin < 0)          margin = 0;
    if (margin > max_margin) margin = max_margin;

    if (sv->cursor >= 0 && sv->cursor < sv->total) {
        /* Scroll up when cursor is too close to the top edge. */
        int min_top = sv->cursor - (sv->row_height - 1 - margin);
        if (sv->scroll < min_top) sv->scroll = min_top;
        /* Scroll down when cursor is too close to the top (respecting
         * margin at the top). */
        int max_top = sv->cursor - margin;
        if (sv->scroll > max_top) sv->scroll = max_top;
    }

    /* Global clamp so we never show blank rows past `total`. */
    int max_scroll = sv->total - sv->row_height;
    if (sv->scroll > max_scroll) sv->scroll = max_scroll;
    if (sv->scroll < 0)          sv->scroll = 0;
}

void scroll_view_visible(const ScrollView *sv, int *first, int *last)
{
    int f = 0, l = 0;
    if (sv && sv->row_height > 0 && sv->total > 0) {
        f = sv->scroll;
        if (f < 0) f = 0;
        if (f > sv->total) f = sv->total;
        l = f + sv->row_height;
        if (l > sv->total) l = sv->total;
    }
    if (first) *first = f;
    if (last)  *last  = l;
}

void scroll_view_draw_scrollbar(const ScrollView *sv, u8 *fb, int col)
{
    if (!sv || !fb) return;
    if (sv->row_height <= 0)             return;
    if (sv->total <= sv->row_height)     return;   /* fits: nothing to draw */
    if (col < 0 || col >= FONT_COLS)     return;

    /* Thumb size proportional to the fraction of content visible,
     * clamped to at least 1 row so it's always drawn. */
    int thumb = (sv->row_height * sv->row_height + sv->total - 1) / sv->total;
    if (thumb < 1)               thumb = 1;
    if (thumb > sv->row_height)  thumb = sv->row_height;

    int track = sv->row_height - thumb;
    int max_scroll = sv->total - sv->row_height;
    int thumb_top = (max_scroll > 0)
                    ? (sv->scroll * track + max_scroll / 2) / max_scroll
                    : 0;
    if (thumb_top < 0)     thumb_top = 0;
    if (thumb_top > track) thumb_top = track;

    /* Track: dim vertical bar so the user can see the whole gutter
     * even when the thumb is small. Thumb: bright highlight. */
    for (int i = 0; i < sv->row_height; i++) {
        int gr = sv->row_y + i;
        bool on_thumb = (i >= thumb_top && i < thumb_top + thumb);
        font_putc(fb, col, gr, on_thumb ? 0x7F : '|',
                  on_thumb ? PAL_WHITE : PAL_DIM);
    }
}
