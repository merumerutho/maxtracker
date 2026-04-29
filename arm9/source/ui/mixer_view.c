/*
 * mixer_view.c — Channel mixer: vertical list of channel rows + detail.
 *
 * Bottom screen: all 32 channels as a vertical scrollable list. Each
 * row shows the channel number, a horizontal volume bar, the volume
 * value, panning indicator, and mute flag. Uses the generic ScrollView
 * widget so BIG font mode (which shrinks the viewport) just scrolls.
 *
 * Top screen: detail view of the selected channel — volume, panning,
 * mute state, and a wide horizontal bar for visual reference.
 *
 * Volume range is 0-64 matching maxmod's MAS channel volume cap.
 */

#include "mixer_view.h"
#include "draw_util.h"
#include "screen.h"
#include "font.h"
#include "editor_state.h"
#include "song.h"
#include "playback.h"
#include "scroll_view.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

MixerState mixer_state = {
    .selected_channel = 0,
    .muted = { false },
};

/* Scroll state for the channel row list (bottom screen). Margin 2 keeps
 * 2 rows of context around the cursor like the other param lists. */
static ScrollView mx_sv = { .margin = 2 };

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char pan_char(u8 pan)
{
    if (pan < 96)  return 'L';
    if (pan > 160) return 'R';
    return 'C';
}

static inline int clamp_vol(int v)
{
    if (v < 0)             return 0;
    if (v > MIXER_VOL_MAX) return MIXER_VOL_MAX;
    return v;
}

static inline int clamp_pan(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* ------------------------------------------------------------------ */
/* Top screen: selected-channel detail                                 */
/* ------------------------------------------------------------------ */

static void draw_top(u8 *fb)
{
    int ch  = mixer_state.selected_channel;
    int vol = song.channel_volume[ch];
    int pan = song.channel_panning[ch];
    bool muted = mixer_state.muted[ch];

    /* Header */
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(fb, 0, 0, PAL_TEXT, "MIXER");
    font_printf(fb, 7, 0, PAL_GRAY, "Selected: CH %02d", ch + 1);
    font_printf(fb, font_scale_col(50), 0, PAL_DIM,
                "T:%03d", song.initial_tempo);

    /* Detail rows */
    font_fill_row(fb, 2, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 2, 2, PAL_TEXT, "Channel:  %02d / %02d",
                ch + 1, song.channel_count);

    font_fill_row(fb, 4, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 2, 4, PAL_TEXT, "Volume:   %2d / %d",
                vol, MIXER_VOL_MAX);

    /* Wide volume bar — pixel-level fader. */
    font_fill_row(fb, 5, 0, FONT_COLS, PAL_BG);
    {
        u8 bar_color = muted ? PAL_RED : PAL_PLAY;
        int px_x = 10 * FONT_W;
        int px_w = (FONT_COLS - 12) * FONT_W;
        if (px_w > 40 * FONT_W) px_w = 40 * FONT_W;
        int px_y = 5 * FONT_H + 1;
        int px_h = FONT_H - 4;
        if (px_h < 1) px_h = 1;
        ui_draw_fader(fb, px_x, px_y, px_w, px_h,
                      vol, MIXER_VOL_MAX, bar_color, PAL_HEADER_BG);
    }

    font_fill_row(fb, 7, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 2, 7, PAL_NOTE, "Panning:  %3d  (%c)",
                pan, pan_char((u8)pan));

    font_fill_row(fb, 9, 0, FONT_COLS, PAL_BG);
    if (muted)
        font_puts(fb, 2, 9, "Status:   MUTED", PAL_RED);
    else
        font_puts(fb, 2, 9, "Status:   ON", PAL_PLAY);

    /* Clear spare rows */
    int foot_row = font_scale_row(30);
    int tport_row = font_scale_row(31);
    for (int r = 11; r < foot_row; r++)
        font_fill_row(fb, r, 0, FONT_COLS, PAL_BG);

    /* Footer */
    font_fill_row(fb, foot_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, foot_row,
              "U/D:ch  A+U/D:vol+-1  A+L/R:vol+-4", PAL_DIM);
    font_fill_row(fb, tport_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, tport_row,
              "Y+L/R:pan  X:mute  SEL+D:back", PAL_DIM);
}

/* ------------------------------------------------------------------ */
/* Bottom screen: vertical scrollable channel list                     */
/*                                                                     */
/* One channel per grid row. Layout (proportional to FONT_COLS):       */
/*                                                                     */
/*   Row 0:        Header                                              */
/*   Rows 2..N-1:  Channel rows (scrollable; selected row highlighted) */
/*   Row 30-31:    Footer (scaled)                                     */
/*                                                                     */
/* Per-row layout:                                                     */
/*   col 1..4:   "CH##"                                                */
/*   col 7..E:   pixel fader bar  (E = FONT_COLS - 9)                  */
/*   col E+2..E+4: "%3d" volume                                        */
/*   col E+6:    pan char (L/C/R)                                      */
/*   col E+7:    'M' if muted                                          */
/*   col FONT_COLS-1: scrollbar reserved                               */
/* ------------------------------------------------------------------ */

static void draw_bot(u8 *fb)
{
    int ch_count = song.channel_count;
    if (ch_count > MT_MAX_CHANNELS) ch_count = MT_MAX_CHANNELS;
    if (ch_count < 1) ch_count = 1;

    int foot_row  = font_scale_row(30);
    int tport_row = font_scale_row(31);

    font_clear(fb, PAL_BG);

    /* Header */
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 0, "MIXER", PAL_TEXT);
    font_printf(fb, 7, 0, PAL_GRAY, "%d ch", ch_count);
    font_printf(fb, font_scale_col(30), 0, PAL_DIM, "Vol 0-%d", MIXER_VOL_MAX);

    /* Viewport: rows 2 .. foot_row-1, scrollbar in rightmost grid col. */
    int list_start  = 2;
    int list_height = foot_row - list_start;
    if (list_height < 1) list_height = 1;

    mx_sv.row_y      = list_start;
    mx_sv.row_height = list_height;
    mx_sv.total      = ch_count;
    mx_sv.cursor     = mixer_state.selected_channel;
    scroll_view_follow(&mx_sv);

    int first, last;
    scroll_view_visible(&mx_sv, &first, &last);

    /* Layout columns derived from FONT_COLS so BIG mode just shrinks
     * the bar — labels and values stay at their right anchors. */
    int bar_open  = 6;
    int bar_close = FONT_COLS - 9;     /* ']' */
    if (bar_close < bar_open + 2) bar_close = bar_open + 2;
    int bar_width = bar_close - (bar_open + 1);
    int vol_col   = bar_close + 2;      /* "%3d" volume */
    int pan_col   = bar_close + 6;      /* L/C/R */
    int mute_col  = bar_close + 7;      /* 'M' */

    for (int ch = first; ch < last; ch++) {
        int row    = mx_sv.row_y + (ch - mx_sv.scroll);
        bool sel   = (ch == mixer_state.selected_channel);
        bool muted = mixer_state.muted[ch];
        int  vol   = song.channel_volume[ch];
        u8   pan   = song.channel_panning[ch];

        if (sel)
            font_fill_row(fb, row, 0, FONT_COLS, PAL_ROW_CURSOR);

        /* Channel label */
        font_printf(fb, 1, row, sel ? PAL_WHITE : PAL_GRAY,
                    "CH%02d", ch + 1);

        /* Pixel fader bar */
        u8 bar_color;
        if (muted)     bar_color = PAL_RED;
        else if (sel)  bar_color = PAL_NOTE;
        else           bar_color = PAL_PLAY;

        {
            int bpx_x = (bar_open + 1) * FONT_W;
            int bpx_w = bar_width * FONT_W;
            int bpx_y = row * FONT_H + 1;
            int bpx_h = FONT_H - 4;
            if (bpx_h < 1) bpx_h = 1;
            ui_draw_fader(fb, bpx_x, bpx_y, bpx_w, bpx_h,
                          vol, MIXER_VOL_MAX, bar_color, PAL_HEADER_BG);
        }

        /* Volume value */
        if (vol_col + 3 <= FONT_COLS - 1)
            font_printf(fb, vol_col, row,
                        sel ? PAL_WHITE : PAL_TEXT, "%3d", vol);

        /* Pan + mute indicators (skip if no room) */
        if (pan_col < FONT_COLS - 1)
            font_putc(fb, pan_col, row, pan_char(pan), PAL_NOTE);
        if (mute_col < FONT_COLS - 1)
            font_putc(fb, mute_col, row,
                      muted ? 'M' : ' ',
                      muted ? PAL_RED : PAL_DIM);
    }

    /* Scrollbar in the rightmost grid column. */
    scroll_view_draw_scrollbar(&mx_sv, fb, FONT_COLS - 1);

    /* Footer */
    font_fill_row(fb, foot_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, foot_row,
              "U/D:ch  A+U/D:vol+-1  A+L/R:vol+-4", PAL_DIM);
    font_fill_row(fb, tport_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, tport_row,
              "Y+L/R:pan  X:mute  SEL+D:back", PAL_DIM);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void mixer_view_draw(u8 *top_fb, u8 *bot_fb)
{
    draw_top(top_fb);
    draw_bot(bot_fb);
}

void mixer_view_input(u32 down, u32 held)
{
    int ch_count = song.channel_count;
    if (ch_count < 1)              ch_count = 1;
    if (ch_count > MT_MAX_CHANNELS) ch_count = MT_MAX_CHANNELS;

    /* Y + LEFT/RIGHT: adjust panning for selected channel (±16). */
    if (held & KEY_Y) {
        int ch = mixer_state.selected_channel;
        if (ch < MT_MAX_CHANNELS) {
            u32 rep = keysDownRepeat();
            int pan = (int)song.channel_panning[ch];
            if (rep & KEY_LEFT)  pan -= 16;
            if (rep & KEY_RIGHT) pan += 16;
            song.channel_panning[ch] = (u8)clamp_pan(pan);
        }
        return;  /* Y modifier consumes d-pad this frame */
    }

    /* --- LSDJ A-held editing ---
     *   UP/DOWN without A: select channel (navigation, wraps)
     *   A held + UP/DOWN: volume ±1 (small step)
     *   A held + LEFT/RIGHT: volume ±4 (large step)
     * Volume clamped to 0-64. Vertical list, so navigation matches the
     * scrolldirection. */
    if (!(held & KEY_A)) {
        u32 rep = keysDownRepeat();
        if (rep & KEY_UP) {
            if (mixer_state.selected_channel > 0)
                mixer_state.selected_channel--;
            else
                mixer_state.selected_channel = (u8)(ch_count - 1);
        }
        if (rep & KEY_DOWN) {
            mixer_state.selected_channel++;
            if (mixer_state.selected_channel >= ch_count)
                mixer_state.selected_channel = 0;
        }
    } else {
        u32 rep = keysDownRepeat();
        int ch = mixer_state.selected_channel;
        if (ch < MT_MAX_CHANNELS) {
            int vol = (int)song.channel_volume[ch];
            if (rep & KEY_UP)    vol += 1;
            if (rep & KEY_DOWN)  vol -= 1;
            if (rep & KEY_RIGHT) vol += 4;
            if (rep & KEY_LEFT)  vol -= 4;
            song.channel_volume[ch] = (u8)clamp_vol(vol);
        }
    }

    /* X: toggle mute */
    if (down & KEY_X) {
        int ch = mixer_state.selected_channel;
        if (ch < MT_MAX_CHANNELS) {
            mixer_state.muted[ch] = !mixer_state.muted[ch];
            playback_set_mute(ch, mixer_state.muted[ch]);
        }
    }
}
