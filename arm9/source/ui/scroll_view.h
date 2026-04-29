/*
 * scroll_view.h — Generic vertical viewport for row-based UI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Holds viewport geometry + scroll state. Knows nothing about what a
 * row is — drawing, input, and row semantics stay with the caller. The
 * typical adoption pattern per frame is:
 *
 *     sv.cursor = my_cursor_row;   // whatever your view uses
 *     scroll_view_follow(&sv);
 *     int first, last;
 *     scroll_view_visible(&sv, &first, &last);
 *     for (int i = first; i < last; i++) {
 *         int gr = sv.row_y + (i - sv.scroll);
 *         draw_my_row(fb, gr, i);
 *     }
 *     scroll_view_draw_scrollbar(&sv, fb, FONT_COLS - 1);
 *
 * Rendering outside [first, last) never happens, so views that used to
 * overflow the footer in BIG font mode stay inside their viewport.
 */

#ifndef MT_SCROLL_VIEW_H
#define MT_SCROLL_VIEW_H

#include <nds.h>
#include <stdbool.h>

typedef struct {
    /* Viewport geometry (grid rows, caller scales for font mode). */
    int row_y;           /* top grid row of the viewport */
    int row_height;      /* number of grid rows in the viewport */

    /* Content. Caller writes both before calling follow()/visible(). */
    int total;           /* total logical row count */
    int cursor;          /* focused logical row, or -1 for none */

    /* Persisted state — owned by the caller across frames. */
    int scroll;          /* top logical row currently displayed */
    int margin;          /* keep N rows above/below cursor visible (0..height/2) */
} ScrollView;

/* Clamp sv->scroll so the cursor sits inside the viewport with the
 * configured margin, and so the viewport never shows non-existent rows
 * past `total`. Safe to call every frame; idempotent. */
void scroll_view_follow(ScrollView *sv);

/* Return the logical index range currently on screen.
 * `*first` inclusive, `*last` exclusive. Always satisfies
 *     0 <= *first <= *last <= sv->total. */
void scroll_view_visible(const ScrollView *sv, int *first, int *last);

/* Draw a 1-column scrollbar at grid column `col`, spanning the full
 * viewport height. No-op when the content fits (total <= row_height). */
void scroll_view_draw_scrollbar(const ScrollView *sv, u8 *fb, int col);

#endif /* MT_SCROLL_VIEW_H */
