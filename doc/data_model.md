# maxtracker -- Data Model

Parent: [DESIGN.md](../DESIGN.md)

---

## 1. Design Principles

The in-memory song model is optimized for **editing speed**, not storage efficiency. All data is uncompressed and directly addressable. MAS compression (pattern RLE, envelope delta encoding) only happens during save/export.

Memory is managed through pool allocators for the two largest consumers (patterns and samples). Small fixed-size structures (instruments, song metadata) are statically allocated.

---

## 2. Core Structures

### 2.1 MT_Cell -- Pattern Cell

The atomic unit of pattern data. One cell per channel per row.

```c
typedef struct {
    u8 note;    // 0-119 = note, 250 = empty, 254 = cut, 255 = off
    u8 inst;    // 0 = none, 1-128 = instrument number
    u8 vol;     // Volume column command. 0 = empty (XM mode).
    u8 fx;      // Effect command (IT-style: A=1..Z=26, ext 27-30). 0 = none.
    u8 param;   // Effect parameter.
} MT_Cell;      // 5 bytes, no padding
```

`MT_Cell` is intentionally identical in layout to mmutil's `PatternEntry` and to mas2xm's `PatternEntry`. Note value 250 (`NOTE_EMPTY`) marks an unused cell.

### 2.2 MT_Pattern -- Pattern

```c
#define MT_MAX_ROWS     256
#define MT_MAX_CHANNELS  32

typedef struct {
    u16     nrows;      // actual row count (1-256)
    u8      ncols;      // actual channel count at time of allocation
    u8      _pad;
    MT_Cell cells[];    // flexible array: nrows * ncols entries
} MT_Pattern;

#define MT_CELL(pat, row, ch) \
    (&(pat)->cells[(row) * (pat)->ncols + (ch)])

#define MT_PATTERN_SIZE(nrows, ncols) \
    (sizeof(MT_Pattern) + (u32)(nrows) * (u32)(ncols) * sizeof(MT_Cell))

// Maximum size (256 rows × 32 ch): 4 + 256*32*5 = 40,964 bytes
// 4-channel 64-row pattern:        4 +  64* 4*5 =  1,284 bytes
```

Patterns are heap-allocated to exactly the size needed for `nrows * ncols` cells. A NULL pattern pointer in the song's pattern table means an empty pattern (all cells are NOTE_EMPTY with no effects). The UI renders empty patterns as blank without needing allocated memory.

**Channel-count optimization (in effect):** Allocations honor `song.channel_count` at the time the pattern is created; a 4-channel song's pattern is ~10x smaller than a 32-channel one. Always access cells through the `MT_CELL` macro, never via raw `cells[r][c]`, because the second dimension is `ncols` (per-pattern) not `MT_MAX_CHANNELS`. Patterns allocated under one channel-count and accessed under another would walk out of bounds; see `song_alloc_pattern()` for the allocation API.

### 2.3 MT_EnvelopeNode -- Envelope Node

```c
#define MT_MAX_ENV_NODES 25

typedef struct {
    u16 x;  // tick position (absolute, 0-65535)
    u8  y;  // value (0-64 for volume, 0-64 for panning)
} MT_EnvelopeNode;
```

Stored as absolute positions, not delta-encoded. Delta/range encoding is computed only during MAS export.

### 2.4 MT_Envelope -- Envelope

```c
typedef struct {
    MT_EnvelopeNode nodes[MT_MAX_ENV_NODES];
    u8  node_count;     // 0-25
    u8  loop_start;     // 0-24 or 255 = no loop
    u8  loop_end;       // 0-24 or 255 = no loop
    u8  sus_start;      // 0-24 or 255 = no sustain
    u8  sus_end;        // 0-24 or 255 = no sustain
    bool enabled;       // envelope affects playback
} MT_Envelope;
```

### 2.5 MT_Instrument -- Instrument

```c
#define MT_MAX_INSTRUMENTS 128
#define MT_MAX_NOTEMAP     120

typedef struct {
    bool active;                // false = unused instrument slot

    u8  global_volume;          // 0-128
    u8  fadeout;                // 0-255 (stored as XM fadeout / 32)
    u8  panning;                // 0-255, 128 = center
    u8  random_volume;          // 0-255 (IT feature)
    u8  nna;                    // New note action: 0=cut, 1=continue, 2=off, 3=fade
    u8  dct;                    // Duplicate check type
    u8  dca;                    // Duplicate check action

    MT_Envelope env_vol;
    MT_Envelope env_pan;
    MT_Envelope env_pitch;

    // Note map: hi byte = sample (1-based), lo byte = note (0-119)
    u16  notemap[MT_MAX_NOTEMAP];
    bool has_full_notemap;      // false = all entries map to same sample (identity)
    u8   sample;                // Default/shorthand sample (1-based; used when
                                // has_full_notemap is false)

    char name[23];              // Display name (not stored in MAS, for UI only)
} MT_Instrument;
// ~356 bytes per instrument, 128 max = ~44KB total
```

**`env_flags` is not stored in the model.** It's a serialization-only byte
that `mas_write.c` builds from the three `MT_Envelope` instances at save
time, and `mas_load.c` decodes back into them at load time. The bit layout
matches mmutil exactly:

```
bit 0 — volume envelope exists      (set when env_vol.node_count > 0)
bit 1 — panning envelope exists     (set when env_pan.node_count > 0)
bit 2 — pitch envelope exists       (set when env_pitch.node_count > 0)
bit 3 — volume envelope enabled     (mirrors env_vol.enabled)
bits 4-7 — unused (must be zero)
```

The volume envelope has a separate `enabled` bit because IT/XM allow you
to keep envelope data while temporarily disabling its effect on playback.
**Pan and pitch envelopes do not.** maxmod's playback engine applies them
whenever the EXISTS bit is set, with no further gating, so the model's
`env_pan.enabled` and `env_pitch.enabled` fields are derived from
`node_count > 0` after load and are not consulted by serialization.

The instrument editor's "enable pan/pitch envelope" toggle therefore
operates by creating a starter node set when toggled on, or destroying
all nodes when toggled off (see `instrument_view.c`'s `env_create_default`
and `env_destroy` helpers). The toggle is destructive and not undoable,
matching the rest of the instrument-editor model.

An earlier version of maxtracker invented bits 4 and 5 of `env_flags` as
"pan envelope enabled" and "pitch envelope enabled". Those bits do not
exist in mmutil and the maxmod playback engine ignores them. Files written
by older maxtracker versions that have those bits set still load
correctly because the current loader simply doesn't read bits 4-7.

### 2.6 MT_Sample -- Sample

```c
#define MT_MAX_SAMPLES 128

typedef struct {
    bool active;                // false = unused sample slot

    u8  *pcm_data;             // Pointer into sample pool (signed PCM)
    u32 length;                 // Total length in samples (not bytes)
    u32 loop_start;             // Loop start in samples
    u32 loop_length;            // Loop length in samples (0 = no loop)
    u8  format;                 // 0 = 8-bit signed, 1 = 16-bit signed
    u8  loop_type;              // 0 = none, 1 = forward

    u32 base_freq;              // Base frequency in Hz
    u8  default_volume;         // 0-64
    u8  panning;                // 0-255, 128 = center
    u8  global_volume;          // 0-64

    u8  vib_type;               // Auto-vibrato: 0=sine, 1=ramp, 2=square, 3=random
    u8  vib_depth;
    u8  vib_speed;
    u16 vib_rate;               // Vibrato sweep rate

    char name[33];              // 32 chars + NUL. Display only — MAS does not
                                // serialize sample names. Expanded from 23 in
                                // 2026-04 when the on-screen QWERTY keyboard
                                // (`text_input.c`) made names user-editable.
    bool drawn;                 // true if sample was drawn on touchscreen (vs loaded)
} MT_Sample;
// ~64 bytes per sample header, 128 max = ~8KB for headers
// PCM data is separate, in the sample pool
```

### 2.7 MT_Song -- Top-Level Song

```c
#define MT_MAX_PATTERNS 256
#define MT_MAX_ORDERS   200

typedef struct {
    char name[33];              // 32 chars + NUL. Display only — MAS has no
                                // song-name section. Expanded from 21 in
                                // 2026-04 to match the sample-name field; the
                                // PROJECT view's "Song Name" row opens the
                                // shared `text_input` keyboard widget.

    u8  initial_speed;          // Ticks per row (1-31)
    u8  initial_tempo;          // BPM (32-255)
    u8  global_volume;          // 0-128
    u8  repeat_position;        // Order index to loop to at song end
    u8  channel_count;          // Active channels: 4, 8, 16, 24, or 32

    u8  channel_volume[MT_MAX_CHANNELS];    // Per-channel volume (0-64)
    u8  channel_panning[MT_MAX_CHANNELS];   // Per-channel panning (0-255)

    u8  order_count;            // 1-200
    u8  orders[MT_MAX_ORDERS];  // Pattern index per order position

    u8  inst_count;             // Highest active instrument number
    u8  samp_count;             // Highest active sample number
    u8  patt_count;             // Highest active pattern number

    MT_Instrument instruments[MT_MAX_INSTRUMENTS];
    MT_Sample     samples[MT_MAX_SAMPLES];
    MT_Pattern   *patterns[MT_MAX_PATTERNS]; // NULL = empty pattern

    // Flags for MAS export
    bool freq_linear;           // true = linear frequency mode (XM/IT)
    bool xm_mode;               // true = XM volume column semantics
    bool old_mode;              // true = MOD/S3M instrument mode
    bool link_gxx;              // true = shared Gxx memory
} MT_Song;
```

---

## 3. Memory Pools

### 3.1 Pattern Pool

```c
#define MT_PATTERN_POOL_SIZE  (640 * 1024)  // 640KB

typedef struct {
    u8      memory[MT_PATTERN_POOL_SIZE];
    size_t  used;           // bytes allocated
    size_t  peak;           // high-water mark
    int     count;          // patterns currently allocated
} MT_PatternPool;
```

Allocation strategy: **simple bump allocator with free list**. Patterns are variable-size (depends on nrows, but width is fixed at 32 channels). When a pattern is deleted, its memory is added to a free list. When a new pattern is created, the free list is checked first (best-fit), then the bump pointer is advanced.

Fragmentation is acceptable because patterns are relatively uniform in size (most are 64 rows = ~10KB). A compaction pass could be triggered on save if fragmentation exceeds a threshold.

```c
MT_Pattern *mt_pattern_alloc(MT_PatternPool *pool, u16 nrows);
void        mt_pattern_free(MT_PatternPool *pool, MT_Pattern *pat);
size_t      mt_pattern_pool_available(const MT_PatternPool *pool);
```

### 3.2 Sample Pool

```c
#define MT_SAMPLE_POOL_SIZE  (1536 * 1024)  // 1.5MB

typedef struct {
    u8      memory[MT_SAMPLE_POOL_SIZE];
    size_t  used;
    size_t  peak;
    int     count;
} MT_SamplePool;
```

Same allocation strategy as pattern pool. Sample data tends to be more variable in size (tiny drawn waveforms vs. long recordings), so fragmentation is more of a concern. The pool reports available space so the UI can warn before loading a sample that won't fit.

```c
u8   *mt_sample_alloc(MT_SamplePool *pool, u32 byte_size);
void  mt_sample_free(MT_SamplePool *pool, u8 *data, u32 byte_size);
size_t mt_sample_pool_available(const MT_SamplePool *pool);
```

---

## 4. Default Song State

A new empty song initializes to:

```c
{
    .name            = "untitled",
    .initial_speed   = 6,
    .initial_tempo   = 125,
    .global_volume   = 64,
    .repeat_position = 0,
    .channel_count   = 8,       // Default 8 channels
    .channel_volume  = { 64, ... },
    .channel_panning = { 128, ... },  // All center
    .order_count     = 1,
    .orders          = { 0 },   // One pattern in the order
    .inst_count      = 1,
    .samp_count      = 0,
    .patt_count      = 1,
    .freq_linear     = true,    // Linear frequency mode (XM standard)
    .xm_mode         = true,    // XM volume column semantics
    .old_mode        = false,
    .link_gxx        = false,
}
```

Pattern 0 is allocated with 64 rows, all cells empty. Instrument 1 is active with default settings (global_volume=128, no envelopes). No samples loaded.

---

## 5. Shared Memory Protocol (ARM9 <-> ARM7)

The patched maxmod on ARM7 reads pattern data directly from the `MT_Pattern` structures in main RAM. For this to work:

1. **Pattern pointers** are communicated via IPC (ARM9 sends the address of the current pattern's `cells` array).
2. **ARM9 must flush cache** (`DC_FlushRange`) after modifying pattern data and before ARM7 reads it.
3. **No concurrent write/read of the same cell**: ARM9 must not modify a row that ARM7 is currently reading. Since ARM7 reads one row per tick (~3-15ms), and ARM9 writes are instantaneous, this is safe as long as ARM9 doesn't edit the currently-playing row. The UI can enforce this by not allowing edits on the playback cursor row (or by accepting a one-tick glitch).

Sample PCM data is also shared. ARM9 loads/draws samples, flushes cache, and ARM7 reads them during mixing.

See [audio_engine.md](audio_engine.md) for the full IPC protocol.

---

## 6. Undo System

A ring buffer of edit operations, each storing:

```c
typedef struct {
    u8  type;       // UNDO_CELL, UNDO_ROW, UNDO_BLOCK, UNDO_INSTRUMENT, etc.
    u8  pattern;    // which pattern index
    u16 row;        // which row (or start row for blocks)
    u8  channel;    // which channel (or start channel)
    u8  count_ch;   // block width (1 for single cell, max MT_MAX_CHANNELS)
    u16 count_rows; // block height (must be u16: MT_MAX_ROWS == 256)
    MT_Cell *old_data; // malloc'd copy of previous cell data
} MT_UndoEntry;

#define MT_UNDO_DEPTH 64
```

Undo buffer is circular. Oldest entries are silently dropped when full. Only pattern edits are undoable in v1 (instrument/sample edits could be added later).

---

## 7. Clipboard

```c
typedef struct {
    MT_Cell *data;      // malloc'd cell array
    u16     rows;       // height of copied block
    u8      channels;   // width of copied block
    bool    valid;      // true if clipboard has content
} MT_Clipboard;
```

One global clipboard. Copy captures a rectangular block of cells. Paste stamps it at the cursor position, clipping to pattern boundaries. In LSDJ style, copy and paste are instant (SELECT+A / SELECT+B).
