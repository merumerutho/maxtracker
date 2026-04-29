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
 * It covers transient allocations (WAV load buffers, conversion temporaries,
 * per-pattern temp buffers during MAS loading). The MAS loader streams from
 * disk so there is no full-file buffer competing with permanent allocations.
 */
#define MT_RAM_TOTAL       (4 * 1024 * 1024)     /* 4MB */
#define MT_RAM_RESERVED    (512 * 1024)           /* 512KB for code+stack+BSS */
#define MT_RAM_SAFETY      (128 * 1024)           /* 128KB headroom */
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

/* Estimate memory needed to load a .mas file.
 * sample_region_bytes: actual byte span of sample data in the file
 * (computed from offset tables, not guessed from file_size). */
u32 mt_mem_estimate_mas(u8 patt_count, u32 sample_region_bytes);

/* Check if enough memory is available. Returns true if OK. */
static inline bool mt_mem_check(u32 needed) {
    return needed <= MT_RAM_AVAILABLE;
}

/* Remaining headroom = MT_RAM_AVAILABLE minus what is already resident.
 * Useful for pre-flighting variable-size loads (WAV samples etc.). */
u32 mt_mem_free_budget(void);

#endif /* MT_MEMTRACK_H */
