/*
 * lfe_row_ui.c — see lfe_row_ui.h for the contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef MAXTRACKER_LFE

#include "lfe_row_ui.h"

#include "draw_util.h"
#include "font.h"
#include "screen.h"
#include "lfe.h"
#include "lfe_dbmath.h"

#include <stdio.h>

void wv_db_step(int *shadow_db, uint16_t *q15_field, int delta)
{
    int db = *shadow_db + delta * 256;
    if (db < LFE_DB_FLOOR_Q8_8) db = LFE_DB_FLOOR_Q8_8;
    if (db > 0)                 db = 0;
    *shadow_db = db;
    *q15_field = (uint16_t)lfe_db_to_q15((int16_t)db);
}

void wv_int_clamped(int *v, int delta, int step, int lo, int hi)
{
    int nv = *v + delta * step;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    *v = nv;
}

void wv_cycle(int *v, int delta, int n)
{
    int nv = (*v + delta) % n;
    if (nv < 0) nv += n;
    *v = nv;
}

void wv_handle_row_input(int *row, int row_count,
                         const wv_adj_fn *adjs,
                         u32 down, u32 held)
{
    if (!(held & KEY_A)) {
        if (down & KEY_UP) {
            if (*row > 0) (*row)--;
            else          *row = row_count - 1;
        }
        if (down & KEY_DOWN) {
            if (*row < row_count - 1) (*row)++;
            else                       *row = 0;
        }
        return;
    }

    /* A-held editing. A+L/R ±1, A+UP/DN ±10 via keysDownRepeat. */
    u32 rep = keysDownRepeat();
    bool dir_pressed =
        (rep & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) != 0;
    if (rep & KEY_LEFT)  adjs[*row](-1);
    if (rep & KEY_RIGHT) adjs[*row](+1);
    if (rep & KEY_DOWN)  adjs[*row](-10);
    if (rep & KEY_UP)    adjs[*row](+10);

    /* Plain A-tap (pressed this frame, no direction) = "activate" the
     * row. Used by submenu-entry rows ("Mod Slots >>"); regular adjust
     * rows route delta=0 into wv_cycle / wv_int_clamped which treat it
     * as a no-op, so this is backward-compat for every other row. */
    if ((down & KEY_A) && !dir_pressed) adjs[*row](0);
}

/* Fader bar geometry for the generator row lists — pixel coordinates
 * derived from value_col. The bar starts after the value text and
 * extends to near the right edge (leaving room for the scrollbar). */
#define GEN_FADER_TRACK_PAL  PAL_HEADER_BG

static void draw_gen_fader(u8 *fb, int grid_row,
                           int value_col,
                           const wv_fader_info *fi)
{
    if (!fi || fi->color == 0) return;

    int px_x  = (value_col + 9) * FONT_W;
    int bar_w = ((FONT_COLS - 1 - value_col - 9) * FONT_W * 2) / 3;
    if (bar_w < 4) return;
    int py_top = grid_row * FONT_H + 1;
    int bar_h  = FONT_H - 4;
    if (bar_h < 1) bar_h = 1;

    ui_draw_fader(fb, px_x, py_top, bar_w, bar_h,
                  fi->val, fi->val_max, fi->color, GEN_FADER_TRACK_PAL);
}

void wv_draw_rows_ex(u8 *top_fb,
                     ScrollView *sv,
                     int label_col, int value_col,
                     const char * const *labels,
                     const wv_fmt_fn *fmts,
                     const wv_fader_info *faders)
{
    if (!sv) return;
    scroll_view_follow(sv);

    int first, last;
    scroll_view_visible(sv, &first, &last);

    for (int r = 0; r < sv->row_height; r++)
        font_fill_row(top_fb, sv->row_y + r, 0, FONT_COLS, PAL_BG);

    for (int i = first; i < last; i++) {
        int gr = sv->row_y + (i - sv->scroll);
        bool selected = (i == sv->cursor);
        u8 color = selected ? PAL_WHITE : PAL_GRAY;
        const char *marker = selected ? ">" : " ";
        font_printf(top_fb, label_col, gr, color, "%s %s",
                    marker, labels[i]);
        char val[32];
        fmts[i](val, sizeof(val));
        font_puts(top_fb, value_col, gr, val, color);

        if (faders) draw_gen_fader(top_fb, gr, value_col, &faders[i]);
    }

    scroll_view_draw_scrollbar(sv, top_fb, FONT_COLS - 1);
}

void wv_draw_rows(u8 *top_fb,
                  ScrollView *sv,
                  int label_col, int value_col,
                  const char * const *labels,
                  const wv_fmt_fn *fmts)
{
    wv_draw_rows_ex(top_fb, sv, label_col, value_col,
                    labels, fmts, NULL);
}

#endif /* MAXTRACKER_LFE */
