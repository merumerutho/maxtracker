/*
 * editor_state.c — Global editor cursor + cursor-position queries.
 */

#include "editor_state.h"
#include "song.h"

EditorCursor cursor = {
    .row        = 0,
    .channel    = 0,
    .column     = 0,
    .octave     = 4,
    .semitone   = 0,
    .instrument = 1,
    .step       = 1,
    .ch_group   = 0,
    .order_pos  = 0,
    .inside     = false,
    .follow     = false,
    .playing    = false,
    .play_row       = 0,
    .play_order     = 0,
    .selecting      = false,
    .sel_start_row  = 0,
    .sel_start_col  = 0,
};

PlaybackMode play_mode = PLAY_STOPPED;

MT_Pattern *editor_get_current_pattern(int *out_nrows)
{
    u8 pat_idx = 0;
    if (cursor.order_pos < song.order_count &&
        song.orders[cursor.order_pos] < song.patt_count)
        pat_idx = song.orders[cursor.order_pos];
    MT_Pattern *pat = song.patterns[pat_idx];
    *out_nrows = pat ? pat->nrows : 64;
    return pat;
}

u8 editor_get_current_pattern_idx(void)
{
    if (cursor.order_pos < song.order_count)
        return song.orders[cursor.order_pos];
    return 0;
}

/* The dirty-flag pair lives in main.c as true globals because autosave
 * and the disk-screen save path both read/clear them. The helper here
 * is the only sanctioned writer — direct assignment from views is
 * discouraged (see the extern block below). */
extern bool song_modified;
extern bool autosave_dirty;

void mt_mark_song_modified(void)
{
    song_modified  = true;
    autosave_dirty = true;
}

/* Key-repeat preferences — see editor_state.h for the contract. */
u8 ui_repeat_delay = 15;   /* ~250 ms before repeat kicks in */
u8 ui_repeat_rate  = 4;    /* ~66 ms between repeats         */

void ui_apply_key_repeat(void)
{
    keysSetRepeat(ui_repeat_delay, ui_repeat_rate);
}
