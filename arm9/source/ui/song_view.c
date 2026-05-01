/*
 * song_view.c — Song arrangement view (order table editor).
 *
 * Extracted from main.c — renders the song arrangement grid and handles
 * order-table editing: pattern assignment, cloning, deletion, navigation,
 * and double-tap pattern cloning.
 */

#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "song_view.h"
#include "clipboard.h"
#include "screen.h"
#include "font.h"
#include "song.h"
#include "editor_state.h"
#include "playback.h"
#include "scroll_view.h"
#include "keybind.h"

/* ---- Externs from main.c ---- */
extern char status_msg[64];
extern int  status_timer;
/* dirty flags: use mt_mark_song_modified() from editor_state.h */
/* solo_playback extern removed — START handling centralized in main.c */

/* Sentinel value for empty/end-of-song order positions */
#define ORDER_EMPTY 0xFF

/* Find the first unused pattern slot (NULL in song.patterns[]).
 * Returns the index (0-255), or MT_MAX_PATTERNS (256) if none available. */
static u16 find_unused_pattern(void)
{
    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i] == NULL) return (u16)i;
    }
    return MT_MAX_PATTERNS;
}

/* Clone timer: counts down frames since last A tap (for double-tap clone) */
static u8 clone_timer = 0;

/* Order clipboard now lives in the unified clipboard (clipboard.h). */

/* Insert a new order row at `pos`, shifting everything below down.
 * Assigns `pat` as the pattern. Returns false if full. */
static bool order_insert_at(u8 pos, u8 pat)
{
    if (song.order_count >= MT_MAX_ORDERS) return false;
    if (pos > song.order_count) pos = song.order_count;
    for (int i = song.order_count; i > pos; i--)
        song.orders[i] = song.orders[i - 1];
    song.orders[pos] = pat;
    song.order_count++;
    return true;
}

bool song_view_has_clip(void)
{
    return clipboard_has_orders();
}

bool song_view_paste_orders(void)
{
    if (!clipboard_paste_orders(cursor.order_pos)) return false;
    mt_mark_song_modified();
    snprintf(status_msg, sizeof(status_msg),
             "Pasted %d order(s)", clipboard.order_count);
    status_timer = 90;
    return true;
}

void song_view_input(u32 down, u32 held)
{
    u32 rep = keysDownRepeat();

    /* -- Selection mode: B = copy, B+A = cut -- */
    if (cursor.selecting) {
        if ((held & MT_KEY_BACK) && (down & MT_KEY_CONFIRM)) {
            /* Cut: copy then delete */
            u8 r0 = cursor.sel_start_row;
            u8 r1 = cursor.order_pos;
            if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
            clipboard_copy_orders(r0, r1);
            int n = r1 - r0 + 1;
            if (song.order_count - n < 1) n = song.order_count - 1;
            for (int i = r0; i < song.order_count - n; i++)
                song.orders[i] = song.orders[i + n];
            for (int i = song.order_count - n; i < song.order_count; i++)
                song.orders[i] = ORDER_EMPTY;
            song.order_count -= n;
            if (cursor.order_pos >= song.order_count)
                cursor.order_pos = song.order_count > 0 ? song.order_count - 1 : 0;
            cursor.selecting = false;
            mt_mark_song_modified();
            snprintf(status_msg, sizeof(status_msg),
                     "Cut %d order(s)", clipboard.order_count);
            status_timer = 90;
            return;
        }
        if (down & MT_KEY_BACK) {
            /* Copy selection to clipboard */
            u8 r0 = cursor.sel_start_row;
            u8 r1 = cursor.order_pos;
            if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
            clipboard_copy_orders(r0, r1);
            cursor.selecting = false;
            snprintf(status_msg, sizeof(status_msg),
                     "Copied %d order(s)", clipboard.order_count);
            status_timer = 90;
            return;
        }
        /* While selecting, d-pad moves cursor to extend selection */
        if (rep & KEY_UP) {
            if (cursor.order_pos > 0) cursor.order_pos--;
        }
        if (rep & KEY_DOWN) {
            if (cursor.order_pos + 1 < song.order_count)
                cursor.order_pos++;
        }
        return;
    }

    /* -- B+A: delete single order entry at cursor -- */
    if ((held & MT_KEY_BACK) && (down & MT_KEY_CONFIRM)) {
        if (song.order_count > 1 && cursor.order_pos < song.order_count) {
            for (int i = cursor.order_pos; i < song.order_count - 1; i++)
                song.orders[i] = song.orders[i + 1];
            song.orders[song.order_count - 1] = ORDER_EMPTY;
            song.order_count--;
            if (cursor.order_pos >= song.order_count)
                cursor.order_pos = song.order_count - 1;
            mt_mark_song_modified();
        }
        return;
    }

    /* -- B modifier: warp navigation (B+UP/DOWN jumps 16 positions) -- */
    if (held & MT_KEY_BACK) {
        if (rep & KEY_UP) {
            if (cursor.order_pos >= 16)
                cursor.order_pos -= 16;
            else
                cursor.order_pos = 0;
        }
        if (rep & KEY_DOWN) {
            u8 max_pos = song.order_count > 0 ? song.order_count - 1 : 0;
            if (cursor.order_pos + 16 <= max_pos)
                cursor.order_pos += 16;
            else
                cursor.order_pos = max_pos;
        }
        return;
    }

    /* -- Y: insert new empty pattern row at cursor -- */
    if (down & MT_KEY_MOD_SECONDARY) {
        u16 new_pat = find_unused_pattern();
        if (new_pat < MT_MAX_PATTERNS) {
            song_alloc_pattern((u8)new_pat, 64, song.channel_count);
            if (new_pat >= song.patt_count)
                song.patt_count = new_pat + 1;
            if (order_insert_at(cursor.order_pos, (u8)new_pat)) {
                snprintf(status_msg, sizeof(status_msg),
                         "Inserted new pat %02X", (unsigned)new_pat);
                status_timer = 120;
                mt_mark_song_modified();
            }
        }
        return;
    }

    /* -- A modifier: edit pattern number at cursor -- */
    if (held & MT_KEY_CONFIRM) {
        if (down & MT_KEY_CONFIRM) {
            if (cursor.order_pos >= song.order_count ||
                song.orders[cursor.order_pos] == ORDER_EMPTY) {
                u16 new_pat = find_unused_pattern();
                if (new_pat < MT_MAX_PATTERNS) {
                    song_alloc_pattern((u8)new_pat, 64, song.channel_count);
                    if (cursor.order_pos >= song.order_count)
                        song.order_count = cursor.order_pos + 1;
                    song.orders[cursor.order_pos] = (u8)new_pat;
                    if (new_pat >= song.patt_count)
                        song.patt_count = new_pat + 1;
                    snprintf(status_msg, sizeof(status_msg),
                             "New pat %02X", (unsigned)new_pat);
                    status_timer = 120;
                    mt_mark_song_modified();
                }
                clone_timer = 0;
            } else {
                if (clone_timer > 0 && clone_timer < 15) {
                    u8 src_pat = song.orders[cursor.order_pos];
                    u16 new_pat = find_unused_pattern();
                    if (new_pat < MT_MAX_PATTERNS && song.patterns[src_pat]) {
                        MT_Pattern *sp = song.patterns[src_pat];
                        song_alloc_pattern((u8)new_pat, sp->nrows, sp->ncols);
                        memcpy(song.patterns[new_pat], sp,
                               MT_PATTERN_SIZE(sp->nrows, sp->ncols));
                        song.orders[cursor.order_pos] = (u8)new_pat;
                        if (new_pat >= song.patt_count)
                            song.patt_count = new_pat + 1;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Cloned %02X->%02X", src_pat, (unsigned)new_pat);
                        status_timer = 120;
                        mt_mark_song_modified();
                    }
                    clone_timer = 0;
                } else {
                    clone_timer = 15;
                }
            }
        }
        if (cursor.order_pos < song.order_count &&
            song.orders[cursor.order_pos] != ORDER_EMPTY) {
            if (rep & KEY_LEFT) {
                if (song.orders[cursor.order_pos] > 0) {
                    song.orders[cursor.order_pos]--;
                    mt_mark_song_modified();
                }
            }
            if (rep & KEY_RIGHT) {
                if (song.orders[cursor.order_pos] < MT_MAX_PATTERNS - 1) {
                    song.orders[cursor.order_pos]++;
                    song_ensure_pattern(song.orders[cursor.order_pos]);
                    if (song.orders[cursor.order_pos] >= song.patt_count)
                        song.patt_count = song.orders[cursor.order_pos] + 1;
                    mt_mark_song_modified();
                }
            }
            if (rep & KEY_UP) {
                u8 v = song.orders[cursor.order_pos];
                if (v + 16 < MT_MAX_PATTERNS) {
                    song.orders[cursor.order_pos] = v + 16;
                    song_ensure_pattern(song.orders[cursor.order_pos]);
                    if (song.orders[cursor.order_pos] >= song.patt_count)
                        song.patt_count = song.orders[cursor.order_pos] + 1;
                    mt_mark_song_modified();
                }
            }
            if (rep & KEY_DOWN) {
                u8 v = song.orders[cursor.order_pos];
                if (v >= 16)
                    song.orders[cursor.order_pos] = v - 16;
                else
                    song.orders[cursor.order_pos] = 0;
                mt_mark_song_modified();
            }
        }
        return;
    }

    /* -- Plain direction: cursor movement (with repeat) -- */
    if (rep & KEY_UP) {
        if (cursor.order_pos > 0) cursor.order_pos--;
    }
    if (rep & KEY_DOWN) {
        u8 limit = song.order_count < MT_MAX_ORDERS
                   ? song.order_count : song.order_count - 1;
        if (cursor.order_pos < limit) cursor.order_pos++;
    }

    if (clone_timer > 0) clone_timer--;
}

/* ---- Song screen renderer ---- */
void song_view_draw(u8 *top_fb, u8 *bot_fb)
{
    font_clear(top_fb, PAL_BG);

    /* Row 0: header */
    font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(top_fb, 0, 0, PAL_TEXT, "SONG ARRANGEMENT");
    font_printf(top_fb, font_scale_col(30), 0, PAL_GRAY,
                "Len:%02X  Rpt:%02X",
                song.order_count, song.repeat_position);

    /* Row 1: column headers */
    font_fill_row(top_fb, 1, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 1, 1, "Pos: Pat", PAL_GRAY);

    /* Order list in a scrolling viewport. Cursor-centered via margin so
     * the user always has context above and below. */
    static ScrollView sv_song = { .row_y = 2, .margin = 4 };
    sv_song.row_height = font_scale_row(30) - 2;
    int append_row = (song.order_count < MT_MAX_ORDERS) ? 1 : 0;
    sv_song.total      = (int)song.order_count + append_row;
    sv_song.cursor     = (int)cursor.order_pos;
    scroll_view_follow(&sv_song);

    int sv_first, sv_last;
    scroll_view_visible(&sv_song, &sv_first, &sv_last);

    /* Fill the visible window; any leftover rows below content are
     * shown as empty slots so the grid height stays stable. */
    for (int v = 0; v < sv_song.row_height; v++) {
        int i = sv_first + v;
        int scr_row = sv_song.row_y + v;

        if (i >= sv_last) {
            font_fill_row(top_fb, scr_row, 0, FONT_COLS, PAL_BG);
            font_puts(top_fb, 1, scr_row, "--: --", PAL_DIM);
            continue;
        }

        bool is_cursor = (i == (int)cursor.order_pos);
        bool is_playing = (cursor.playing && i == (int)cursor.play_order);

        if (i >= (int)song.order_count) {
            /* Append slot: show as empty but with cursor highlight */
            if (is_cursor)
                font_fill_row(top_fb, scr_row, 0, 20, PAL_ROW_CURSOR);
            else
                font_fill_row(top_fb, scr_row, 0, FONT_COLS, PAL_BG);
            font_printf(top_fb, 1, scr_row,
                        is_cursor ? PAL_WHITE : PAL_DIM,
                        "%02X: --", i);
            continue;
        }
        bool is_repeat = (i == (int)song.repeat_position);
        u8 pat_val = song.orders[i];
        bool is_empty = (pat_val == ORDER_EMPTY);

        /* Selection range check */
        bool in_sel = false;
        if (cursor.selecting) {
            int s0 = cursor.sel_start_row;
            int s1 = cursor.order_pos;
            if (s0 > s1) { int t = s0; s0 = s1; s1 = t; }
            in_sel = (i >= s0 && i <= s1);
        }

        /* Background highlight */
        if (is_cursor)
            font_fill_row(top_fb, scr_row, 0, 20, PAL_ROW_CURSOR);
        else if (in_sel)
            font_fill_row(top_fb, scr_row, 0, 20, PAL_SEL_BG);
        else if (is_playing)
            font_fill_row(top_fb, scr_row, 0, 20, PAL_PLAY_BG);

        /* Position number */
        u8 pos_color = is_cursor ? PAL_WHITE :
                       is_repeat ? PAL_ORANGE : PAL_GRAY;
        font_printf(top_fb, 1, scr_row, pos_color, "%02X:", i);

        /* Pattern number or "--" for empty */
        if (is_empty) {
            font_puts(top_fb, 5, scr_row, "--",
                      is_cursor ? PAL_WHITE : PAL_DIM);
        } else {
            u8 pat_color = is_cursor ? PAL_WHITE : PAL_TEXT;
            font_printf(top_fb, 5, scr_row, pat_color, "%02X", pat_val);
        }

        /* Repeat marker */
        if (is_repeat)
            font_putc(top_fb, 8, scr_row, 'R', PAL_ORANGE);

        /* Playback marker */
        if (is_playing)
            font_putc(top_fb, 9, scr_row, '>', PAL_PLAY);
    }

    scroll_view_draw_scrollbar(&sv_song, top_fb, FONT_COLS - 1);

    /* Footer help (SMALL row 30) */
    int help_row = font_scale_row(30);
    int trow     = font_scale_row(31);
    font_fill_row(top_fb, help_row, 0, FONT_COLS, PAL_HEADER_BG);
    if (cursor.selecting)
        font_puts(top_fb, 0, help_row,
                  "[SEL] B:copy A+B:cut  SEL+A:paste", PAL_ORANGE);
    else
        font_puts(top_fb, 0, help_row,
                  "A+LR:pat AA:clone Y:ins B+A:del SEL+B:sel", PAL_DIM);

    /* Transport row (SMALL row 31) */
    font_fill_row(top_fb, trow, 0, FONT_COLS, PAL_HEADER_BG);
    u8 cur_pat = (cursor.order_pos < song.order_count) ?
                 song.orders[cursor.order_pos] : ORDER_EMPTY;
    if (cur_pat == ORDER_EMPTY)
        font_printf(top_fb, 0, trow, PAL_DIM, "Pos:%02X Pat:--",
                    cursor.order_pos);
    else
        font_printf(top_fb, 0, trow, PAL_DIM, "Pos:%02X Pat:%02X",
                    cursor.order_pos, cur_pat);
    if (cursor.playing)
        font_puts(top_fb, font_scale_col(30), trow, "PLAYING", PAL_PLAY);
    else
        font_puts(top_fb, font_scale_col(30), trow,
                  "SEL+R:in  B+UD:warp", PAL_DIM);

    (void)bot_fb;  /* song view only draws on top screen */
}
