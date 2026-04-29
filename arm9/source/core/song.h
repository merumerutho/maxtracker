/*
 * song.h — In-memory song model for maxtracker.
 *
 * Defines all data structures for the song: cells, patterns,
 * envelopes, instruments, samples, and the top-level song container.
 */

#ifndef MT_SONG_H
#define MT_SONG_H

#include <nds.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define MT_MAX_CHANNELS     32
#define MT_MAX_ROWS         256
#define MT_MAX_PATTERNS     256
#define MT_MAX_ORDERS       200
#define MT_MAX_INSTRUMENTS  128
#define MT_MAX_SAMPLES      128
#define MT_MAX_ENV_NODES    25
#define MT_MAX_NOTEMAP      120
#define MT_MAX_GROOVES      16
#define MT_MAX_GROOVE_STEPS 16

#define NOTE_EMPTY  250
#define NOTE_CUT    254
#define NOTE_OFF    255

/* ------------------------------------------------------------------ */
/* Pattern cell: 5 bytes                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 note;
    u8 inst;
    u8 vol;
    u8 fx;
    u8 param;
} MT_Cell;

/* Pattern: header + variable-size flat cell array.
 * Cells are stored as cells[row * ncols + channel].
 * Use MT_CELL() macro for access. */
typedef struct {
    u16  nrows;         /* actual row count (1-256) */
    u8   ncols;         /* actual channel count at time of allocation */
    u8   _pad;
    MT_Cell cells[];    /* flexible array: nrows * ncols entries */
} MT_Pattern;

/* Access a cell in a variable-size pattern */
#define MT_CELL(pat, row, ch) (&(pat)->cells[(row) * (pat)->ncols + (ch)])

/* Calculate allocation size for a pattern */
#define MT_PATTERN_SIZE(nrows, ncols) \
    (sizeof(MT_Pattern) + (u32)(nrows) * (u32)(ncols) * sizeof(MT_Cell))

/* ------------------------------------------------------------------ */
/* Envelope                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 x;      /* tick position (absolute, 0-65535) */
    u8  y;      /* value (0-64 for volume/panning)   */
} MT_EnvelopeNode;

typedef struct {
    MT_EnvelopeNode nodes[MT_MAX_ENV_NODES];
    u8   node_count;    /* 0-25                          */
    u8   loop_start;    /* node index, or 255 = no loop  */
    u8   loop_end;      /* node index, or 255 = no loop  */
    u8   sus_start;     /* node index, or 255 = no sus   */
    u8   sus_end;       /* node index, or 255 = no sus   */
    bool enabled;
} MT_Envelope;

/* ------------------------------------------------------------------ */
/* Instrument                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    bool active;            /* false = unused slot                  */

    u8  global_volume;      /* 0-128                                */
    u8  fadeout;            /* 0-255 (XM fadeout / 32)              */
    u8  panning;            /* 0-255, 128 = center                  */
    u8  random_volume;      /* 0-255 (IT feature)                   */
    u8  nna;                /* New note action: 0=cut 1=cont 2=off 3=fade */
    u8  dct;                /* Duplicate check type                 */
    u8  dca;                /* Duplicate check action               */

    MT_Envelope env_vol;
    MT_Envelope env_pan;
    MT_Envelope env_pitch;

    /* Note-sample map: hi byte = sample (1-based), lo byte = note.
     * Identity map: all entries point to the same sample. */
    u16  notemap[MT_MAX_NOTEMAP];
    bool has_full_notemap;  /* false = identity (all same sample)   */

    u8   sample;            /* Default/shorthand sample (1-based, 0=none).
                             * Used when has_full_notemap is false.  */

    char name[23];
} MT_Instrument;

/* ------------------------------------------------------------------ */
/* Sample                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool active;            /* false = unused slot                  */

    u8  *pcm_data;          /* Signed PCM (in sample pool or malloc'd) */
    u32  length;            /* Total length in samples (not bytes)  */
    u32  loop_start;        /* Loop start in samples                */
    u32  loop_length;       /* Loop length in samples (0 = no loop) */
    u8   format;            /* 0 = 8-bit signed, 1 = 16-bit signed  */
    u8   loop_type;         /* 0 = none, 1 = forward                */

    u32  base_freq;         /* Base frequency in Hz                 */
    u8   default_volume;    /* 0-64                                 */
    u8   panning;           /* 0-255, 128 = center                  */
    u8   global_volume;     /* 0-64                                 */

    u8   vib_type;          /* Auto-vibrato: 0=sine 1=ramp 2=sq 3=rnd */
    u8   vib_depth;
    u8   vib_speed;
    u16  vib_rate;          /* Vibrato sweep rate                   */

    char name[33];          /* 32 chars + NUL — display-only, not in MAS */
    bool drawn;             /* true = drawn on touchscreen          */

    /* Convenience alias for format: 8 or 16 */
    u8   bits;
} MT_Sample;

/* ------------------------------------------------------------------ */
/* Groove                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 steps[MT_MAX_GROOVE_STEPS];
    u8 length;
} MT_Groove;

/* ------------------------------------------------------------------ */
/* Song: top-level container                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[33];          /* 32 chars + NUL — display-only, not in MAS */

    u8  initial_speed;
    u8  initial_tempo;
    u8  global_volume;
    u8  repeat_position;
    u8  channel_count;

    u8  channel_volume[MT_MAX_CHANNELS];
    u8  channel_panning[MT_MAX_CHANNELS];

    u8  order_count;
    u8  orders[MT_MAX_ORDERS];

    u8  inst_count;
    u8  samp_count;
    u8  patt_count;

    /* Patterns: NULL = empty (not allocated) */
    MT_Pattern *patterns[MT_MAX_PATTERNS];

    /* Instrument and sample arrays (inline, not pointers) */
    MT_Instrument instruments[MT_MAX_INSTRUMENTS];
    MT_Sample     samples[MT_MAX_SAMPLES];

    /* Groove table */
    MT_Groove grooves[MT_MAX_GROOVES];
    u8 groove_count;

    /* Flags for MAS compatibility */
    bool freq_linear;
    bool xm_mode;
    bool old_mode;
    bool old_effects;   /* MAS_HEADER_FLAG_OLD_EFFECTS (bit 1) */
    bool link_gxx;
} MT_Song;

/* Global song instance */
extern MT_Song song;

/* Initialize a new empty song */
void song_init(void);

/* Ensure a pattern exists (allocate if NULL with default size). Returns pointer. */
MT_Pattern *song_ensure_pattern(u8 index);

/* Allocate a pattern with specific dimensions. Frees any existing pattern at index.
 * SAFETY: If playback is active and the current pattern is being replaced,
 * the shared state cells pointer is temporarily nulled to prevent ARM7
 * from reading freed memory. */
MT_Pattern *song_alloc_pattern(u8 index, u16 nrows, u8 ncols);

/* Resize a pattern's row count, preserving existing cell data.
 * If growing, new rows are filled with empty cells. If shrinking,
 * rows past new_nrows are discarded. Returns the new pattern pointer,
 * or NULL on malloc failure (old pattern kept intact). */
MT_Pattern *song_resize_pattern(u8 index, u16 new_nrows);

/* Free all allocated patterns and sample PCM data */
void song_free(void);

/*
 * Pattern lifecycle hooks.
 *
 * song.c needs to notify "something" before it frees or replaces a
 * pattern, so that subsystems holding a pointer to the old cells (like
 * the playback engine sharing the cells pointer with ARM7) can detach
 * before the memory disappears. To avoid making the song model depend
 * on playback, the song module exposes registration hooks instead.
 *
 *   on_detach  — called BEFORE an existing pattern is freed
 *   on_reattach — called AFTER a new pattern has been allocated in its
 *                 place. May be a no-op if the subsystem isn't active.
 *
 * Either hook may be NULL. Whoever cares (currently playback.c) calls
 * song_set_pattern_lifecycle() at init time.
 */
typedef void (*song_lifecycle_cb)(void);
void song_set_pattern_lifecycle(song_lifecycle_cb on_detach,
                                song_lifecycle_cb on_reattach);

#endif /* MT_SONG_H */
