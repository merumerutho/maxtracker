/*
 * undo.c — Undo ring buffer for pattern edits.
 */

#include <stdlib.h>
#include <string.h>
#include "undo.h"

static MT_UndoEntry ring[MT_UNDO_DEPTH];
static int head;   /* next slot to write */
static int count;  /* number of valid entries */
static undo_push_cb on_push_cb = NULL;

void undo_set_on_push(undo_push_cb cb) { on_push_cb = cb; }

/*
 * Free the old_data in a single entry.
 */
static void entry_free(MT_UndoEntry *e)
{
    if (e->old_data) {
        free(e->old_data);
        e->old_data = NULL;
    }
}

/*
 * undo_init — Clear the ring buffer.
 */
void undo_init(void)
{
    for (int i = 0; i < MT_UNDO_DEPTH; i++)
        entry_free(&ring[i]);
    head  = 0;
    count = 0;
    memset(ring, 0, sizeof(ring));
}

/*
 * undo_free — Release all undo memory (call on song_free / exit).
 */
void undo_free(void)
{
    undo_init();
}

/*
 * Push a new entry into the ring. If full, the oldest entry is freed.
 * Returns pointer to the new entry (already zeroed).
 */
static MT_UndoEntry *ring_push(void)
{
    /* If buffer is full, free the oldest entry we're about to overwrite */
    if (count == MT_UNDO_DEPTH)
        entry_free(&ring[head]);
    else
        count++;

    MT_UndoEntry *e = &ring[head];
    memset(e, 0, sizeof(*e));
    head = (head + 1) % MT_UNDO_DEPTH;
    return e;
}

/*
 * undo_push_cell — Snapshot a single cell before editing it.
 */
void undo_push_cell(u8 pattern, u16 row, u8 channel)
{
    MT_Pattern *pat = song.patterns[pattern];
    if (!pat) return;
    if (row >= pat->nrows || channel >= MT_MAX_CHANNELS) return;

    MT_Cell *data = (MT_Cell *)malloc(sizeof(MT_Cell));
    if (!data) return;

    *data = *MT_CELL(pat, row, channel);

    MT_UndoEntry *e = ring_push();
    e->type       = UNDO_CELL;
    e->pattern    = pattern;
    e->row        = row;
    e->channel    = channel;
    e->count_rows = 1;
    e->count_ch   = 1;
    e->old_data   = data;

    if (on_push_cb) on_push_cb();
}

/*
 * undo_push_block — Snapshot a rectangular block before editing it.
 * row_start..row_end and ch_start..ch_end are inclusive.
 */
void undo_push_block(u8 pattern, u16 row_start, u16 row_end,
                     u8 ch_start, u8 ch_end)
{
    MT_Pattern *pat = song.patterns[pattern];
    if (!pat) return;
    if (row_end < row_start || ch_end < ch_start) return;

    /* Clamp to pattern bounds */
    if (row_end >= pat->nrows) row_end = pat->nrows - 1;
    if (ch_end >= MT_MAX_CHANNELS) ch_end = MT_MAX_CHANNELS - 1;

    u16 nrows = (u16)(row_end - row_start + 1);
    u8  nchan = (u8) (ch_end  - ch_start  + 1);

    MT_Cell *data = (MT_Cell *)malloc((u32)nrows * nchan * sizeof(MT_Cell));
    if (!data) return;

    for (u16 r = 0; r < nrows; r++) {
        memcpy(&data[r * nchan],
               MT_CELL(pat, row_start + r, ch_start),
               nchan * sizeof(MT_Cell));
    }

    MT_UndoEntry *e = ring_push();
    e->type       = (nrows == 1 && nchan == 1) ? UNDO_CELL :
                    (nchan == 1) ? UNDO_ROW : UNDO_BLOCK;
    e->pattern    = pattern;
    e->row        = row_start;
    e->channel    = ch_start;
    e->count_rows = nrows;
    e->count_ch   = nchan;
    e->old_data   = data;

    if (on_push_cb) on_push_cb();
}

/*
 * undo_pop — Restore the most recent undo entry. Returns true if applied.
 */
bool undo_pop(MT_Song *song)
{
    if (count == 0) return false;

    /* The most recent entry is at (head - 1), wrapping */
    head = (head - 1 + MT_UNDO_DEPTH) % MT_UNDO_DEPTH;
    count--;

    MT_UndoEntry *e = &ring[head];
    if (!e->old_data) return false;

    MT_Pattern *pat = song->patterns[e->pattern];
    if (!pat) {
        entry_free(e);
        return false;
    }

    /* Restore the saved cells back into the pattern */
    for (u16 r = 0; r < e->count_rows; r++) {
        u16 dst_row = e->row + r;
        if (dst_row >= pat->nrows) break;

        memcpy(MT_CELL(pat, dst_row, e->channel),
               &e->old_data[r * e->count_ch],
               e->count_ch * sizeof(MT_Cell));
    }

    entry_free(e);
    if (on_push_cb) on_push_cb();   /* restored cells need a flush too */
    return true;
}
