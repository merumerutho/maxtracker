/*
 * clipboard.h — Unified tagged clipboard for maxtracker.
 *
 * One clipboard instance holds exactly one type of data at a time:
 * pattern cell blocks, order list entries, or instrument data.
 * Paste operations check the type and refuse mismatched content.
 */

#ifndef MT_CLIPBOARD_H
#define MT_CLIPBOARD_H

#include <nds.h>
#include <stdbool.h>
#include "song.h"

/* ---- Clipboard type tag ---- */

typedef enum {
    CLIP_NONE = 0,
    CLIP_CELLS,       /* rectangular block of pattern cells */
    CLIP_ORDERS,      /* order list entries (pattern references) */
    CLIP_INSTRUMENT,  /* full instrument struct */
} ClipboardType;

/* ---- Unified clipboard ---- */

#define CLIP_ORDER_MAX 32

typedef struct {
    ClipboardType type;

    /* CLIP_CELLS: malloc'd cell array [rows * channels] */
    MT_Cell *cell_data;
    u16      cell_rows;
    u8       cell_channels;

    /* CLIP_ORDERS: small inline buffer */
    u8  order_data[CLIP_ORDER_MAX];
    int order_count;

    /* CLIP_INSTRUMENT: instrument struct (no PCM — just the reference) */
    MT_Instrument inst_data;
} MT_Clipboard;

extern MT_Clipboard clipboard;

/* ---- Pattern cell block operations ---- */

void clipboard_copy(MT_Pattern *pat, u8 row_start, u8 row_end,
                    u8 ch_start, u8 ch_end);
void clipboard_paste(MT_Pattern *pat, u8 row, u8 channel);
void clipboard_clear_block(MT_Pattern *pat, u8 row_start, u8 row_end,
                           u8 ch_start, u8 ch_end);
bool clipboard_has_block(void);

/* ---- Order list operations ---- */

void clipboard_copy_orders(u8 start, u8 end);
bool clipboard_paste_orders(u8 insert_pos);
bool clipboard_has_orders(void);

/* ---- Instrument operations ---- */

void clipboard_copy_instrument(u8 inst_idx);
bool clipboard_paste_instrument(u8 inst_idx);
bool clipboard_has_instrument(void);

/* ---- Common ---- */

void clipboard_free(void);

/* ---- Single-slot note clipboard (independent, M8-style A-press) ---- */

typedef struct {
    u8   note;
    u8   inst;
    bool valid;
} MT_NoteClipboard;

extern MT_NoteClipboard note_clipboard;

void clipboard_copy_cell(MT_Pattern *pat, u8 row, u8 channel);
bool note_slot_a_press(MT_Cell *cell, u8 pi, u8 row, u8 ch);
bool inst_slot_a_press(MT_Cell *cell, u8 pi, u8 row, u8 ch);

#endif /* MT_CLIPBOARD_H */
