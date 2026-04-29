/*
 * pattern_view.c — Pattern grid renderer.
 *
 * Top screen layout (64 cols x 32 rows at 4x6 font):
 *   Row 0:      Status bar
 *   Row 1:      Column headers
 *   Rows 2-29:  Pattern data (28 visible rows)
 *   Row 30:     Footer (edit state)
 *   Row 31:     Transport bar
 */

#include "pattern_view.h"
#include "screen.h"
#include "font.h"
#include "song.h"
#include "playback.h"
#include "memtrack.h"
#include "effects.h"
#include "navigation.h"
#include "clipboard.h"
#include "undo.h"
#include "mixer_view.h"
#include <stdio.h>
#include <string.h>

/* Shared globals defined in main.c */
extern char status_msg[64];
extern int  status_timer;

/* Transport helpers defined in main.c — called from START handling below. */
void stop_playback_all(void);
void start_pattern_loop(void);

/* The global cursor lives in editor_state.c. */

#define DATA_ROW_START  2
/* Data rows span from DATA_ROW_START to footer row (SMALL 30).
 * Use font_scale_row(30) so BIG mode (rows 22/23 for footer/transport)
 * shrinks the data window correctly. Accessed via data_row_count(). */
static inline int data_row_count(void) { return font_scale_row(30) - DATA_ROW_START; }

/* Resize confirmation state: when the user shrinks a pattern via X+L,
 * we set a target and wait for a second X+L press to confirm. */
static u16  resize_pending_nrows = 0;
static u8   resize_confirm_timer = 0;

/* Selection helpers: compute normalized min/max for active selection */
static bool sel_row_in_range(int pat_row)
{
    if (!cursor.selecting) return false;
    int r0 = cursor.sel_start_row;
    int r1 = cursor.row;
    if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
    return pat_row >= r0 && pat_row <= r1;
}

static bool sel_ch_in_range(int ch)
{
    if (!cursor.selecting) return false;
    int c0 = cursor.sel_start_col;
    int c1 = cursor.channel;
    if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }
    return ch >= c0 && ch <= c1;
}

/* Hex lookup for fast 2-digit formatting (replaces font_printf "%02X") */
static const char hex_chars[] = "0123456789ABCDEF";

/* Note names */
static const char note_names[12][3] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

static void format_note(char *buf, u8 note)
{
    if (note == 250) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '-';
    } else if (note == 254) {
        buf[0] = '='; buf[1] = '='; buf[2] = '=';
    } else if (note == 255) {
        buf[0] = '^'; buf[1] = '^'; buf[2] = '^';
    } else {
        u8 oct = note / 12;
        u8 sem = note % 12;
        buf[0] = note_names[sem][0];
        buf[1] = note_names[sem][1];
        buf[2] = '0' + oct;
    }
    buf[3] = '\0';
}

static void format_hex2(char *buf, u8 val)
{
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[val >> 4];
    buf[1] = hex[val & 0xF];
    buf[2] = '\0';
}

/* How many channels fit in the overview row given the current font
 * mode. Each channel cell is 7 cols (note+inst+sep), prefix is 4 cols.
 * SMALL mode: (64 - 4) / 7 = 8. BIG mode: (42 - 4) / 7 = 5. */
static inline int overview_visible_channels(void)
{
    int n = (FONT_COLS - 4) / 7;
    if (n > 8) n = 8;
    if (n < 1) n = 1;
    return n;
}

int pattern_view_overview_visible_channels(void)
{
    return overview_visible_channels();
}

/* ------------------------------------------------------------------ */
/* Overview mode: up to 8 channels, note + instrument                 */
/* ------------------------------------------------------------------ */

static void draw_overview_row(u8 *fb, int screen_row, int pat_row,
                              MT_Pattern *pat, int ch_start, int ch_count,
                              bool is_cursor_row, bool is_play_row)
{
    /* Background color for this row */
    u8 bg;
    if (is_cursor_row)
        bg = PAL_ROW_CURSOR;
    else if (is_play_row)
        bg = PAL_PLAY_BG;
    else if ((pat_row & 3) == 0)
        bg = PAL_ROW_BEAT;
    else
        bg = PAL_ROW_EVEN;

    /* 4 cols (row#/marker/sep) + 8 channels * 7 cols = 60 */
    font_fill_row(fb, screen_row, 0, 60, bg);

    /* Row number */
    char rowstr[4];
    format_hex2(rowstr, (u8)pat_row);
    u8 row_color = is_cursor_row ? PAL_WHITE : PAL_GRAY;
    font_puts(fb, 0, screen_row, rowstr, row_color);

    /* Row marker: cursor or playback position */
    if (is_cursor_row)
        font_putc(fb, 2, screen_row, '>', PAL_WHITE);
    else if (is_play_row)
        font_putc(fb, 2, screen_row, '>', PAL_PLAY);

    /* Separator */
    font_putc(fb, 3, screen_row, '|', PAL_DIM);

    /* Channel data */
    MT_Cell *row_data = NULL;
    if (pat && pat_row < pat->nrows)
        row_data = &pat->cells[pat_row * pat->ncols];

    u32 mute_mask = playback_get_mute_mask();

    for (int ci = 0; ci < ch_count && ci < 8; ci++) {
        int ch = ch_start + ci;
        int col_base = 4 + ci * 7;
        bool is_cursor_ch = is_cursor_row && (ch == cursor.channel);
        bool ch_muted = (mute_mask >> ch) & 1;
        bool is_selected = sel_row_in_range(pat_row) && sel_ch_in_range(ch);

        /* Fill selection background for this channel cell. font_fill_row
         * takes (col_start, col_end) — passing 6 as a width here would
         * give col_end < col_start for any channel past 0, which produces
         * a negative memset length and writes hundreds of megabytes of
         * background color across both VRAM banks. The cell content
         * occupies col_base..col_base+5; col_base+6 is the '|' separator
         * which should stay dim. */
        if (is_selected && !is_cursor_ch)
            font_fill_row(fb, screen_row, col_base, col_base + 6, PAL_SEL_BG);

        u8 cell_bg = (is_selected && !is_cursor_ch) ? PAL_SEL_BG : bg;

        if (row_data && ch < MT_MAX_CHANNELS) {
            MT_Cell *cell = &row_data[ch];
            char note_str[4];
            char inst_str[3];

            format_note(note_str, cell->note);

            if (cell->inst > 0)
                format_hex2(inst_str, cell->inst);
            else {
                inst_str[0] = '-'; inst_str[1] = '-'; inst_str[2] = '\0';
            }

            u8 nc = (cell->note != 250) ? PAL_NOTE : PAL_DIM;
            u8 ic = (cell->inst > 0) ? PAL_INST : PAL_DIM;

            if (is_cursor_ch) { nc = PAL_WHITE; ic = PAL_WHITE; }
            else if (is_selected) { nc = PAL_ORANGE; ic = PAL_ORANGE; }
            else if (ch_muted) { nc = PAL_DIM; ic = PAL_DIM; }

            font_puts(fb, col_base, screen_row, note_str, nc);
            font_putc(fb, col_base + 3, screen_row, ' ', cell_bg);
            font_puts(fb, col_base + 4, screen_row, inst_str, ic);
        } else {
            u8 dc = is_selected ? PAL_ORANGE : PAL_DIM;
            font_puts(fb, col_base, screen_row, "--- --", dc);
        }
        font_putc(fb, col_base + 6, screen_row, '|', PAL_DIM);
    }
}

/* ------------------------------------------------------------------ */
/* Inside mode: single channel, all fields                            */
/* ------------------------------------------------------------------ */

static void draw_inside_row(u8 *fb, int screen_row, int pat_row,
                            MT_Pattern *pat, int ch,
                            bool is_cursor_row, bool is_play_row)
{
    bool is_selected = sel_row_in_range(pat_row) && !is_cursor_row;

    u8 bg;
    if (is_cursor_row)
        bg = PAL_ROW_CURSOR;
    else if (is_selected)
        bg = PAL_SEL_BG;
    else if (is_play_row)
        bg = PAL_PLAY_BG;
    else if ((pat_row & 3) == 0)
        bg = PAL_ROW_BEAT;
    else
        bg = PAL_ROW_EVEN;

    /* Inside mode uses cols 0-24 */
    font_fill_row(fb, screen_row, 0, 25, bg);

    /* Row number */
    char tmp[4];
    format_hex2(tmp, (u8)pat_row);
    u8 rn_color = is_cursor_row ? PAL_WHITE : (is_selected ? PAL_ORANGE : PAL_GRAY);
    font_puts(fb, 0, screen_row, tmp, rn_color);
    if (is_cursor_row)
        font_putc(fb, 2, screen_row, '>', PAL_WHITE);
    else if (is_play_row)
        font_putc(fb, 2, screen_row, '>', PAL_PLAY);
    font_putc(fb, 3, screen_row, '|', PAL_DIM);

    MT_Cell *cell = NULL;
    if (pat && pat_row < pat->nrows && ch < MT_MAX_CHANNELS)
        cell = MT_CELL(pat, pat_row, ch);

    if (!cell) return;

    bool ch_muted = (playback_get_mute_mask() >> ch) & 1;

    /* Note (cols 4-7) */
    char note_str[4];
    format_note(note_str, cell->note);
    u8 nc = (cell->note != 250) ? PAL_NOTE : PAL_DIM;
    if (is_cursor_row && cursor.column == 0) nc = PAL_WHITE;
    else if (is_selected) nc = PAL_ORANGE;
    else if (ch_muted) nc = PAL_DIM;
    font_puts(fb, 4, screen_row, note_str, nc);
    font_putc(fb, 7, screen_row, ' ', bg);

    font_putc(fb, 8, screen_row, '|', PAL_DIM);

    /* Instrument (cols 9-11) */
    char inst_str[3];
    if (cell->inst > 0)
        format_hex2(inst_str, cell->inst);
    else { inst_str[0] = '-'; inst_str[1] = '-'; inst_str[2] = '\0'; }
    u8 ic = (cell->inst > 0) ? PAL_INST : PAL_DIM;
    if (is_cursor_row && cursor.column == 1) ic = PAL_WHITE;
    else if (is_selected) ic = PAL_ORANGE;
    else if (ch_muted) ic = PAL_DIM;
    font_putc(fb, 9, screen_row, ' ', bg);
    font_puts(fb, 10, screen_row, inst_str, ic);
    font_putc(fb, 12, screen_row, '|', PAL_DIM);

    /* Volume (cols 13-15) */
    char vol_str[3];
    if (cell->vol > 0)
        format_hex2(vol_str, cell->vol);
    else { vol_str[0] = '-'; vol_str[1] = '-'; vol_str[2] = '\0'; }
    u8 vc = (cell->vol > 0) ? PAL_TEXT : PAL_DIM;
    if (is_cursor_row && cursor.column == 2) vc = PAL_WHITE;
    else if (is_selected) vc = PAL_ORANGE;
    else if (ch_muted) vc = PAL_DIM;
    font_putc(fb, 13, screen_row, ' ', bg);
    font_puts(fb, 14, screen_row, vol_str, vc);
    font_putc(fb, 16, screen_row, '|', PAL_DIM);

    /* Effect (cols 17-19) */
    char fx_str[3];
    if (cell->fx > 0)
        format_hex2(fx_str, cell->fx);
    else { fx_str[0] = '-'; fx_str[1] = '-'; fx_str[2] = '\0'; }
    u8 fc = (cell->fx > 0) ? PAL_EFFECT : PAL_DIM;
    if (is_cursor_row && cursor.column == 3) fc = PAL_WHITE;
    else if (is_selected) fc = PAL_ORANGE;
    else if (ch_muted) fc = PAL_DIM;
    font_putc(fb, 17, screen_row, ' ', bg);
    font_puts(fb, 18, screen_row, fx_str, fc);
    font_putc(fb, 20, screen_row, '|', PAL_DIM);

    /* Parameter (cols 21-23) */
    char pm_str[3];
    if (cell->fx > 0 || cell->param > 0)
        format_hex2(pm_str, cell->param);
    else { pm_str[0] = '-'; pm_str[1] = '-'; pm_str[2] = '\0'; }
    u8 pc = (cell->fx > 0 || cell->param > 0) ? PAL_PARAM : PAL_DIM;
    if (is_cursor_row && cursor.column == 4) pc = PAL_WHITE;
    else if (is_selected) pc = PAL_ORANGE;
    else if (ch_muted) pc = PAL_DIM;
    font_putc(fb, 21, screen_row, ' ', bg);
    font_puts(fb, 22, screen_row, pm_str, pc);
    font_putc(fb, 24, screen_row, '|', PAL_DIM);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void pattern_view_draw_status(u8 *fb)
{
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 0, "maxtracker", PAL_TEXT);

    u8 pat_idx = (cursor.order_pos < song.order_count) ?
                  song.orders[cursor.order_pos] : 0;
    font_printf(fb, 11, 0, PAL_GRAY, "Ord:%02X/%02X P:%02X S:%02d T:%03d O:%d I:%02X",
                cursor.order_pos, song.order_count - 1,
                pat_idx, song.initial_speed, song.initial_tempo,
                cursor.octave, cursor.instrument);

    if (cursor.inside)
        font_printf(fb, font_scale_col(56), 0, PAL_TEXT,
                    "CH:%02d", cursor.channel + 1);
    else {
        int vis = overview_visible_channels();
        font_printf(fb, font_scale_col(56), 0, PAL_TEXT, "[%d-%d]",
                    cursor.ch_group + 1, cursor.ch_group + vis);
    }
}

void pattern_view_draw_transport(u8 *fb)
{
    int trow = font_scale_row(31);
    font_fill_row(fb, trow, 0, FONT_COLS, PAL_HEADER_BG);

    u8 pi = (cursor.order_pos < song.order_count)
            ? song.orders[cursor.order_pos] : 0;
    MT_Pattern *pp = (pi < MT_MAX_PATTERNS) ? song.patterns[pi] : NULL;
    int nrows = pp ? pp->nrows : 64;

    if (cursor.playing) {
        const char *mode_str;
        switch (play_mode) {
        case PLAY_PATTERN_ALL:  mode_str = "PAT";  break;
        case PLAY_PATTERN_SOLO: mode_str = "SOLO"; break;
        case PLAY_SONG:         mode_str = "SONG"; break;
        default:                mode_str = "PLAY"; break;
        }
        font_puts(fb, 0, trow, mode_str, PAL_PLAY);
        font_printf(fb, 5, trow, PAL_GRAY, "R:%02X S:%d L:%02X PR:%02X",
                    cursor.row, cursor.step, nrows, cursor.play_row);
    } else {
        font_puts(fb, 0, trow, "STOP", PAL_RED);
        font_printf(fb, 5, trow, PAL_GRAY, "R:%02X S:%d L:%02X",
                    cursor.row, cursor.step, nrows);
    }

    if (cursor.follow)
        font_puts(fb, font_scale_col(36), trow, "[FOL]", PAL_PLAY);

    if (cursor.selecting)
        font_puts(fb, font_scale_col(42), trow,
                  "[SEL] B:copy A+B:cut", PAL_ORANGE);
    else if (cursor.inside)
        font_puts(fb, font_scale_col(40), trow,
                  "SEL+L:back  X+L/R:resize", PAL_DIM);
    else
        font_puts(fb, font_scale_col(40), trow,
                  "SEL+R:in  X+L/R:resize", PAL_DIM);
}

void pattern_view_draw(u8 *fb)
{
    pattern_view_draw_status(fb);

    /* Column headers (row 1) */
    font_fill_row(fb, 1, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 1, "RW", PAL_GRAY);
    font_putc(fb, 3, 1, '|', PAL_DIM);

    u32 mute_mask = playback_get_mute_mask();

    if (cursor.inside) {
        bool ch_muted = (mute_mask >> cursor.channel) & 1;
        font_puts(fb, 4, 1, "Note", PAL_NOTE);
        font_putc(fb, 8, 1, '|', PAL_DIM);
        font_puts(fb, 9, 1, "Ins", PAL_INST);
        font_putc(fb, 12, 1, '|', PAL_DIM);
        font_puts(fb, 13, 1, "Vol", PAL_TEXT);
        font_putc(fb, 16, 1, '|', PAL_DIM);
        font_puts(fb, 17, 1, "Eff", PAL_EFFECT);
        font_putc(fb, 20, 1, '|', PAL_DIM);
        font_puts(fb, 21, 1, "Prm", PAL_PARAM);
        font_putc(fb, 24, 1, '|', PAL_DIM);
        if (ch_muted)
            font_puts(fb, font_scale_col(26), 1, "MUTED", PAL_RED);
    } else {
        int nch = overview_visible_channels();
        for (int ci = 0; ci < nch; ci++) {
            int ch = cursor.ch_group + ci;
            int col_base = 4 + ci * 7;
            bool ch_muted = (mute_mask >> ch) & 1;
            u8 chnum = ch + 1;
            char hdr[7] = {' ','C','H', '0'+(chnum/10), '0'+(chnum%10),
                           ch_muted ? 'M' : ' ', '\0'};
            font_puts(fb, col_base, 1, hdr, ch_muted ? PAL_DIM : PAL_GRAY);
            font_putc(fb, col_base + 6, 1, '|', PAL_DIM);
        }
    }

    /* Get current pattern from order table */
    u8 pat_idx = 0;
    if (cursor.order_pos < song.order_count &&
        song.orders[cursor.order_pos] < song.patt_count)
        pat_idx = song.orders[cursor.order_pos];
    MT_Pattern *pat = song.patterns[pat_idx];

    int nrows = pat ? pat->nrows : 64;

    /* Center cursor in the data area */
    int scroll_top = (int)cursor.row - data_row_count() / 2;

    for (int i = 0; i < data_row_count(); i++) {
        int pat_row = scroll_top + i;
        int screen_row = DATA_ROW_START + i;

        /* Wrap or blank out-of-range rows */
        if (pat_row < 0 || pat_row >= nrows) {
            font_fill_row(fb, screen_row, 0, cursor.inside ? 25 : 60, PAL_BG);
            continue;
        }

        bool is_cursor = (pat_row == cursor.row);
        bool is_play   = cursor.playing &&
                         cursor.play_order == cursor.order_pos &&
                         pat_row == cursor.play_row;

        if (cursor.inside)
            draw_inside_row(fb, screen_row, pat_row, pat, cursor.channel,
                           is_cursor, is_play);
        else
            draw_overview_row(fb, screen_row, pat_row, pat,
                             cursor.ch_group,
                             overview_visible_channels(),
                             is_cursor, is_play);
    }

    /* Footer (SMALL row 30) */
    int footer_row = font_scale_row(30);
    font_fill_row(fb, footer_row, 0, FONT_COLS, PAL_BG);
    char note_str[4];
    u8 preview_note = cursor.octave * 12 + cursor.semitone;
    format_note(note_str, preview_note);
    font_printf(fb, 0, footer_row, PAL_TEXT,
                "Step:%d  Note:%s  Ins:%02X  Oct:%d",
                cursor.step, note_str, cursor.instrument, cursor.octave);

    pattern_view_draw_transport(fb);
}

/* ------------------------------------------------------------------ */
/* Bottom-screen panel for pattern mode                                */
/*                                                                     */
/* Instrument list (col 1, rows 2-9), channel mute grid (cols 28-59,  */
/* rows 2-9), playback status (row 12), memory usage bar (rows 28-29), */
/* and transport/info footer (rows 30-31). Extracted from main.c.      */
/*                                                                     */
/* Dirty-flag optimization: the bottom panel rarely changes (only on   */
/* cursor move, mute toggle, playback state change). We snapshot the   */
/* relevant state and skip the full redraw when nothing changed.       */
/* ------------------------------------------------------------------ */

static struct {
    u8  instrument;
    u8  channel;
    u8  order_pos;
    u8  order_count;
    u8  play_row;
    u8  play_order;
    u32 mute_mask;
    bool playing;
    PlaybackMode play_mode;
    u8  hint_fx;       /* fx code shown in the effect-hint row, 0 = none */
    bool valid;
} bot_prev;

void pattern_view_invalidate_bottom(void)
{
    bot_prev.valid = false;
}

void pattern_view_draw_bottom(u8 *fb)
{
    u32 mute_mask = playback_get_mute_mask();

    /* Effect hint: if cursor is on the fx or param column and the cell
     * carries an effect, surface "<letter> <name>" on the otherwise-idle
     * play_row. We re-read the cell via editor_get_current_pattern so
     * out-of-bounds cursors during pattern swaps stay safe. */
    u8 hint_fx = 0;
    if (cursor.inside && (cursor.column == 3 || cursor.column == 4)) {
        int nrows_h;
        MT_Pattern *hp = editor_get_current_pattern(&nrows_h);
        if (hp && cursor.row < hp->nrows && cursor.channel < hp->ncols) {
            MT_Cell *hc = MT_CELL(hp, cursor.row, cursor.channel);
            if (hc) hint_fx = hc->fx;
        }
    }

    bool dirty = !bot_prev.valid
        || bot_prev.hint_fx != hint_fx
        || bot_prev.instrument  != cursor.instrument
        || bot_prev.channel     != cursor.channel
        || bot_prev.order_pos   != cursor.order_pos
        || bot_prev.order_count != song.order_count
        || bot_prev.mute_mask   != mute_mask
        || bot_prev.playing     != cursor.playing
        || bot_prev.play_mode   != play_mode
        || (cursor.playing && (bot_prev.play_row   != cursor.play_row
                            || bot_prev.play_order != cursor.play_order));

    if (!dirty) return;

    bot_prev.instrument  = cursor.instrument;
    bot_prev.channel     = cursor.channel;
    bot_prev.order_pos   = cursor.order_pos;
    bot_prev.order_count = song.order_count;
    bot_prev.mute_mask   = mute_mask;
    bot_prev.playing     = cursor.playing;
    bot_prev.play_mode   = play_mode;
    bot_prev.play_row    = cursor.play_row;
    bot_prev.play_order  = cursor.play_order;
    bot_prev.hint_fx     = hint_fx;
    bot_prev.valid       = true;

    font_clear(fb, PAL_BG);

    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 0, "INSTRUMENTS", PAL_TEXT);
    /* MUTES panel starts at SMALL-col 28; scale so BIG (42 cols) fits.
     * Step must leave at least 1 col of gap between 2-char labels, i.e.
     * minimum step = 3. In SMALL this resolves to the original 4. */
    int mutes_col0 = font_scale_col(28);
    int mute_step  = font_scale_col(4);
    if (mute_step < 3) mute_step = 3;
    font_puts(fb, mutes_col0, 0, "MUTES", PAL_TEXT);

    for (int i = 0; i < 8 && i < song.inst_count; i++) {
        u8 color = (i + 1 == cursor.instrument) ? PAL_WHITE : PAL_GRAY;
        char hex[3] = { hex_chars[(i+1) >> 4], hex_chars[(i+1) & 0xF], '\0' };
        font_puts(fb, 1, 2 + i, hex, color);
    }

    int total_ch = song.channel_count;
    if (total_ch > MT_MAX_CHANNELS) total_ch = MT_MAX_CHANNELS;
    for (int ch = 0; ch < total_ch; ch++) {
        int row_idx   = ch / 8;
        int col_idx   = ch % 8;
        int num_row   = 2 + row_idx * 2;
        int state_row = num_row + 1;
        int col       = mutes_col0 + col_idx * mute_step;
        bool muted = (mute_mask >> ch) & 1;
        bool is_cursor_ch = (ch == cursor.channel);
        u8 num_color = is_cursor_ch ? PAL_WHITE : PAL_GRAY;
        u8 chnum = ch + 1;
        char num[3] = { (chnum >= 10) ? ('0' + chnum/10) : ' ',
                        '0' + (chnum % 10), '\0' };
        font_puts(fb, col, num_row, num, num_color);
        if (muted)
            font_puts(fb, col, state_row, "MT", PAL_RED);
        else
            font_puts(fb, col, state_row, "ON", PAL_PLAY);
    }

    int ram_row   = font_scale_row(28);
    int play_row  = ram_row - 1;   /* sit directly above the RAM line */
    int hint_row1 = play_row - 1;  /* FX description line 1             */
    int hint_row0 = play_row - 2;  /* FX header (letter + name)          */
    int bar_row  = font_scale_row(29);
    int foot_row = font_scale_row(30);
    int tport_row = font_scale_row(31);

    /* Three-row FX hint block when not playing:
     *   hint_row0: "<letter> <name>"                 (header, colored)
     *   hint_row1: description line 1                (word-wrapped)
     *   play_row:  description line 2                (wrap continuation)
     * When playing, hint rows stay clear and play_row shows "Playing..."
     * as before. All three are cleared unconditionally so stale text
     * never survives a cursor move, a font-mode flip, or a play toggle. */
    font_fill_row(fb, hint_row0, 0, FONT_COLS, PAL_BG);
    font_fill_row(fb, hint_row1, 0, FONT_COLS, PAL_BG);
    font_fill_row(fb, play_row,  0, FONT_COLS, PAL_BG);
    if (!cursor.playing && hint_fx != 0) {
        const MT_EffectInfo *ei = effect_info(hint_fx);
        if (ei) {
            int cols = FONT_COLS;

            char head[40];
            snprintf(head, sizeof(head), "%c %s", ei->letter, ei->name);
            font_puts(fb, 0, hint_row0, head, PAL_EFFECT);

            const char *desc = ei->description ? ei->description : "";

            /* Wrap `desc` into line1 (<= cols) and line2 (<= cols), breaking
             * at the last space at or before the column limit. Anything
             * past the second line is truncated — descriptions longer than
             * 2 * cols chars should be tightened in effects.def instead. */
            char line1[96];
            char line2[96];
            line1[0] = line2[0] = '\0';

            int dlen = (int)strlen(desc);
            if (dlen <= cols) {
                int cap = (dlen < (int)sizeof(line1)) ? dlen : (int)sizeof(line1) - 1;
                memcpy(line1, desc, cap);
                line1[cap] = '\0';
            } else {
                int brk = cols;
                for (int i = cols; i > 0; i--) {
                    if (desc[i] == ' ') { brk = i; break; }
                }
                int cap1 = (brk < (int)sizeof(line1)) ? brk : (int)sizeof(line1) - 1;
                memcpy(line1, desc, cap1);
                line1[cap1] = '\0';

                const char *rest = desc + brk;
                while (*rest == ' ') rest++;

                int rlen = (int)strlen(rest);
                int cap2 = (rlen < cols) ? rlen : cols;
                if (cap2 >= (int)sizeof(line2)) cap2 = (int)sizeof(line2) - 1;
                memcpy(line2, rest, cap2);
                line2[cap2] = '\0';
            }

            font_puts(fb, 0, hint_row1, line1, PAL_DIM);
            if (line2[0])
                font_puts(fb, 0, play_row, line2, PAL_DIM);
        } else {
            char hintbuf[16];
            snprintf(hintbuf, sizeof(hintbuf), "FX %02X ?", hint_fx);
            font_puts(fb, 0, hint_row0, hintbuf, PAL_DIM);
        }
    } else if (cursor.playing) {
        char pbuf[28];
        char *p = pbuf;
        memcpy(p, "Playing  Ord:", 13); p += 13;
        *p++ = hex_chars[cursor.play_order >> 4];
        *p++ = hex_chars[cursor.play_order & 0xF];
        memcpy(p, " Row:", 5); p += 5;
        *p++ = hex_chars[cursor.play_row >> 4];
        *p++ = hex_chars[cursor.play_row & 0xF];
        *p = '\0';
        font_puts(fb, 0, play_row, pbuf, PAL_PLAY);
    }

    {
        MT_MemUsage mem;
        mt_mem_usage(&mem);
        u32 used_kb = mem.total / 1024;
        u32 avail_kb = mem.available / 1024;
        u32 pct = (mem.total * 100) / mem.available;
        u8 bar_color = (pct > 90) ? PAL_RED : (pct > 70) ? PAL_ORANGE : PAL_PLAY;

        font_printf(fb, 0, ram_row, PAL_GRAY,
                    "RAM %luKB/%luKB (%lu%%) P:%d S:%d",
                    (unsigned long)used_kb, (unsigned long)avail_kb,
                    (unsigned long)pct, mem.patt_count, mem.samp_count);

        int bar_width = FONT_COLS - 4;
        int bar_len = (int)(pct * bar_width / 100);
        if (bar_len > bar_width) bar_len = bar_width;
        for (int x = 0; x < bar_width; x++) {
            font_putc(fb, 2 + x, bar_row,
                      (x < bar_len) ? '=' : '.',
                      (x < bar_len) ? bar_color : PAL_DIM);
        }
    }

    font_fill_row(fb, foot_row, 0, FONT_COLS, PAL_HEADER_BG);
    {
        u8 pi = (cursor.order_pos < song.order_count) ?
                song.orders[cursor.order_pos] : 0;
        MT_Pattern *pp = (pi < MT_MAX_PATTERNS) ? song.patterns[pi] : NULL;
        int nr = pp ? pp->nrows : 0;
        font_printf(fb, 0, foot_row, PAL_DIM,
                    "Ord:%02X/%02X  Pat:%02X  Len:%02X  Ch:%d  X+L/R:resize",
                    cursor.order_pos, song.order_count - 1,
                    pi, nr, song.channel_count);
    }
    font_fill_row(fb, tport_row, 0, FONT_COLS, PAL_HEADER_BG);
    if (cursor.playing) {
        const char *mode_str;
        switch (play_mode) {
        case PLAY_PATTERN_ALL:  mode_str = "PAT LOOP"; break;
        case PLAY_PATTERN_SOLO: mode_str = "SOLO";     break;
        case PLAY_SONG:         mode_str = "SONG";     break;
        default:                mode_str = "PLAYING";  break;
        }
        font_puts(fb, 0, tport_row, mode_str, PAL_PLAY);
        font_puts(fb, 12, tport_row, "START:stop", PAL_DIM);
    } else {
        font_puts(fb, 0, tport_row, "START:pat  SEL+ST:song", PAL_DIM);
    }
}

/* ================================================================== */
/* Input handling                                                     */
/* ================================================================== */

static void cursor_advance(int nrows)
{
    int next = cursor.row + cursor.step;
    if (next >= nrows) next = nrows - 1;
    cursor.row = (u8)next;
}

static MT_Cell *cursor_cell(MT_Pattern *pat)
{
    if (!pat) return NULL;
    if (cursor.row >= pat->nrows) return NULL;
    if (cursor.channel >= MT_MAX_CHANNELS) return NULL;
    return MT_CELL(pat, cursor.row, cursor.channel);
}

void pattern_view_input(u32 kd, u32 kh)
{
    u32 down = kd;
    u32 held = kh;

    if (resize_confirm_timer > 0) resize_confirm_timer--;

    if (navigation_handle_shift(down, held)) return;

    /* Follow mode: when playing, block all input except START (stop)
     * and SELECT (navigation, handled above by navigation_handle_shift) */
    if (cursor.follow && cursor.playing) {
        if (down & KEY_START) {
            /* Fall through to START handler below */
        } else {
            return; /* block all other input */
        }
    }

    int nrows;
    MT_Pattern *pat = editor_get_current_pattern(&nrows);

    /* ---- R modifier: mute/solo controls (both modes) ---- */
    if (held & KEY_R) {
        if (down & KEY_A) {
            bool muted = (playback_get_mute_mask() >> cursor.channel) & 1;
            playback_set_mute(cursor.channel, !muted);
            return;
        }
        if (down & KEY_B) {
            u32 mask = playback_get_mute_mask();
            bool is_solo = true;
            for (int i = 0; i < song.channel_count; i++) {
                if (i != cursor.channel && !((mask >> i) & 1)) {
                    is_solo = false;
                    break;
                }
            }
            if (is_solo) {
                for (int i = 0; i < song.channel_count; i++)
                    playback_set_mute(i, false);
            } else {
                for (int i = 0; i < song.channel_count; i++)
                    playback_set_mute(i, i != cursor.channel);
            }
            return;
        }
    }

    /* ---- L+R together: unmute all channels ---- */
    if (((down & KEY_L) && (held & KEY_R)) ||
        ((down & KEY_R) && (held & KEY_L))) {
        for (int i = 0; i < song.channel_count; i++)
            playback_set_mute(i, false);
        return;
    }

    /* ---- L + LEFT/RIGHT: tempo nudge (LGPT-style) ---- */
    if ((held & KEY_L) && !(held & KEY_R)) {
        u32 rep_l = keysDownRepeat();
        if (rep_l & KEY_LEFT) {
            if (song.initial_tempo > 32) {
                song.initial_tempo--;
                if (cursor.playing) playback_set_tempo(song.initial_tempo);
                snprintf(status_msg, sizeof(status_msg),
                         "Tempo: %d BPM", song.initial_tempo);
                status_timer = 60;
                mt_mark_song_modified();
            }
            return;
        }
        if (rep_l & KEY_RIGHT) {
            if (song.initial_tempo < 255) {
                song.initial_tempo++;
                if (cursor.playing) playback_set_tempo(song.initial_tempo);
                snprintf(status_msg, sizeof(status_msg),
                         "Tempo: %d BPM", song.initial_tempo);
                status_timer = 60;
                mt_mark_song_modified();
            }
            return;
        }
    }

    /* ---- X held: page movement / step change / resize ---- */
    if (held & KEY_X) {
        u32 rep_x = keysDownRepeat();
        if (rep_x & KEY_UP) {
            if (cursor.row >= 16) cursor.row -= 16;
            else cursor.row = 0;
        }
        if (rep_x & KEY_DOWN) {
            if (cursor.row + 16 < nrows) cursor.row += 16;
            else cursor.row = nrows - 1;
        }
        if (down & KEY_LEFT) {
            if (cursor.step > 1) cursor.step /= 2;
        }
        if (down & KEY_RIGHT) {
            if (cursor.step < 16) cursor.step *= 2;
        }

        /* X+L/R (shoulder): resize pattern */
        if ((down & (KEY_L | KEY_R)) && pat &&
            cursor.order_pos < song.order_count) {
            u8 pidx = song.orders[cursor.order_pos];
            if (down & KEY_R) {
                if (nrows < MT_MAX_ROWS) {
                    if (song_resize_pattern(pidx, nrows + 1)) {
                        snprintf(status_msg, sizeof(status_msg),
                                 "Pat %02X: %d rows", pidx, nrows + 1);
                        status_timer = 60;
                        mt_mark_song_modified();
                    }
                }
                resize_confirm_timer = 0;
            }
            if (down & KEY_L) {
                if (nrows > 1) {
                    u16 target = nrows - 1;
                    if (resize_confirm_timer > 0 &&
                        resize_pending_nrows == target) {
                        song_resize_pattern(pidx, target);
                        if (cursor.row >= target)
                            cursor.row = target - 1;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Pat %02X: %d rows", pidx, target);
                        status_timer = 60;
                        mt_mark_song_modified();
                        resize_confirm_timer = 0;
                    } else {
                        resize_pending_nrows = target;
                        resize_confirm_timer = 45;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Shrink to %d rows? X+L again", target);
                        status_timer = 90;
                    }
                }
            }
        }
        return;
    }

    /* ---- Y held: octave / instrument change (both modes) ---- */
    if (held & KEY_Y) {
        u32 rep_y = keysDownRepeat();
        if (rep_y & KEY_UP) {
            if (cursor.octave < 9) cursor.octave++;
        }
        if (rep_y & KEY_DOWN) {
            if (cursor.octave > 0) cursor.octave--;
        }
        if (rep_y & KEY_LEFT) {
            if (cursor.instrument > 1) cursor.instrument--;
        }
        if (rep_y & KEY_RIGHT) {
            if (cursor.instrument < MT_MAX_INSTRUMENTS) cursor.instrument++;
        }
        return;
    }

    /* ---- Transport: START = pattern loop (LSDJ style) ---- */
    if (down & KEY_START) {
        if (cursor.playing) {
            stop_playback_all();
        } else {
            start_pattern_loop();
        }
        return;
    }

    /* ==== INSIDE MODE (single channel, all 5 fields) ==== */
    if (cursor.inside) {
        u32 rep = keysDownRepeat();

        /* ---- B modifier (highest priority) ---- */
        if (held & KEY_B) {
            if (down & KEY_A) {
                if (cursor.selecting && pat) {
                    u8 r0 = cursor.sel_start_row, r1 = cursor.row;
                    u8 c0 = cursor.sel_start_col, c1 = cursor.channel;
                    if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
                    if (c0 > c1) { u8 t = c0; c0 = c1; c1 = t; }
                    u8 pi = editor_get_current_pattern_idx();
                    undo_push_block(pi, r0, r1, c0, c1);
                    clipboard_copy(pat, r0, r1, c0, c1);
                    clipboard_clear_block(pat, r0, r1, c0, c1);
                    cursor.selecting = false;
                    mt_mark_song_modified();
                } else {
                    MT_Cell *cell = cursor_cell(pat);
                    if (cell) {
                        u8 pi = editor_get_current_pattern_idx();
                        undo_push_cell(pi, cursor.row, cursor.channel);
                        switch (cursor.column) {
                        case 0:
                            cell->note  = NOTE_EMPTY;
                            cell->inst  = 0;
                            cell->vol   = 0;
                            cell->fx    = 0;
                            cell->param = 0;
                            playback_stop_preview();
                            break;
                        case 1: cell->inst  = 0; break;
                        case 2: cell->vol   = 0; break;
                        case 3: cell->fx    = 0; break;
                        case 4: cell->param = 0; break;
                        }
                        mt_mark_song_modified();
                    }
                }
                return;
            }
            if ((down & KEY_B) && cursor.selecting) {
                u8 r0 = cursor.sel_start_row, r1 = cursor.row;
                u8 c0 = cursor.sel_start_col, c1 = cursor.channel;
                if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
                if (c0 > c1) { u8 t = c0; c0 = c1; c1 = t; }
                clipboard_copy(pat, r0, r1, c0, c1);
                cursor.selecting = false;
                snprintf(status_msg, sizeof(status_msg),
                         "Copied %dx%d", r1 - r0 + 1, c1 - c0 + 1);
                status_timer = 60;
                return;
            }
            if (rep & KEY_UP) {
                if (cursor.row >= 16) cursor.row -= 16;
                else cursor.row = 0;
            }
            if (rep & KEY_DOWN) {
                if (cursor.row + 16 < nrows) cursor.row += 16;
                else cursor.row = nrows - 1;
            }
            if (rep & KEY_LEFT) {
                if (cursor.order_pos > 0) {
                    cursor.order_pos--;
                    MT_Pattern *np = song.patterns[song.orders[cursor.order_pos]];
                    int nr = np ? np->nrows : 64;
                    if (cursor.row >= nr) cursor.row = nr - 1;
                }
            }
            if (rep & KEY_RIGHT) {
                if (cursor.order_pos + 1 < song.order_count) {
                    cursor.order_pos++;
                    MT_Pattern *np = song.patterns[song.orders[cursor.order_pos]];
                    int nr = np ? np->nrows : 64;
                    if (cursor.row >= nr) cursor.row = nr - 1;
                }
            }
            return;
        }

        /* ---- A modifier ---- */
        if (held & KEY_A) {
            if ((down & KEY_START) && cursor.column == 0) {
                MT_Cell *cell = cursor_cell(pat);
                if (cell) {
                    u8 pi = editor_get_current_pattern_idx();
                    undo_push_cell(pi, cursor.row, cursor.channel);
                    cell->note = NOTE_OFF;
                    mt_mark_song_modified();
                    cursor_advance(nrows);
                }
                return;
            }

            if ((down & KEY_A) && !cursor.selecting) {
                MT_Cell *cell = cursor_cell(pat);
                if (cell) {
                    u8 pi = editor_get_current_pattern_idx();
                    bool changed = false;
                    if (cursor.column == 0) {
                        changed = note_slot_a_press(cell, pi, cursor.row, cursor.channel);
                    } else if (cursor.column == 1) {
                        changed = inst_slot_a_press(cell, pi, cursor.row, cursor.channel);
                    }
                    if (changed) { mt_mark_song_modified(); }
                }
            }

            MT_Cell *cell = cursor_cell(pat);
            if (cell) {
                if (cursor.column == 0) {
                    if (cell->note < 120) {
                        if (rep & KEY_UP) {
                            u8 pi = editor_get_current_pattern_idx();
                            undo_push_cell(pi, cursor.row, cursor.channel);
                            if (cell->note + 12 <= 119)
                                cell->note += 12;
                            else
                                cell->note = 119;
                        }
                        if (rep & KEY_DOWN) {
                            u8 pi = editor_get_current_pattern_idx();
                            undo_push_cell(pi, cursor.row, cursor.channel);
                            if (cell->note >= 12)
                                cell->note -= 12;
                            else
                                cell->note = 0;
                        }
                        if (rep & KEY_RIGHT) {
                            u8 pi = editor_get_current_pattern_idx();
                            undo_push_cell(pi, cursor.row, cursor.channel);
                            if (cell->note < 119)
                                cell->note++;
                        }
                        if (rep & KEY_LEFT) {
                            u8 pi = editor_get_current_pattern_idx();
                            undo_push_cell(pi, cursor.row, cursor.channel);
                            if (cell->note > 0)
                                cell->note--;
                        }
                        if (rep & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
                            cursor.octave   = cell->note / 12;
                            cursor.semitone = cell->note % 12;
                            if (down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
                                playback_preview_note(cell->note, cell->inst);
                        }
                    }
                } else {
                    u8 *field = NULL;
                    switch (cursor.column) {
                    case 1: field = &cell->inst;  break;
                    case 2: field = &cell->vol;   break;
                    case 3: field = &cell->fx;    break;
                    case 4: field = &cell->param; break;
                    }
                    if (field && (rep & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
                        u8 pi = editor_get_current_pattern_idx();
                        undo_push_cell(pi, cursor.row, cursor.channel);
                        if (rep & KEY_UP) {
                            u8 hi = (*field >> 4) & 0xF;
                            if (hi < 0xF) *field = ((hi + 1) << 4) | (*field & 0xF);
                        }
                        if (rep & KEY_DOWN) {
                            u8 hi = (*field >> 4) & 0xF;
                            if (hi > 0) *field = ((hi - 1) << 4) | (*field & 0xF);
                        }
                        if (rep & KEY_RIGHT) {
                            u8 lo = *field & 0xF;
                            if (lo < 0xF) *field = (*field & 0xF0) | (lo + 1);
                        }
                        if (rep & KEY_LEFT) {
                            u8 lo = *field & 0xF;
                            if (lo > 0) *field = (*field & 0xF0) | (lo - 1);
                        }
                    }
                }
                mt_mark_song_modified();
            }
            return;
        }

        /* ---- Plain direction: cursor movement (with repeat) ---- */
        if (rep & KEY_UP) {
            if (cursor.row >= cursor.step)
                cursor.row -= cursor.step;
            else
                cursor.row = 0;
        }
        if (rep & KEY_DOWN) {
            int next = cursor.row + cursor.step;
            if (next >= nrows) next = nrows - 1;
            cursor.row = (u8)next;
        }
        if (rep & KEY_LEFT) {
            if (cursor.column > 0) cursor.column--;
        }
        if (rep & KEY_RIGHT) {
            if (cursor.column < 4) cursor.column++;
        }

        {
            int vis = pattern_view_overview_visible_channels();
            if (rep & KEY_L) {
                if (cursor.channel > 0) {
                    cursor.channel--;
                    if (cursor.channel < cursor.ch_group)
                        cursor.ch_group = (cursor.channel / vis) * vis;
                }
            }
            if (rep & KEY_R) {
                if (cursor.channel < song.channel_count - 1) {
                    cursor.channel++;
                    if (cursor.channel >= cursor.ch_group + vis)
                        cursor.ch_group = (cursor.channel / vis) * vis;
                }
            }
        }
        return;
    }

    /* ==== OVERVIEW MODE (all channels compressed) ==== */

    if ((held & KEY_B) && (down & KEY_A)) {
        if (cursor.selecting && pat) {
            u8 r0 = cursor.sel_start_row, r1 = cursor.row;
            u8 c0 = cursor.sel_start_col, c1 = cursor.channel;
            if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
            if (c0 > c1) { u8 t = c0; c0 = c1; c1 = t; }
            u8 pi = editor_get_current_pattern_idx();
            undo_push_block(pi, r0, r1, c0, c1);
            clipboard_copy(pat, r0, r1, c0, c1);
            clipboard_clear_block(pat, r0, r1, c0, c1);
            cursor.selecting = false;
            mt_mark_song_modified();
        } else if (pat) {
            MT_Cell *cell = cursor_cell(pat);
            if (cell) {
                u8 pi = editor_get_current_pattern_idx();
                undo_push_cell(pi, cursor.row, cursor.channel);
                cell->note = NOTE_EMPTY;
                cell->inst = 0;
                cell->vol = 0;
                cell->fx = 0;
                cell->param = 0;
                playback_stop_preview();
                mt_mark_song_modified();
            }
        }
        return;
    }

    if ((down & KEY_B) && cursor.selecting && pat) {
        u8 r0 = cursor.sel_start_row, r1 = cursor.row;
        u8 c0 = cursor.sel_start_col, c1 = cursor.channel;
        if (r0 > r1) { u8 t = r0; r0 = r1; r1 = t; }
        if (c0 > c1) { u8 t = c0; c0 = c1; c1 = t; }
        clipboard_copy(pat, r0, r1, c0, c1);
        cursor.selecting = false;
        snprintf(status_msg, sizeof(status_msg),
                 "Copied %dx%d", r1 - r0 + 1, c1 - c0 + 1);
        status_timer = 60;
        return;
    }

    if ((held & KEY_A) &&
        !cursor.selecting &&
        !(held & (KEY_B | KEY_R | KEY_L | MT_SHIFT_KEY))) {
        MT_Cell *cell = cursor_cell(pat);
        if (cell) {
            u8 pi = editor_get_current_pattern_idx();

            if (down & KEY_A) {
                if (note_slot_a_press(cell, pi, cursor.row, cursor.channel)) {
                    mt_mark_song_modified();
                }
            }

            u32 rep = keysDownRepeat();
            if (cell->note < 120) {
                if (rep & KEY_UP) {
                    undo_push_cell(pi, cursor.row, cursor.channel);
                    if (cell->note + 12 <= 119) cell->note += 12;
                    else                        cell->note = 119;
                }
                if (rep & KEY_DOWN) {
                    undo_push_cell(pi, cursor.row, cursor.channel);
                    if (cell->note >= 12) cell->note -= 12;
                    else                  cell->note = 0;
                }
                if (rep & KEY_RIGHT) {
                    undo_push_cell(pi, cursor.row, cursor.channel);
                    if (cell->note < 119) cell->note++;
                }
                if (rep & KEY_LEFT) {
                    undo_push_cell(pi, cursor.row, cursor.channel);
                    if (cell->note > 0) cell->note--;
                }
                if (rep & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
                    cursor.octave   = cell->note / 12;
                    cursor.semitone = cell->note % 12;
                    mt_mark_song_modified();
                    if (down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
                        playback_preview_note(cell->note, cell->inst);
                }
            }
        }
        return;
    }

    {
        u32 rep = keysDownRepeat();

        if (rep & KEY_UP) {
            if (cursor.row >= cursor.step)
                cursor.row -= cursor.step;
            else
                cursor.row = 0;
        }
        if (rep & KEY_DOWN) {
            int next = cursor.row + cursor.step;
            if (next >= nrows) next = nrows - 1;
            cursor.row = (u8)next;
        }
        int vis = pattern_view_overview_visible_channels();
        if (rep & KEY_LEFT) {
            if (cursor.channel > 0) {
                cursor.channel--;
                if (cursor.channel < cursor.ch_group)
                    cursor.ch_group = cursor.channel;
            }
        }
        if (rep & KEY_RIGHT) {
            if (cursor.channel < song.channel_count - 1) {
                cursor.channel++;
                if (cursor.channel >= cursor.ch_group + vis)
                    cursor.ch_group = cursor.channel - (vis - 1);
            }
        }
    }

    {
        int vis = pattern_view_overview_visible_channels();
        u32 rep_lr = keysDownRepeat();
        if (rep_lr & KEY_L) {
            if (cursor.ch_group >= vis) {
                cursor.ch_group -= vis;
                cursor.channel = cursor.ch_group;
            } else if (cursor.ch_group > 0) {
                cursor.ch_group = 0;
                cursor.channel = 0;
            }
        }
        if (rep_lr & KEY_R) {
            if (cursor.ch_group + vis < song.channel_count) {
                cursor.ch_group += vis;
                cursor.channel = cursor.ch_group;
            }
        }
    }
}
