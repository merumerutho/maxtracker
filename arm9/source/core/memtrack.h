/*
 * memtrack.h — Memory usage tracking for maxtracker.
 *
 * Tracks how much RAM is used by patterns, samples, and the MAS buffer.
 * NDS has 4MB main RAM; this helps the user understand their budget.
 */

#ifndef MT_MEMTRACK_H
#define MT_MEMTRACK_H

#include <nds.h>

/* NDS main RAM: 4MB total. Reserve some for code, stack, BSS, heap overhead.
 *
 * MT_RAM_SAFETY is an extra margin on top of the code/stack/BSS reservation.
 * It keeps us away from the heap edge when transient allocations (WAV load
 * buffers, conversion temporaries, MAS parser state) briefly spike peak use
 * above the tracked "resident" total. Without it, a sample whose permanent
 * footprint fits the budget can still OOM mid-load when raw + buf16 coexist.
 */
#define MT_RAM_TOTAL       (4 * 1024 * 1024)     /* 4MB */
#define MT_RAM_RESERVED    (512 * 1024)           /* 512KB for code+stack+BSS */
#define MT_RAM_SAFETY      (384 * 1024)           /* 384KB headroom */
#define MT_RAM_AVAILABLE   (MT_RAM_TOTAL - MT_RAM_RESERVED - MT_RAM_SAFETY)

/* Memory usage snapshot */
typedef struct {
    u32 patterns;       /* total bytes in allocated patterns */
    u32 samples;        /* total bytes in sample PCM data */
    u32 song_struct;    /* sizeof(MT_Song) */
    u32 total;          /* sum of all tracked allocations */
    u32 available;      /* MT_RAM_AVAILABLE */
    u8  patt_count;     /* number of allocated patterns */
    u8  samp_count;     /* number of active samples */
} MT_MemUsage;

/* Calculate current memory usage from the global song */
void mt_mem_usage(MT_MemUsage *out);

/* Estimate memory needed to load a .mas file (from header counts).
 * Returns estimated total bytes needed. */
u32 mt_mem_estimate_mas(u8 patt_count, u8 samp_count, u32 file_size);

/* Check if enough memory is available. Returns true if OK. */
static inline bool mt_mem_check(u32 needed) {
    return needed <= MT_RAM_AVAILABLE;
}

/* Remaining headroom = MT_RAM_AVAILABLE minus what is already resident.
 * Useful for pre-flighting variable-size loads (WAV samples etc.). */
u32 mt_mem_free_budget(void);

#endif /* MT_MEMTRACK_H */
