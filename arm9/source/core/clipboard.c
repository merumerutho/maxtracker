/*
 * clipboard.c — Unified tagged clipboard for maxtracker.
 *
 * One clipboard holds cells, orders, or instrument data. Copying any
 * type frees the previous content. Paste functions check the type tag
 * and return false on mismatch.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "clipboard.h"
#include "editor_state.h"
#include "undo.h"

extern char status_msg[64];
extern int  status_timer;

MT_Clipboard clipboard;

MT_NoteClipboard note_clipboard = { .note = NOTE_EMPTY, .inst = 0, .valid = false };

/* ---- Common ---- */

void clipboard_free(void)
{
    if (clipboard.cell_data) {
        free(clipboard.cell_data);
        clipboard.cell_data = NULL;
    }
    clipboard.cell_rows     = 0;
    clipboard.cell_channels = 0;
    clipboard.order_count   = 0;
    clipboard.type          = CLIP_NONE;
}

/* ---- Pattern cell block ---- */

void clipboard_copy(MT_Pattern *pat, u8 row_start, u8 row_end,
                    u8 ch_start, u8 ch_end)
{
    if (!pat) return;
    if (row_end < row_start || ch_end < ch_start) return;
    if (row_end >= pat->nrows) row_end = pat->nrows - 1;
    if (ch_end >= pat->ncols) ch_end = pat->ncols - 1;

    u16 nrows = (u16)(row_end - row_start + 1);
    u8  nchan = (u8)(ch_end - ch_start + 1);

    clipboard_free();

    clipboard.cell_data = (MT_Cell *)malloc(nrows * nchan * sizeof(MT_Cell));
    if (!clipboard.cell_data) return;

    for (u16 r = 0; r < nrows; r++) {
        memcpy(&clipboard.cell_data[r * nchan],
               MT_CELL(pat, row_start + r, ch_start),
               nchan * sizeof(MT_Cell));
    }

    clipboard.cell_rows     = nrows;
    clipboard.cell_channels = nchan;
    clipboard.type          = CLIP_CELLS;
}

void clipboard_copy_cell(MT_Pattern *pat, u8 row, u8 channel)
{
    clipboard_copy(pat, row, row, channel, channel);
}

void clipboard_paste(MT_Pattern *pat, u8 row, u8 channel)
{
    if (!pat || clipboard.type != CLIP_CELLS || !clipboard.cell_data) return;
    if (row >= pat->nrows || channel >= pat->ncols) return;

    u16 paste_rows = clipboard.cell_rows;
    u8  paste_ch   = clipboard.cell_channels;

    if (row + paste_rows > pat->nrows)
        paste_rows = pat->nrows - row;
    if (channel + paste_ch > pat->ncols)
        paste_ch = pat->ncols - channel;

    if (paste_rows == 0 || paste_ch == 0) return;

    for (u16 r = 0; r < paste_rows; r++) {
        memcpy(MT_CELL(pat, row + r, channel),
               &clipboard.cell_data[r * clipboard.cell_channels],
               paste_ch * sizeof(MT_Cell));
    }
}

void clipboard_clear_block(MT_Pattern *pat, u8 row_start, u8 row_end,
                           u8 ch_start, u8 ch_end)
{
    if (!pat) return;
    if (row_end < row_start || ch_end < ch_start) return;
    if (row_end >= pat->nrows) row_end = pat->nrows - 1;
    if (ch_end >= pat->ncols) ch_end = pat->ncols - 1;

    MT_Cell empty = { NOTE_EMPTY, 0, 0, 0, 0 };
    for (u16 r = row_start; r <= row_end; r++) {
        for (u8 c = ch_start; c <= ch_end; c++) {
            *MT_CELL(pat, r, c) = empty;
        }
    }
}

bool clipboard_has_block(void)
{
    return clipboard.type == CLIP_CELLS && clipboard.cell_data != NULL;
}

/* ---- Order list ---- */

void clipboard_copy_orders(u8 start, u8 end)
{
    if (start > end) { u8 t = start; start = end; end = t; }
    if (end >= song.order_count) end = song.order_count - 1;

    clipboard_free();

    int n = end - start + 1;
    if (n > CLIP_ORDER_MAX) n = CLIP_ORDER_MAX;
    for (int i = 0; i < n; i++)
        clipboard.order_data[i] = song.orders[start + i];
    clipboard.order_count = n;
    clipboard.type = CLIP_ORDERS;
}

bool clipboard_paste_orders(u8 insert_pos)
{
    if (clipboard.type != CLIP_ORDERS || clipboard.order_count == 0)
        return false;

    int avail = MT_MAX_ORDERS - (int)song.order_count;
    int n = clipboard.order_count;
    if (n > avail) n = avail;
    if (n <= 0) return false;

    u8 pos = insert_pos;
    if (pos > song.order_count) pos = song.order_count;

    for (int i = song.order_count - 1; i >= pos; i--)
        song.orders[i + n] = song.orders[i];
    for (int i = 0; i < n; i++) {
        song.orders[pos + i] = clipboard.order_data[i];
        song_ensure_pattern(clipboard.order_data[i]);
        if (clipboard.order_data[i] >= song.patt_count)
            song.patt_count = clipboard.order_data[i] + 1;
    }
    song.order_count += n;
    return true;
}

bool clipboard_has_orders(void)
{
    return clipboard.type == CLIP_ORDERS && clipboard.order_count > 0;
}

/* ---- Instrument ---- */

void clipboard_copy_instrument(u8 inst_idx)
{
    if (inst_idx >= MT_MAX_INSTRUMENTS) return;

    clipboard_free();

    clipboard.inst_data = song.instruments[inst_idx];
    clipboard.type = CLIP_INSTRUMENT;
}

bool clipboard_paste_instrument(u8 inst_idx)
{
    if (clipboard.type != CLIP_INSTRUMENT) return false;
    if (inst_idx >= MT_MAX_INSTRUMENTS) return false;

    song.instruments[inst_idx] = clipboard.inst_data;
    if (inst_idx + 1 > song.inst_count)
        song.inst_count = inst_idx + 1;
    return true;
}

bool clipboard_has_instrument(void)
{
    return clipboard.type == CLIP_INSTRUMENT;
}

/* ---- Single-slot note clipboard (independent of main clipboard) ---- */

bool note_slot_a_press(MT_Cell *cell, u8 pi, u8 row, u8 ch)
{
    bool has_note = (cell->note != NOTE_EMPTY);
    bool has_inst = (cell->inst != 0);

    if (has_note && has_inst) {
        note_clipboard.note  = cell->note;
        note_clipboard.inst  = cell->inst;
        note_clipboard.valid = true;
        snprintf(status_msg, sizeof(status_msg), "Slot copied");
        status_timer = 60;
        return false;
    }

    if (!has_note && !has_inst) {
        bool clip_useful = note_clipboard.valid &&
            (note_clipboard.note != NOTE_EMPTY || note_clipboard.inst != 0);
        if (clip_useful) {
            undo_push_cell(pi, row, ch);
            if (note_clipboard.note != NOTE_EMPTY) cell->note = note_clipboard.note;
            if (note_clipboard.inst != 0)          cell->inst = note_clipboard.inst;
            return true;
        }
        u8 note = (u8)(cursor.octave * 12 + cursor.semitone);
        if (note > 119) note = 119;
        undo_push_cell(pi, row, ch);
        cell->note = note;
        cell->inst = cursor.instrument;
        return true;
    }

    if (!note_clipboard.valid) return false;

    if (!has_note && has_inst) {
        if (note_clipboard.note == NOTE_EMPTY) return false;
        undo_push_cell(pi, row, ch);
        cell->note = note_clipboard.note;
        return true;
    }

    if (note_clipboard.inst == 0) return false;
    undo_push_cell(pi, row, ch);
    cell->inst = note_clipboard.inst;
    return true;
}

bool inst_slot_a_press(MT_Cell *cell, u8 pi, u8 row, u8 ch)
{
    if (cell->inst != 0) {
        note_clipboard.inst  = cell->inst;
        note_clipboard.valid = true;
        snprintf(status_msg, sizeof(status_msg), "Inst %02X copied", cell->inst);
        status_timer = 60;
        return false;
    }

    if (note_clipboard.valid && note_clipboard.inst != 0) {
        undo_push_cell(pi, row, ch);
        cell->inst = note_clipboard.inst;
        return true;
    }
    undo_push_cell(pi, row, ch);
    cell->inst = cursor.instrument;
    return true;
}
