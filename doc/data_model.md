# maxtracker -- Data Model

Parent: [README.md](../README.md)

---

## 1. Design Principles

The in-memory song model is optimized for **editing speed**, not storage efficiency. All data is uncompressed and directly addressable. MAS compression (pattern RLE, envelope delta encoding) only happens during save/export.

Memory is managed with plain `malloc`/`free`. Patterns and sample PCM are heap-allocated on demand; instruments and song metadata live inline in the statically-allocated `MT_Song` struct. There is no custom pool allocator. Budgeting against the NDS's limited main RAM is tracked separately (see § 3).

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

    u8  *pcm_data;             // malloc'd signed PCM (+4 wraparound bytes)
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

    u8  bits;                   // Convenience alias for format: 8 or 16
} MT_Sample;
// ~64 bytes per sample header, 128 max = ~8KB for headers
// PCM data is separate, malloc'd
```

### 2.7 MT_Groove -- Groove (Swing) Table

```c
#define MT_MAX_GROOVES      16
#define MT_MAX_GROOVE_STEPS 16

typedef struct {
    u8 steps[MT_MAX_GROOVE_STEPS];  // per-step tick counts
    u8 length;                      // active step count
} MT_Groove;
```

### 2.8 MT_Song -- Top-Level Song

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

    // Groove (swing) table
    MT_Groove grooves[MT_MAX_GROOVES];
    u8        groove_count;

    // Flags for MAS export
    bool freq_linear;           // true = linear frequency mode (XM/IT)
    bool xm_mode;               // true = XM volume column semantics
    bool old_mode;              // true = MOD/S3M instrument mode
    bool old_effects;           // MAS_HEADER_FLAG_OLD_EFFECTS (bit 1)
    bool link_gxx;              // true = shared Gxx memory
} MT_Song;
```

---

## 3. Memory Model and Budget Tracking

There are no pool allocators. Both of the large consumers use plain
`malloc`/`free` straight from the C heap:

- **Patterns** are allocated by `song_alloc_pattern()` / `song_ensure_pattern()`
  (`arm9/source/core/song.c`) to exactly `MT_PATTERN_SIZE(nrows, ncols)` bytes
  and freed by `song_alloc_pattern()` (when replacing) or `song_free()`.
- **Sample PCM** is `malloc`'d by the WAV loader, the MAS loader, and the LFE
  generators, with 4 trailing bytes for DS interpolation wraparound. It is
  freed by `song_free()` and when a sample slot is overwritten or deleted.

### 3.1 RAM Budget

The NDS has 4 MB of main RAM. Rather than carving out fixed pools, maxtracker
tracks how much of that budget is already resident and pre-flights variable-size
loads against the remainder. The constants live in
`arm9/source/core/memtrack.h`:

```c
#define MT_RAM_TOTAL       (4 * 1024 * 1024)   // 4MB
#define MT_RAM_RESERVED    (512 * 1024)         // 512KB for code+stack+BSS
#define MT_RAM_SAFETY      (128 * 1024)         // 128KB headroom (transient buffers)
#define MT_RAM_AVAILABLE   (MT_RAM_TOTAL - MT_RAM_RESERVED - MT_RAM_SAFETY)
```

`MT_RAM_AVAILABLE` is therefore ~3.36 MB. `MT_RAM_SAFETY` covers transient
allocations (WAV load buffers, conversion temporaries, per-pattern temp buffers
during MAS loading) that briefly coexist with the permanent ones.

### 3.2 Usage snapshot and pre-flight checks

`memtrack.c` walks the global song to compute a usage snapshot and to gate loads:

```c
typedef struct {
    u32 patterns;       // total bytes in allocated patterns
    u32 samples;        // total bytes in sample PCM data
    u32 song_struct;    // sizeof(MT_Song)
    u32 total;          // sum of all tracked allocations
    u32 available;      // MT_RAM_AVAILABLE
    u8  patt_count;     // number of allocated patterns
    u8  samp_count;     // number of active samples
} MT_MemUsage;

void mt_mem_usage(MT_MemUsage *out);                 // current resident usage
u32  mt_mem_estimate_mas(u8 patt_count, u32 sample_region_bytes); // load estimate
u32  mt_mem_free_budget(void);                       // AVAILABLE - resident
static inline bool mt_mem_check(u32 needed);         // needed <= AVAILABLE ?
```

`mt_mem_usage()` sums `MT_PATTERN_SIZE(nrows, ncols)` over allocated patterns and
`length * bytes_per_sample` over active samples. The UI uses `mt_mem_free_budget()`
to warn before loading a sample or song that would not fit.

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
    .channel_count   = 32,      // MT_MAX_CHANNELS — all 32 channels available
    .channel_volume  = { 64, ... },
    .channel_panning = { 128, ... },  // All center
    .order_count     = 1,
    .orders          = { 0 },   // One pattern in the order
    .inst_count      = 1,
    .samp_count      = 1,       // Sample 1 (square wave) is seeded
    .patt_count      = 1,
    .groove_count    = 0,
    .freq_linear     = true,    // Linear frequency mode (XM standard)
    .xm_mode         = true,    // XM volume column semantics
    .old_mode        = false,
    .old_effects     = false,
    .link_gxx        = false,
}
```

Pattern 0 is allocated with 64 rows, all cells empty.

Instrument 1 (index 0) is active with `global_volume = 128`, `panning = 0xC0`
(`0x80 | 64` = center stereo), and `sample = 1`; no envelopes.

Sample 1 (index 0) is **seeded with a 256-sample square wave** so playback works
out of the box: 8-bit (`format = 0`, `bits = 8`), `base_freq = 8363`,
`default_volume = 64`, `panning = 0xC0`, forward loop over the whole sample,
named "Square". `samp_count` is therefore 1, not 0.

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

The clipboard is a **tagged union**: one global instance holds exactly one
kind of payload at a time (a pattern cell block, order-list entries, or a
full instrument). Paste operations check the tag and refuse mismatched
content.

```c
typedef enum {
    CLIP_NONE = 0,
    CLIP_CELLS,       // rectangular block of pattern cells
    CLIP_ORDERS,      // order list entries (pattern references)
    CLIP_INSTRUMENT,  // full instrument struct
} ClipboardType;

#define CLIP_ORDER_MAX 32

typedef struct {
    ClipboardType type;

    // CLIP_CELLS: malloc'd cell array [cell_rows * cell_channels]
    MT_Cell *cell_data;
    u16      cell_rows;
    u8       cell_channels;

    // CLIP_ORDERS: small inline buffer
    u8  order_data[CLIP_ORDER_MAX];
    int order_count;

    // CLIP_INSTRUMENT: instrument struct (no PCM — just the reference)
    MT_Instrument inst_data;
} MT_Clipboard;
```

Cell copy captures a rectangular block; paste stamps it at the cursor,
clipping to pattern boundaries. In LSDJ style, copy and paste are instant
(SELECT+A / SELECT+B).

A separate single-slot **note clipboard** (`MT_NoteClipboard`: note, inst,
valid) backs the M8-style A-press note stamping and is independent of the
tagged clipboard above.
