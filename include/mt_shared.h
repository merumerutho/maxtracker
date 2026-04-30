/*
 * mt_shared.h — Shared state between ARM9 (writer) and ARM7 (reader).
 *
 * ARM9 populates this struct and flushes the cache before signalling ARM7.
 * ARM7 reads through the uncached bus. Both sides include this header.
 */

#ifndef MT_SHARED_H
#define MT_SHARED_H

#include <nds.h>

#ifdef ARM9
#include "song.h"        /* MT_Cell, MT_MAX_CHANNELS, MT_MAX_ROWS */
#else
/* ARM7 side: reproduce the MT_Cell layout so we don't pull in the whole song.h */
#ifndef MT_CELL_DEFINED
#define MT_CELL_DEFINED
typedef struct {
    u8 note;
    u8 inst;
    u8 vol;
    u8 fx;
    u8 param;
} MT_Cell;
#endif

#define MT_MAX_CHANNELS  32
#define MT_MAX_ROWS      256
#define MT_MAX_ORDERS    200
#endif

/*
 * MT_PatternEntry — per-pattern info for ARM7 lookup.
 * ARM9 populates at playback start so ARM7 can resolve the correct
 * cells pointer immediately on pattern transitions (no IPC needed).
 */
typedef struct {
    volatile MT_Cell *cells;     /* Pointer to flat cell array (NULL = empty) */
    volatile u16      nrows;     /* Number of rows (1-based count) */
    u16               pad;
} MT_PatternEntry;

/*
 * MT_SharedPatternState
 *
 * Lives in main RAM (allocated by ARM9). ARM9 writes, ARM7 reads.
 * ARM9 must DC_FlushRange() after every modification.
 */
typedef struct {
    /* Pointer to current pattern's flat cell array: cells[row * ncols + ch].
     * Points directly into an MT_Pattern's cells[] flexible array member.
     * Updated by ARM7 itself via the order/pattern tables below. */
    volatile MT_Cell *cells;

    volatile u16 nrows;          /* Number of rows in current pattern (1-based count) */
    volatile u8  channel_count;  /* Active channel count (from song) */
    volatile u8  active;         /* 1 = maxtracker mode, 0 = standard MAS fallback */

    /* Playing flag — written by ARM9 (start/stop). 1-byte atomic. */
    volatile u8  playing;
    volatile u8  _pad_play[3];   /* align pos_state to a 4-byte boundary */

    /* Playback position — written by ARM7 in one 32-bit store, read by
     * ARM9 in one 32-bit load. Packing all three fields together makes
     * the read/write atomic on the bus, so ARM9 can never see a torn
     * combination like "new tick + old row" during a tick callback.
     *
     *   bits  7:0  = position (current order index)
     *   bits 15:8  = row      (current row in pattern)
     *   bits 23:16 = tick     (current tick within row)
     *   bits 31:24 = reserved (zero)
     */
    volatile u32 pos_state;

    /* Channel mute bitmask — set by ARM9, read by ARM7.
     * Bit N = 1 means channel N is muted. */
    volatile u32 mute_mask;

    /* Order table — copy of song.orders[]. ARM7 uses this to look up
     * which pattern index to play at each position. */
    u8 order_count;
    u8 orders[MT_MAX_ORDERS];

    /* Pattern table — cells pointers + row counts for each pattern.
     * ARM7 indexes this by orders[position] to get the cells pointer
     * and row count when the position changes, with zero IPC latency. */
    u8 patt_count;
    MT_PatternEntry patterns[256];  /* indexed by pattern index */
} MT_SharedPatternState;

/* Pack/unpack helpers for pos_state. Both sides should use these so the
 * bit layout is defined in exactly one place. */
#define MT_POS_PACK(pos, row, tick) \
    ( ((u32)(u8)(pos))         | \
     (((u32)(u8)(row))  <<  8) | \
     (((u32)(u8)(tick)) << 16) )
#define MT_POS_POSITION(s) ((u8)( (s)        & 0xFF))
#define MT_POS_ROW(s)      ((u8)(((s) >>  8) & 0xFF))
#define MT_POS_TICK(s)     ((u8)(((s) >> 16) & 0xFF))

/*
 * Global pointer to the shared state.
 * - ARM7 (MAXTRACKER_MODE): defined in mas_arm.c, set via MT_CMD_SET_SHARED.
 * - ARM9: defined in playback.c, allocated at init.
 */
#ifdef MAXTRACKER_MODE
extern MT_SharedPatternState *mt_shared;
#endif

#endif /* MT_SHARED_H */
