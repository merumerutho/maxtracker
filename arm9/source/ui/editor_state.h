/*
 * editor_state.h — Shared editor cursor and selection state.
 *
 * The EditorCursor describes "where the user is" across every screen
 * (which row, which channel, which sub-column, current octave/instrument,
 * playback follow mode, block-selection anchor, etc.). It used to live
 * in pattern_view.h, but several other views (instrument, mixer, sample,
 * project) only included pattern_view.h to read this struct, which made
 * pattern_view a de-facto central state holder. Splitting it out keeps
 * pattern_view.h focused on rendering and lets other views depend on
 * just the editor state without dragging in the renderer.
 */

#ifndef MT_EDITOR_STATE_H
#define MT_EDITOR_STATE_H

#include <nds.h>
#include <stdbool.h>
#include "song.h"

typedef struct {
    u8  row;            /* current row in pattern (0-255) */
    u8  channel;        /* current channel (0-31) */
    u8  column;         /* sub-column in inside view: 0=note,1=inst,2=vol,3=fx,4=param */
    u8  octave;         /* current octave for note entry (0-9) */
    u8  semitone;       /* current semitone for note entry (0-11) */
    u8  instrument;     /* current instrument number (1-128) */
    u8  step;           /* edit step (cursor advance after entry) */
    u8  ch_group;       /* first visible channel (0, 8, 16, 24) */
    u8  order_pos;      /* current position in order table (0-199) */
    bool inside;        /* true = inside (single channel) view */
    bool follow;        /* true = cursor follows playback */
    bool playing;       /* true = playback active */
    u8   play_row;      /* currently playing row (from ARM7 tick messages) */
    u8   play_order;    /* currently playing order position */
    bool selecting;     /* true = block selection mode active */
    u8   sel_start_row; /* selection anchor row */
    u8   sel_start_col; /* anchor column (channel in overview, sub-column in inside) */
} EditorCursor;

extern EditorCursor cursor;

/* What kind of playback is active (when cursor.playing == true). */
typedef enum {
    PLAY_STOPPED = 0,     /* not playing */
    PLAY_PATTERN_ALL,     /* loop current pattern, all channels */
    PLAY_PATTERN_SOLO,    /* loop current pattern, solo cursor.channel */
    PLAY_SONG,            /* full arrangement from cursor.order_pos */
} PlaybackMode;

extern PlaybackMode play_mode;

/* Convenience queries that bridge cursor position with the song model.
 * These answer "what pattern / pattern index is the cursor looking at
 * right now?" — used pervasively by pattern input handling, navigation,
 * and clipboard actions. */

/* Get the pattern at the cursor's current order position, plus its row
 * count via *out_nrows. Returns NULL if the pattern slot is empty. */
MT_Pattern *editor_get_current_pattern(int *out_nrows);

/* Get the pattern INDEX at the cursor's current order position. */
u8 editor_get_current_pattern_idx(void);

/*
 * Mark the song as edited. Sets both `song_modified` (used by the
 * save-prompt / dirty indicator) and `autosave_dirty` (polled by the
 * autosave timer) in one call.
 *
 * Prefer this over touching the flags directly — it's impossible to
 * forget one, and the linker will catch any view that forgets to call
 * it (the raw flags are now a closed namespace in main.c).
 *
 * Cheap to call — a pair of stores, no memory barrier. Safe from any
 * view / input / draw context; not safe from ARM7.
 */
void mt_mark_song_modified(void);

/* ------------------------------------------------------------------ */
/* Key-repeat preferences                                              */
/* ------------------------------------------------------------------ */

/*
 * Frames (60 Hz) before auto-repeat kicks in, and frames between
 * subsequent repeats. Session-scoped — not persisted in the MAS file.
 * PROJECT view exposes both as editable rows.
 *
 * Defaults: 15-frame delay (~250ms), 4-frame rate (~66ms) — matches
 * the previous hardcoded keysSetRepeat(15, 4) and the LSDJ feel.
 *
 * Clamp: delay 4..60, rate 1..30. Anything outside those bounds is
 * either unusably sluggish or runaway-repeating and was rejected by
 * the settings UI.
 */
extern u8 ui_repeat_delay;
extern u8 ui_repeat_rate;

/* Push the current (delay, rate) values into libnds. Call after every
 * mutation of ui_repeat_delay / ui_repeat_rate. */
void ui_apply_key_repeat(void);

#endif /* MT_EDITOR_STATE_H */
