/*
 * undo.h — Undo ring buffer for pattern edits.
 */

#ifndef MT_UNDO_H
#define MT_UNDO_H

#include <nds.h>
#include <stdbool.h>
#include "song.h"

#define MT_UNDO_DEPTH 64

enum {
    UNDO_CELL  = 0,
    UNDO_ROW   = 1,
    UNDO_BLOCK = 2
};

typedef struct {
    u8  type;           /* UNDO_CELL, UNDO_ROW, UNDO_BLOCK */
    u8  pattern;        /* which pattern index */
    u16 row;            /* start row */
    u8  channel;        /* start channel */
    u8  count_ch;       /* block width (max MT_MAX_CHANNELS = 32) */
    u16 count_rows;     /* block height (must be u16: MT_MAX_ROWS == 256) */
    MT_Cell *old_data;  /* malloc'd copy of previous data */
} MT_UndoEntry;

/* Push current state before modifying (call BEFORE the edit) */
void undo_push_cell(u8 pattern, u16 row, u8 channel);
void undo_push_block(u8 pattern, u16 row_start, u16 row_end,
                     u8 ch_start, u8 ch_end);

/* Undo last edit (restore previous data). Returns true if undo was applied. */
bool undo_pop(MT_Song *song);

/* Initialize / reset undo system */
void undo_init(void);
void undo_free(void);

/* Optional callback fired AFTER any successful undo_push_cell or
 * undo_push_block. Used by playback.c to mark the cells region dirty
 * for the next ARM9→ARM7 cache flush — every cell modification site
 * goes through one of those push functions, so this is a single
 * choke point that captures all of them. NULL = no callback.
 *
 * The hook is callable from any context (no I/O, no FIFO) — the
 * concrete consumer just sets a flag, real flush happens in the
 * playback_update() loop.
 */
typedef void (*undo_push_cb)(void);
void undo_set_on_push(undo_push_cb cb);

#endif /* MT_UNDO_H */
