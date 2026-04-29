# maxtracker -- Test Plan

Parent: [DESIGN.md](../DESIGN.md)

---

## Section 1: Unit Tests (Host x86 or NDS)

These tests are designed to run on the host build (x86/x64) using a minimal
test harness.  They depend only on the C standard library and the maxtracker
core modules; no libnds or ARM hardware is required.  For host builds,
provide a stub `<nds.h>` that typedefs `u8`/`u16`/`u32`/`s8`/`s16`/`s32`
and `bool` to their standard equivalents.

All tests follow a common pattern: set up state, call the function under
test, assert postconditions.  A failing assertion prints the test name,
file, line, expected vs actual values, then exits non-zero.

Source files under test:

| Module | Source | Header |
|--------|--------|--------|
| Song model | `arm9/source/core/song.c` | `arm9/source/core/song.h` |
| Clipboard | `arm9/source/core/clipboard.c` | `arm9/source/core/clipboard.h` |
| Undo | `arm9/source/core/undo.c` | `arm9/source/core/undo.h` |
| MAS write | `arm9/source/io/mas_write.c` | `arm9/source/io/mas_write.h` |
| MAS load | `arm9/source/io/mas_load.c` | `arm9/source/io/mas_load.h` |
| WAV load | `arm9/source/io/wav_load.c` | `arm9/source/io/wav_load.h` |

---

### 1.1 Song Model Tests

#### T1.1.1 `test_song_init_defaults`

**Verifies:** `song_init()` sets all fields to documented default values
(DESIGN.md section 4, data_model.md section 4).

```
call song_init()
assert song.initial_speed  == 6
assert song.initial_tempo  == 125
assert song.global_volume  == 64
assert song.repeat_position == 0
assert song.channel_count  == 8
assert song.freq_linear    == true
assert song.xm_mode        == true
assert song.order_count    == 1
assert song.orders[0]      == 0
assert song.patt_count     == 1
assert song.inst_count     == 1
assert strcmp(song.name, "untitled") == 0
for i in 0..MT_MAX_CHANNELS-1:
    assert song.channel_volume[i]  == 64
    assert song.channel_panning[i] == 128
assert song.patterns[0] != NULL
assert song.patterns[0]->nrows == 64
for i in 1..MT_MAX_PATTERNS-1:
    assert song.patterns[i] == NULL
call song_free()
```

#### T1.1.2 `test_song_init_pattern0_cells_empty`

**Verifies:** After `song_init()`, every cell in pattern 0 is empty
(`note == NOTE_EMPTY`, all other fields zero).

```
call song_init()
pat = song.patterns[0]
assert pat != NULL
for row in 0..pat->nrows-1:
    for ch in 0..MT_MAX_CHANNELS-1:
        assert pat->cells[row][ch].note  == NOTE_EMPTY  // 250
        assert pat->cells[row][ch].inst  == 0
        assert pat->cells[row][ch].vol   == 0
        assert pat->cells[row][ch].fx    == 0
        assert pat->cells[row][ch].param == 0
call song_free()
```

#### T1.1.3 `test_song_ensure_pattern_allocates`

**Verifies:** `song_ensure_pattern()` allocates a new pattern when index is
NULL, returns the same pointer on second call, and updates `patt_count`.

```
call song_init()
assert song.patterns[5] == NULL
pat = song_ensure_pattern(5)
assert pat != NULL
assert pat->nrows == 64
assert song.patt_count >= 6
// All cells must be empty
assert pat->cells[0][0].note == NOTE_EMPTY
// Second call returns same pointer
pat2 = song_ensure_pattern(5)
assert pat2 == pat
call song_free()
```

#### T1.1.4 `test_song_ensure_pattern_existing`

**Verifies:** `song_ensure_pattern()` on an already-allocated index returns
the existing pointer without modifying its contents.

```
call song_init()
pat = song.patterns[0]
// Write data into cell [0][0]
pat->cells[0][0].note = 60   // C-5
pat->cells[0][0].inst = 1
pat2 = song_ensure_pattern(0)
assert pat2 == pat
assert pat2->cells[0][0].note == 60
assert pat2->cells[0][0].inst == 1
call song_free()
```

#### T1.1.5 `test_song_free_releases_all`

**Verifies:** `song_free()` frees every allocated pattern and sets pointers
to NULL.

```
call song_init()
song_ensure_pattern(0)
song_ensure_pattern(3)
song_ensure_pattern(10)
call song_free()
for i in 0..MT_MAX_PATTERNS-1:
    assert song.patterns[i] == NULL
```

#### T1.1.6 `test_mt_cell_size_5_bytes`

**Verifies:** `sizeof(MT_Cell) == 5` -- no padding. This is critical for
binary compatibility with MAS pattern layout.

```
assert sizeof(MT_Cell) == 5
```

#### T1.1.7 `test_mt_pattern_cell_addressing`

**Verifies:** `MT_Pattern.cells[row][channel]` is correctly laid out as a
contiguous array of `MT_MAX_ROWS * MT_MAX_CHANNELS` cells with no gaps.

```
call song_init()
pat = song.patterns[0]
// Write a unique value at known positions
pat->cells[0][0].note  = 10
pat->cells[0][31].note = 20
pat->cells[63][0].note = 30
pat->cells[63][31].note = 40
// Verify raw byte access matches struct access
u8 *base = (u8 *)&pat->cells[0][0]
assert base[0] == 10                                            // [0][0].note
assert base[(31) * 5] == 20                                     // [0][31].note
assert base[(63 * MT_MAX_CHANNELS) * 5] == 30                  // [63][0].note
assert base[(63 * MT_MAX_CHANNELS + 31) * 5] == 40             // [63][31].note
call song_free()
```

---

### 1.2 Clipboard Tests

#### T1.2.1 `test_clipboard_copy_single_cell`

**Verifies:** `clipboard_copy_cell()` copies exactly one cell and sets
`clipboard.valid`, `clipboard.rows == 1`, `clipboard.channels == 1`.

```
call song_init()
pat = song.patterns[0]
pat->cells[5][2].note = 48
pat->cells[5][2].inst = 3
clipboard_copy_cell(pat, 5, 2)
assert clipboard.valid    == true
assert clipboard.rows     == 1
assert clipboard.channels == 1
assert clipboard.data[0].note == 48
assert clipboard.data[0].inst == 3
call clipboard_free()
call song_free()
```

#### T1.2.2 `test_clipboard_copy_block`

**Verifies:** `clipboard_copy()` copies a rectangular block with correct
dimensions and data order (row-major, channels contiguous within each row).

```
call song_init()
pat = song.patterns[0]
// Fill a 3-row x 2-channel block with known data
for r in 0..2:
    for c in 0..1:
        pat->cells[10+r][4+c].note = (r * 2 + c) + 60
        pat->cells[10+r][4+c].inst = (r * 2 + c) + 1
clipboard_copy(pat, 10, 12, 4, 5)   // rows 10-12, channels 4-5
assert clipboard.valid    == true
assert clipboard.rows     == 3
assert clipboard.channels == 2
// Verify data order: clipboard.data[r * channels + c]
for r in 0..2:
    for c in 0..1:
        idx = r * 2 + c
        assert clipboard.data[idx].note == (r * 2 + c) + 60
        assert clipboard.data[idx].inst == (r * 2 + c) + 1
call clipboard_free()
call song_free()
```

#### T1.2.3 `test_clipboard_paste_at_origin`

**Verifies:** `clipboard_paste()` stamps copied data at cursor position.

```
call song_init()
pat = song.patterns[0]
// Write data, copy it
pat->cells[0][0].note = 72
pat->cells[0][0].inst = 5
clipboard_copy_cell(pat, 0, 0)
// Clear origin, paste to a different location
pat->cells[0][0].note = NOTE_EMPTY
clipboard_paste(pat, 20, 3)
assert pat->cells[20][3].note == 72
assert pat->cells[20][3].inst == 5
call clipboard_free()
call song_free()
```

#### T1.2.4 `test_clipboard_paste_clips_to_boundary`

**Verifies:** `clipboard_paste()` clips when the pasted block extends beyond
the pattern boundary (does not crash, does not write out of bounds).

```
call song_init()
pat = song.patterns[0]   // nrows = 64
// Copy a 4-row x 2-channel block
for r in 0..3:
    for c in 0..1:
        pat->cells[r][c].note = 60 + r
clipboard_copy(pat, 0, 3, 0, 1)
// Paste at row 62 -- only 2 rows fit (62, 63)
clipboard_paste(pat, 62, 0)
assert pat->cells[62][0].note == 60
assert pat->cells[63][0].note == 61
// Rows 64+ are out of bounds -- verify no crash occurred
// Paste at channel 31 -- only 1 channel fits
clipboard_paste(pat, 0, 31)
assert pat->cells[0][31].note == 60
call clipboard_free()
call song_free()
```

#### T1.2.5 `test_clipboard_copy_replaces_previous`

**Verifies:** A second `clipboard_copy()` frees the previous data and
replaces it.

```
call song_init()
pat = song.patterns[0]
pat->cells[0][0].note = 60
clipboard_copy_cell(pat, 0, 0)
assert clipboard.data[0].note == 60
pat->cells[1][0].note = 72
clipboard_copy_cell(pat, 1, 0)
assert clipboard.data[0].note == 72
assert clipboard.rows == 1
call clipboard_free()
call song_free()
```

#### T1.2.6 `test_clipboard_paste_invalid`

**Verifies:** `clipboard_paste()` is a no-op when clipboard is empty or
pattern is NULL.

```
call song_init()
pat = song.patterns[0]
clipboard_free()  // ensure empty
clipboard_paste(pat, 0, 0)       // should not crash
clipboard_paste(NULL, 0, 0)      // should not crash
call song_free()
```

---

### 1.3 Undo System Tests

#### T1.3.1 `test_undo_push_pop_single_cell`

**Verifies:** `undo_push_cell()` captures the cell state before a write, and
`undo_pop()` restores it.

```
call song_init()
call undo_init()
pat = song.patterns[0]
pat->cells[10][3].note = 60
pat->cells[10][3].inst = 1
// Push state (captures note=60, inst=1)
undo_push_cell(0, 10, 3)
// Modify the cell
pat->cells[10][3].note = 72
pat->cells[10][3].inst = 5
// Pop should restore original
result = undo_pop(&song)
assert result == true
assert pat->cells[10][3].note == 60
assert pat->cells[10][3].inst == 1
call undo_free()
call song_free()
```

#### T1.3.2 `test_undo_push_pop_block`

**Verifies:** `undo_push_block()` snapshots a rectangular area and
`undo_pop()` restores all cells in the block.

```
call song_init()
call undo_init()
pat = song.patterns[0]
// Fill 3x2 block with known data
for r in 0..2:
    for c in 0..1:
        pat->cells[5+r][2+c].note = 48 + r*2 + c
undo_push_block(0, 5, 7, 2, 3)   // rows 5-7, channels 2-3
// Overwrite the block
for r in 0..2:
    for c in 0..1:
        pat->cells[5+r][2+c].note = 0
        pat->cells[5+r][2+c].inst = 99
result = undo_pop(&song)
assert result == true
for r in 0..2:
    for c in 0..1:
        assert pat->cells[5+r][2+c].note == 48 + r*2 + c
        assert pat->cells[5+r][2+c].inst == 0  // was originally 0
call undo_free()
call song_free()
```

#### T1.3.3 `test_undo_multiple_pops`

**Verifies:** Multiple undos pop in LIFO order (most recent edit first).

```
call song_init()
call undo_init()
pat = song.patterns[0]
// Edit 1: write note at [0][0]
pat->cells[0][0].note = NOTE_EMPTY
undo_push_cell(0, 0, 0)
pat->cells[0][0].note = 60
// Edit 2: write note at [1][0]
pat->cells[1][0].note = NOTE_EMPTY
undo_push_cell(0, 1, 0)
pat->cells[1][0].note = 72
// Pop edit 2 first
undo_pop(&song)
assert pat->cells[1][0].note == NOTE_EMPTY
assert pat->cells[0][0].note == 60  // edit 1 still in effect
// Pop edit 1
undo_pop(&song)
assert pat->cells[0][0].note == NOTE_EMPTY
call undo_free()
call song_free()
```

#### T1.3.4 `test_undo_ring_buffer_overflow`

**Verifies:** After pushing more than `MT_UNDO_DEPTH` (64) entries, the
oldest entries are silently dropped and the most recent 64 can still be
undone.

```
call song_init()
call undo_init()
pat = song.patterns[0]
// Push 80 edits (exceeds ring size of 64)
for i in 0..79:
    row = i % 64
    pat->cells[row][0].note = NOTE_EMPTY
    undo_push_cell(0, row, 0)
    pat->cells[row][0].note = i + 1
// Should be able to pop the most recent 64
count = 0
while undo_pop(&song):
    count++
assert count == 64
// Further pops should return false
assert undo_pop(&song) == false
call undo_free()
call song_free()
```

#### T1.3.5 `test_undo_pop_empty`

**Verifies:** `undo_pop()` returns false and does nothing when the ring
buffer is empty.

```
call song_init()
call undo_init()
result = undo_pop(&song)
assert result == false
call undo_free()
call song_free()
```

#### T1.3.6 `test_undo_push_null_pattern`

**Verifies:** `undo_push_cell()` is a no-op when the referenced pattern is
NULL (does not crash or corrupt the ring).

```
call song_init()
call undo_init()
// Pattern 50 is not allocated
assert song.patterns[50] == NULL
undo_push_cell(50, 0, 0)  // should silently do nothing
assert undo_pop(&song) == false  // nothing was pushed
call undo_free()
call song_free()
```

---

### 1.4 MAS Serialization Roundtrip Tests

#### T1.4.1 `test_mas_roundtrip_empty_song`

**Verifies:** A freshly initialized song survives a write/load cycle with
all header fields, order table, and empty pattern data preserved.

```
call song_init()
rc = mas_write("/tmp/test_empty.mas", &song)
assert rc == 0
// Prepare a second song struct for loading
MT_Song loaded
memset(&loaded, 0, sizeof(loaded))
rc = mas_load("/tmp/test_empty.mas", &loaded)
assert rc == 0
assert loaded.initial_speed   == song.initial_speed   // 6
assert loaded.initial_tempo   == song.initial_tempo    // 125
assert loaded.global_volume   == song.global_volume    // 64
assert loaded.repeat_position == song.repeat_position  // 0
assert loaded.order_count     == song.order_count      // 1
assert loaded.orders[0]       == 0
assert loaded.patt_count      == song.patt_count       // 1
assert loaded.freq_linear     == song.freq_linear      // true
assert loaded.xm_mode         == song.xm_mode          // true
// Channel volumes and panning
for i in 0..MT_MAX_CHANNELS-1:
    assert loaded.channel_volume[i]  == song.channel_volume[i]
    assert loaded.channel_panning[i] == song.channel_panning[i]
// Pattern 0: all cells should be empty
pat = loaded.patterns[0]
assert pat != NULL
for r in 0..pat->nrows-1:
    for c in 0..MT_MAX_CHANNELS-1:
        assert pat->cells[r][c].note == NOTE_EMPTY
// Cleanup
song_free()   // frees original song.patterns
// Free loaded patterns
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.4.2 `test_mas_roundtrip_pattern_data`

**Verifies:** Pattern cell data (notes, instruments, volume, effects,
parameters) survives the write -> RLE compress -> load -> RLE decompress
cycle bit-perfectly.

```
call song_init()
pat = song.patterns[0]
// Write diverse data: notes, note-off, note-cut, effects
pat->cells[0][0]  = { .note = 48,        .inst = 1,  .vol = 40, .fx = 0,  .param = 0 }
pat->cells[1][0]  = { .note = 60,        .inst = 2,  .vol = 0,  .fx = 15, .param = 0x20 }
pat->cells[2][0]  = { .note = NOTE_CUT,  .inst = 0,  .vol = 0,  .fx = 0,  .param = 0 }
pat->cells[3][0]  = { .note = NOTE_OFF,  .inst = 0,  .vol = 0,  .fx = 0,  .param = 0 }
pat->cells[0][1]  = { .note = 72,        .inst = 3,  .vol = 0,  .fx = 1,  .param = 0x0A }
// Repeated value (tests cache suppression + MF carry-forward)
pat->cells[4][0]  = { .note = 48,        .inst = 1,  .vol = 40, .fx = 0,  .param = 0 }
pat->cells[5][0]  = { .note = 48,        .inst = 1,  .vol = 40, .fx = 0,  .param = 0 }
rc = mas_write("/tmp/test_data.mas", &song)
assert rc == 0
MT_Song loaded
memset(&loaded, 0, sizeof(loaded))
rc = mas_load("/tmp/test_data.mas", &loaded)
assert rc == 0
lpat = loaded.patterns[0]
assert lpat != NULL
// Compare every populated cell
assert lpat->cells[0][0].note == 48
assert lpat->cells[0][0].inst == 1
assert lpat->cells[0][0].vol  == 40
assert lpat->cells[1][0].fx   == 15
assert lpat->cells[1][0].param == 0x20
assert lpat->cells[2][0].note == NOTE_CUT
assert lpat->cells[3][0].note == NOTE_OFF
assert lpat->cells[0][1].note == 72
assert lpat->cells[0][1].inst == 3
// Cached values must be carried forward correctly
assert lpat->cells[4][0].note == 48
assert lpat->cells[4][0].inst == 1
assert lpat->cells[4][0].vol  == 40
assert lpat->cells[5][0].note == 48
assert lpat->cells[5][0].inst == 1
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.4.3 `test_mas_roundtrip_multiple_patterns`

**Verifies:** Multiple patterns with varying row counts survive roundtrip.

```
call song_init()
// Pattern 0: default 64 rows (from song_init)
// Pattern 1: 128 rows
pat1 = song_ensure_pattern(1)
pat1->nrows = 128
pat1->cells[127][0].note = 84
// Pattern 2: 32 rows
pat2 = song_ensure_pattern(2)
pat2->nrows = 32
pat2->cells[31][15].note = 36
song.patt_count = 3
song.order_count = 3
song.orders[0] = 0
song.orders[1] = 1
song.orders[2] = 2
rc = mas_write("/tmp/test_multi.mas", &song)
assert rc == 0
MT_Song loaded
memset(&loaded, 0, sizeof(loaded))
rc = mas_load("/tmp/test_multi.mas", &loaded)
assert rc == 0
assert loaded.patt_count == 3
assert loaded.patterns[0]->nrows == 64
assert loaded.patterns[1]->nrows == 128
assert loaded.patterns[1]->cells[127][0].note == 84
assert loaded.patterns[2]->nrows == 32
assert loaded.patterns[2]->cells[31][15].note == 36
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.4.4 `test_mas_roundtrip_order_table`

**Verifies:** The full order table (up to 200 entries) roundtrips correctly.

```
call song_init()
song.order_count = 12
for i in 0..11:
    song.orders[i] = i % song.patt_count
// Ensure referenced patterns exist
for i in 0..11:
    song_ensure_pattern(song.orders[i])
rc = mas_write("/tmp/test_orders.mas", &song)
assert rc == 0
MT_Song loaded
memset(&loaded, 0, sizeof(loaded))
rc = mas_load("/tmp/test_orders.mas", &loaded)
assert rc == 0
assert loaded.order_count == 12
for i in 0..11:
    assert loaded.orders[i] == song.orders[i]
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.4.5 `test_mas_roundtrip_flags`

**Verifies:** Boolean flags (`freq_linear`, `xm_mode`) roundtrip correctly
through the MAS header flags byte.

```
call song_init()
song.freq_linear = false
song.xm_mode     = false
rc = mas_write("/tmp/test_flags.mas", &song)
assert rc == 0
MT_Song loaded
memset(&loaded, 0, sizeof(loaded))
rc = mas_load("/tmp/test_flags.mas", &loaded)
assert rc == 0
assert loaded.freq_linear == false
assert loaded.xm_mode     == false
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

---

### 1.5 Pattern RLE Compression/Decompression Roundtrip Tests

These tests write patterns through `mas_write()` and load them back with
`mas_load()`, focusing on edge cases in the IT-style RLE compressor/
decompressor implemented in `write_pattern()` and `parse_pattern()`.

#### T1.5.1 `test_rle_all_empty_pattern`

**Verifies:** A pattern with every cell empty compresses and decompresses to
all-empty cells.

```
call song_init()
// Pattern 0 is already all-empty from song_init()
mas_write("/tmp/test_rle_empty.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_empty.mas", &loaded)
pat = loaded.patterns[0]
for r in 0..pat->nrows-1:
    for c in 0..MT_MAX_CHANNELS-1:
        assert pat->cells[r][c].note == NOTE_EMPTY
        assert pat->cells[r][c].inst == 0
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.5.2 `test_rle_value_cache_carry_forward`

**Verifies:** When the same note+instrument repeats across rows, the MF
carry-forward mechanism correctly re-emits the cached values on load.

```
call song_init()
pat = song.patterns[0]
// Same note on 16 consecutive rows in channel 0
for r in 0..15:
    pat->cells[r][0].note = 60
    pat->cells[r][0].inst = 1
mas_write("/tmp/test_rle_cache.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_cache.mas", &loaded)
lpat = loaded.patterns[0]
for r in 0..15:
    assert lpat->cells[r][0].note == 60
    assert lpat->cells[r][0].inst == 1
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.5.3 `test_rle_note_off_cut_no_cache`

**Verifies:** `NOTE_CUT` (254) and `NOTE_OFF` (255) do not get cached by
the compressor (the value cache is reset to 256 after these special notes,
forcing re-emit on next occurrence).

```
call song_init()
pat = song.patterns[0]
pat->cells[0][0].note = 60;  pat->cells[0][0].inst = 1
pat->cells[1][0].note = NOTE_CUT
pat->cells[2][0].note = NOTE_CUT   // must not be suppressed by cache
pat->cells[3][0].note = 60;  pat->cells[3][0].inst = 1
mas_write("/tmp/test_rle_noteoff.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_noteoff.mas", &loaded)
lpat = loaded.patterns[0]
assert lpat->cells[0][0].note == 60
assert lpat->cells[1][0].note == NOTE_CUT
assert lpat->cells[2][0].note == NOTE_CUT
assert lpat->cells[3][0].note == 60
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.5.4 `test_rle_multi_channel`

**Verifies:** Data across many channels (up to 32) compresses and
decompresses correctly with independent per-channel value caches.

```
call song_init()
pat = song.patterns[0]
// Put different data on each channel at row 0
for c in 0..31:
    pat->cells[0][c].note = c + 36
    pat->cells[0][c].inst = c + 1
mas_write("/tmp/test_rle_multichan.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_multichan.mas", &loaded)
lpat = loaded.patterns[0]
for c in 0..31:
    assert lpat->cells[0][c].note == c + 36
    assert lpat->cells[0][c].inst == c + 1
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.5.5 `test_rle_all_fields_present`

**Verifies:** A cell with all 5 fields populated (note + inst + vol + fx +
param) roundtrips correctly.

```
call song_init()
pat = song.patterns[0]
pat->cells[0][0] = { .note = 60, .inst = 5, .vol = 48, .fx = 15, .param = 0xFF }
mas_write("/tmp/test_rle_all.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_all.mas", &loaded)
lpat = loaded.patterns[0]
assert lpat->cells[0][0].note  == 60
assert lpat->cells[0][0].inst  == 5
assert lpat->cells[0][0].vol   == 48
assert lpat->cells[0][0].fx    == 15
assert lpat->cells[0][0].param == 0xFF
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

#### T1.5.6 `test_rle_pattern_break_cache_reset`

**Verifies:** A Cxx (effect 3) pattern break causes a cache reset on the
marked row of the target pattern. Uses `mark_patterns()` indirectly via the
write/load cycle.

```
call song_init()
song_ensure_pattern(1)
song.patt_count = 2
song.order_count = 2
song.orders[0] = 0
song.orders[1] = 1
pat0 = song.patterns[0]
pat1 = song.patterns[1]
// Row 0, ch 0 of pattern 0: note + Cxx break to row 8 of next pattern
pat0->cells[0][0].note = 60
pat0->cells[0][0].inst = 1
pat0->cells[0][0].fx   = 3    // effect C (pattern break in IT numbering)
pat0->cells[0][0].param = 8   // break to row 8
// Row 8, ch 0 of pattern 1: note that must NOT be cache-suppressed
pat1->cells[8][0].note = 72
pat1->cells[8][0].inst = 2
mas_write("/tmp/test_rle_break.mas", &song)
MT_Song loaded; memset(&loaded, 0, sizeof(loaded))
mas_load("/tmp/test_rle_break.mas", &loaded)
assert loaded.patterns[1]->cells[8][0].note == 72
assert loaded.patterns[1]->cells[8][0].inst == 2
// Cleanup
song_free()
for i in 0..MT_MAX_PATTERNS-1:
    if loaded.patterns[i]: free(loaded.patterns[i])
```

---

### 1.6 Envelope Encoding/Decoding Roundtrip Tests

These tests require the envelope encoding (in `mas_write.c`) and decoding
(in `mas_load.c`) to be fully implemented. Currently both are stubbed.
The tests are written against the `MT_Envelope` / `MT_EnvelopeNode` types
from data_model.md and the encoding formula from file_io.md section 3.3.

**Note:** Until `MT_Instrument` and envelope serialization are implemented
in the codebase, these tests serve as a specification and should be
implemented alongside the feature.

#### T1.6.1 `test_envelope_encode_decode_linear`

**Verifies:** A simple linear ramp (2 nodes: 0->64 over 64 ticks) encodes
to correct delta/base/range values and decodes back to the original nodes.

```
MT_Envelope env
env.node_count = 2
env.nodes[0] = { .x = 0,  .y = 0 }
env.nodes[1] = { .x = 64, .y = 64 }
// Encode to MAS format buffer
u8 buf[256]
int len = envelope_encode(&env, buf)
// Decode from MAS format buffer
MT_Envelope decoded
envelope_decode(buf, len, &decoded)
assert decoded.node_count == 2
assert decoded.nodes[0].x == 0
assert decoded.nodes[0].y == 0
assert decoded.nodes[1].x == 64
assert decoded.nodes[1].y == 64
```

#### T1.6.2 `test_envelope_encode_decode_sustain_loop`

**Verifies:** Envelope with sustain and loop markers roundtrips correctly.

```
MT_Envelope env
env.node_count = 4
env.nodes[0] = { .x = 0,  .y = 64 }
env.nodes[1] = { .x = 16, .y = 32 }
env.nodes[2] = { .x = 32, .y = 48 }
env.nodes[3] = { .x = 96, .y = 0 }
env.loop_start = 1
env.loop_end   = 2
env.sus_start  = 0
env.sus_end    = 0
env.enabled    = true
// Encode -> decode
u8 buf[256]
int len = envelope_encode(&env, buf)
MT_Envelope decoded
envelope_decode(buf, len, &decoded)
assert decoded.node_count == 4
assert decoded.loop_start == 1
assert decoded.loop_end   == 2
assert decoded.sus_start  == 0
assert decoded.sus_end    == 0
for i in 0..3:
    assert decoded.nodes[i].x == env.nodes[i].x
    assert decoded.nodes[i].y == env.nodes[i].y
```

#### T1.6.3 `test_envelope_encode_decode_single_node`

**Verifies:** An envelope with a single node (sustain at constant level)
produces delta=0, range=0 for the last node.

```
MT_Envelope env
env.node_count = 1
env.nodes[0] = { .x = 0, .y = 48 }
u8 buf[256]
int len = envelope_encode(&env, buf)
MT_Envelope decoded
envelope_decode(buf, len, &decoded)
assert decoded.node_count == 1
assert decoded.nodes[0].x == 0
assert decoded.nodes[0].y == 48
```

#### T1.6.4 `test_envelope_delta_overflow_clamping`

**Verifies:** When the delta exceeds s16 range (-32768..32767), it is
clamped, and the overflow correction loop in the encoder adjusts the delta
so that `base + (delta * range) >> 9` stays within 0..64.

```
MT_Envelope env
env.node_count = 2
env.nodes[0] = { .x = 0,   .y = 0 }
env.nodes[1] = { .x = 1,   .y = 64 }   // range=1, ideal delta = 64*512 = 32768 -> overflow
u8 buf[256]
int len = envelope_encode(&env, buf)
MT_Envelope decoded
envelope_decode(buf, len, &decoded)
// Value may not be exactly 64 due to clamping; verify it's close and within [0, 64]
assert decoded.nodes[1].y >= 60 && decoded.nodes[1].y <= 64
```

---

### 1.7 WAV Parser Tests

These tests write synthetic WAV files to disk (or use in-memory buffers
passed to an internal parse function) and verify that `wav_load()` correctly
parses and converts them.

#### T1.7.1 `test_wav_load_8bit_mono`

**Verifies:** 8-bit unsigned mono WAV is loaded, converted to signed, with
correct length and sample rate.

```
// Create a minimal 8-bit mono WAV: 256 samples at 8000 Hz
write_wav_file("/tmp/test_8m.wav", 8000, 8, 1, data_8bit_unsigned, 256)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_8m.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 8
assert rate == 8000
assert len  == 256    // out_len is in samples for 8-bit
// Verify conversion: unsigned 128 -> signed 0
assert (s8)pcm[128_index] == 0   // where input was 128
free(pcm)
```

#### T1.7.2 `test_wav_load_16bit_mono`

**Verifies:** 16-bit signed mono WAV is loaded with no conversion, correct
byte count.

```
write_wav_file("/tmp/test_16m.wav", 44100, 16, 1, data_16bit_signed, 1024)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_16m.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 16
assert rate == 44100
assert len  == 1024 * 2    // out_len is in bytes for 16-bit
free(pcm)
```

#### T1.7.3 `test_wav_load_16bit_stereo_mixdown`

**Verifies:** 16-bit stereo WAV is mixed down to mono using `(L+R)/2`.

```
// Create stereo file: L = +16384, R = -16384 for all frames
s16 stereo_data[512]  // 256 frames * 2 channels
for i in 0..255:
    stereo_data[i*2]   = +16384
    stereo_data[i*2+1] = -16384
write_wav_file("/tmp/test_16s.wav", 22050, 16, 2, stereo_data, 512)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_16s.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 16
assert len  == 256 * 2    // 256 mono samples * 2 bytes
s16 *samples = (s16 *)pcm
// Each sample should be (16384 + -16384) / 2 = 0
for i in 0..255:
    assert samples[i] == 0
free(pcm)
```

#### T1.7.4 `test_wav_load_24bit_to_16bit`

**Verifies:** 24-bit PCM is downsampled to 16-bit by taking the upper 16
bits (sign-extended).

```
// Create 24-bit mono WAV with a known sample: 0x7FFFFF (max positive)
u8 data24[3] = { 0xFF, 0xFF, 0x7F }  // little-endian 0x7FFFFF
write_wav_file("/tmp/test_24.wav", 44100, 24, 1, data24, 1)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_24.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 16
s16 *s = (s16 *)pcm
assert s[0] == 0x7FFF   // upper 16 bits of 0x7FFFFF
free(pcm)
```

#### T1.7.5 `test_wav_load_32bit_float`

**Verifies:** 32-bit IEEE 754 float WAV is converted to 16-bit signed with
clamping at +/-1.0.

```
// Create float WAV with values: 0.0, 0.5, -0.5, 1.0, -1.0, 1.5 (clamp)
float fdata[6] = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f }
write_wav_file_float("/tmp/test_f32.wav", 44100, fdata, 6)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_f32.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 16
s16 *s = (s16 *)pcm
assert s[0] == 0              // 0.0
assert s[1] == 16383 or 16384 // ~0.5 * 32767
assert s[3] == 32767          // 1.0
assert s[4] == -32767         // -1.0 (note: wav_load uses * 32767.0f)
assert s[5] == 32767          // 1.5 clamped to 1.0
free(pcm)
```

#### T1.7.6 `test_wav_load_8bit_stereo_mixdown`

**Verifies:** 8-bit stereo is converted unsigned-to-signed then mixed to
mono.

```
// Create 8-bit stereo: L = 200 (unsigned), R = 56 (unsigned)
// Signed: L = 72, R = -72.  Mono = 0
u8 stereo8[4] = { 200, 56, 200, 56 }  // 2 frames
write_wav_file("/tmp/test_8s.wav", 8000, 8, 2, stereo8, 4)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_8s.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert bits == 8
assert len  == 2    // 2 mono samples
s8 *s = (s8 *)pcm
assert s[0] == 0    // (72 + -72) / 2 = 0
free(pcm)
```

#### T1.7.7 `test_wav_load_errors`

**Verifies:** `wav_load()` returns correct error codes for invalid files.

```
// Non-existent file
rc = wav_load("/tmp/nonexistent.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_ERR_OPEN  // -1
// File with wrong magic
write_file("/tmp/notawav.wav", "NOT_RIFF_DATA")
rc = wav_load("/tmp/notawav.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_ERR_NOT_RIFF  // -2
// RIFF file but not WAVE
write_riff_non_wave("/tmp/nowave.wav")
rc = wav_load("/tmp/nowave.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_ERR_NOT_WAVE  // -3
```

#### T1.7.8 `test_wav_load_skips_unknown_chunks`

**Verifies:** The parser correctly skips unknown RIFF chunks (e.g., `LIST`,
`INFO`) and still finds the `fmt ` and `data` chunks.

```
// Create a WAV with an extra "LIST" chunk between fmt and data
write_wav_with_extra_chunks("/tmp/test_extra.wav", 44100, 16, 1, data, 256)
u8 *pcm; u32 len, rate; u8 bits
rc = wav_load("/tmp/test_extra.wav", &pcm, &len, &rate, &bits)
assert rc == WAV_OK
assert len == 256 * 2
free(pcm)
```

---

### 1.8 Sample Alignment and Loop Boundary Tests

These tests verify the alignment logic described in file_io.md section 3.4,
applied during MAS export. They require the sample serialization to be
implemented in `mas_write.c`.

**Note:** Like envelope tests, these are specification-level until
`MT_Sample` serialization is implemented.

#### T1.8.1 `test_sample_loop_start_word_aligned_8bit`

**Verifies:** For 8-bit samples, `loop_start` is padded to a 4-byte
(word) boundary.

```
MT_Sample samp
samp.format     = 0     // 8-bit
samp.length     = 100
samp.loop_start = 5     // not word-aligned
samp.loop_length = 50
aligned = sample_align_for_export(&samp)
assert aligned.loop_start % 4 == 0
assert aligned.loop_start >= 5
// Padding bytes are prepended (silence or copied from beginning)
```

#### T1.8.2 `test_sample_loop_start_word_aligned_16bit`

**Verifies:** For 16-bit samples, `loop_start` is padded to a 2-sample
(4-byte) boundary.

```
MT_Sample samp
samp.format     = 1     // 16-bit
samp.length     = 100
samp.loop_start = 3     // 3 * 2 bytes = 6, not word-aligned
samp.loop_length = 50
aligned = sample_align_for_export(&samp)
assert (aligned.loop_start * 2) % 4 == 0
```

#### T1.8.3 `test_sample_loop_length_word_aligned`

**Verifies:** `loop_length` is rounded up to a word boundary.

```
MT_Sample samp
samp.format      = 0
samp.length      = 100
samp.loop_start  = 0
samp.loop_length = 7   // not word-aligned for 8-bit
aligned = sample_align_for_export(&samp)
assert aligned.loop_length % 4 == 0
assert aligned.loop_length >= 7
```

#### T1.8.4 `test_sample_total_length_word_aligned`

**Verifies:** Total sample length (in bytes) is padded to 4-byte boundary.

```
MT_Sample samp
samp.format     = 0
samp.length     = 5   // 5 bytes, not word-aligned
samp.loop_start = 0
samp.loop_length = 0  // no loop
aligned = sample_align_for_export(&samp)
total_bytes = aligned.length * (samp.format == 1 ? 2 : 1)
assert total_bytes % 4 == 0
```

#### T1.8.5 `test_sample_wraparound_padding`

**Verifies:** 4 bytes of wraparound padding are appended after the sample
data (copied from loop start if looping, or zeros if not).

```
// Looping sample: wraparound should contain first 4 bytes of loop
MT_Sample samp
samp.format      = 0
samp.length      = 64
samp.loop_start  = 8
samp.loop_length = 32
samp.pcm_data    = <fill with known values>
buf = serialize_sample_data(&samp)
// Last 4 bytes should equal pcm_data[loop_start..loop_start+3]
assert buf[samp.length + 0] == samp.pcm_data[samp.loop_start + 0]
assert buf[samp.length + 1] == samp.pcm_data[samp.loop_start + 1]
assert buf[samp.length + 2] == samp.pcm_data[samp.loop_start + 2]
assert buf[samp.length + 3] == samp.pcm_data[samp.loop_start + 3]
// Non-looping sample: wraparound should be zeros
samp.loop_length = 0
buf = serialize_sample_data(&samp)
assert buf[samp.length + 0] == 0
assert buf[samp.length + 1] == 0
assert buf[samp.length + 2] == 0
assert buf[samp.length + 3] == 0
```

---

## Section 2: Integration / Manual Test Procedures for no$gba

All procedures assume:
- no$gba (debugging/developer version) is installed and configured
- The maxtracker `.nds` ROM has been built with `make` and is loadable
- The emulated SD card image (or DLDI-patched FAT image) is available with
  the `sd:/maxtracker/` directory structure
- Breakpoints and memory watches are set via no$gba's debugger pane

---

### 2.1 Pattern Navigation

#### P2.1.1 Overview Mode Cursor Movement

**Preconditions:** maxtracker is running on the Pattern screen (default after
boot). A song is loaded with at least 2 patterns and 8+ channels.

**Steps:**

1. Verify the pattern grid is in overview mode (`cursor.inside == false`).
   The status bar (row 0) should show `[1-8]` for the channel group.
2. Press D-pad DOWN 4 times.
3. Press D-pad RIGHT 3 times.
4. Press D-pad UP 2 times.
5. Press X + D-pad DOWN (page down: 16 rows).
6. Press X + D-pad UP (page up: 16 rows).
7. Press L to switch channel group to [9-16] (if channel_count > 8).
8. Press R to switch back to [1-8].

**Expected Results:**

- Step 2: Cursor row advances by `cursor.step` (default 1) per press.
  The cursor row indicator (`>`) moves down in the row number column.
- Step 3: Cursor moves to channels 1, 2, 3 (0-indexed) within the visible
  group. The column highlight shifts right.
- Step 4: Cursor row decreases. Does not go below row 0.
- Step 5: Cursor row jumps by 16. If the cursor would go past `nrows-1`,
  it clamps to the last row.
- Step 6: Cursor row jumps back up by 16.
- Step 7: The visible channel group label changes to `[9-16]` and the
  channel headers update. `cursor.ch_group` is now 8.
- Step 8: Returns to `[1-8]`, `cursor.ch_group` is 0.

**no$gba Debugger Checks:**

- Set a watch on `cursor.row` at the address of the `EditorCursor cursor`
  global (defined in `arm9/source/ui/pattern_view.c`). Verify it tracks the
  displayed row number.
- Set a watch on `cursor.channel`. Verify it updates on L/R and LEFT/RIGHT.
- Set a watch on `cursor.ch_group`. Verify it changes in steps of 8 on L/R.

---

#### P2.1.2 Overview to Inside Mode Switching

**Preconditions:** Pattern screen in overview mode, cursor at row 4 channel 2.

**Steps:**

1. Press A to enter inside mode.
2. Verify the display switches to single-channel view showing all 5 columns
   (Note, Ins, Vol, Eff, Prm) for channel 2.
3. Press D-pad LEFT/RIGHT to move between sub-columns.
4. Press B to exit back to overview mode.
5. Verify the cursor position (row 4, channel 2) is preserved.

**Expected Results:**

- Step 1: `cursor.inside` becomes `true`. The top screen redraws to show
  the inside-mode layout (as per screen_layout.md section 3.2). The status
  bar shows `CH:02`.
- Step 3: `cursor.column` cycles through 0 (Note), 1 (Ins), 2 (Vol),
  3 (Eff), 4 (Prm). The cursor highlight moves between sub-columns.
- Step 4: `cursor.inside` becomes `false`. Overview mode is restored.
- Step 5: `cursor.row == 4` and `cursor.channel == 2`.

**no$gba Debugger Checks:**

- Watch `cursor.inside` -- should toggle between 0 and 1.
- Watch `cursor.column` -- should cycle 0-4 in inside mode.

---

### 2.2 Note Entry and Editing

#### P2.2.1 Note Entry in Inside Mode

**Preconditions:** Pattern screen, inside mode on channel 0, cursor at
row 0 on the Note column (`cursor.column == 0`). Octave = 4, instrument = 1.

**Steps:**

1. Press A to stamp a note.
2. Observe the cell at `[0][0]`.
3. Press Y + D-pad UP to change octave to 5.
4. Move cursor to row 1 (D-pad DOWN).
5. Press A to stamp another note.
6. Move cursor to row 2 and press START to enter note-off (^^^).
7. Move cursor to row 3 and press Y + A to enter note-cut (===).
8. Move cursor back to row 0. Press B to clear the cell.

**Expected Results:**

- Step 2: `cells[0][0].note == cursor.octave * 12 + cursor.semitone`
  (= 48 for octave 4, semitone C). `cells[0][0].inst == 1`.
- Step 5: `cells[1][0].note == 60` (octave 5, C). `cells[1][0].inst == 1`.
- Step 6: `cells[2][0].note == NOTE_OFF` (255).
- Step 7: `cells[3][0].note == NOTE_CUT` (254).
- Step 8: `cells[0][0].note == NOTE_EMPTY` (250), `cells[0][0].inst == 0`.

**no$gba Debugger Checks:**

- Set a memory breakpoint on `song.patterns[0]->cells[0][0]` (5 bytes).
  After each A press, verify the correct note value is written.
- After step 8, verify the cell is fully zeroed (note=250, inst/vol/fx/param=0).

---

#### P2.2.2 Hex Editing (Instrument, Volume, Effect, Parameter Columns)

**Preconditions:** Inside mode, channel 0, row 0 already has a note. Cursor
on the Instrument column (`cursor.column == 1`).

**Steps:**

1. Press D-pad UP to increment the high nibble of the instrument value.
2. Press D-pad DOWN to decrement the high nibble.
3. Press D-pad RIGHT to increment the low nibble.
4. Press D-pad LEFT to decrement the low nibble.
5. Move cursor to Volume column (column 2) and repeat steps 1-4.
6. Move cursor to Effect column (column 3) and set a value.
7. Move cursor to Parameter column (column 4) and set a value.

**Expected Results:**

- Step 1: Instrument value changes from `01` to `11` (high nibble +1).
- Step 2: Back to `01`.
- Step 3: Instrument value changes to `02` (low nibble +1).
- Step 4: Back to `01`.
- Steps 5-7: Volume, effect, and parameter fields update with hex values
  in the range 0x00-0xFF.

**no$gba Debugger Checks:**

- Watch the specific cell's `inst`, `vol`, `fx`, `param` bytes in memory.
  Verify each nibble change is correctly reflected.

---

### 2.3 Song / Order Table Editing

#### P2.3.1 Order Table Navigation and Editing

**Preconditions:** Press SHIFT + D-pad DOWN to switch to the Song screen
(`current_screen == SCREEN_SONG`). The song has `order_count == 4` with
orders `{0, 1, 0, 1}`.

**Steps:**

1. Press D-pad DOWN to navigate to order position 1.
2. Press D-pad RIGHT to increment the pattern number at this position.
3. Press D-pad LEFT to decrement it back.
4. Navigate to position 2. Press Y + A to insert a new order entry.
5. Press Y + B to delete the entry just inserted.
6. Press A to jump to the pattern editor for the selected pattern.

**Expected Results:**

- Step 2: `song.orders[1]` changes from 1 to 2 (incremented).
  The display updates to show `02` at position 1.
- Step 3: `song.orders[1]` back to 1.
- Step 4: `song.order_count` increases by 1 (now 5). A new entry is
  inserted at position 2 (duplicate of current value). Entries below shift
  down.
- Step 5: `song.order_count` decreases by 1 (back to 4). The entry is
  removed and entries below shift up.
- Step 6: Screen switches to Pattern screen (`current_screen == SCREEN_PATTERN`).
  `cursor.order_pos` is set to the selected order position.

**no$gba Debugger Checks:**

- Watch `song.order_count` and `song.orders[]` array in memory.
- After insert: verify the array contents shifted correctly.
- After delete: verify the array is compacted.

---

### 2.4 File Save/Load Roundtrip

#### P2.4.1 Save and Reload a Song

**Preconditions:** A song with known data: at least 2 patterns with notes,
specific tempo (e.g., 140), speed (e.g., 4), and order table.

**Steps:**

1. Press SHIFT + START to switch to the Disk screen (`SCREEN_DISK`).
2. Navigate to `sd:/maxtracker/songs/`.
3. Press X to save the current song. Enter filename `test_save.mas`.
4. Confirm the save dialog.
5. Verify the status message shows "Saved: test_save.mas (XX KB)".
6. Modify the song: change tempo to 180, add notes to pattern 0.
7. Return to Disk screen. Navigate to `test_save.mas`. Press A to load.
8. Confirm the load dialog.
9. Verify all fields match the state from before step 6.

**Expected Results:**

- Step 5: File is written to the emulated SD card. The return code from
  `mas_write()` is 0.
- Step 9: After loading, the song model matches the saved state:
  - `song.initial_tempo == 140` (not 180)
  - `song.initial_speed == 4`
  - Pattern data matches what was saved
  - Order table is restored

**no$gba Debugger Checks:**

- Set a breakpoint on `mas_write()` entry. Verify `path` argument is
  correct.
- After `mas_write()` returns, verify return value == 0 (register r0).
- After `mas_load()` returns, inspect `song.initial_tempo`,
  `song.initial_speed`, `song.orders[]` in memory.
- Compare pattern cell data at key positions against expected values.

---

#### P2.4.2 Load a Known .mas File

**Preconditions:** A .mas file generated by mmutil (from a known XM/IT
source) is placed on the emulated SD card.

**Steps:**

1. Navigate to the file in the Disk screen and load it.
2. Switch to the Pattern screen. Verify patterns have data.
3. Switch to the Song screen. Verify order table length and entries.
4. Check `song.channel_count` via debugger -- should be auto-detected.

**Expected Results:**

- Patterns contain notes and effects from the original module.
- Order table matches the source XM/IT structure.
- Channel count is rounded up to the nearest standard size (4/8/16/24/32).

**no$gba Debugger Checks:**

- Watch `song.patt_count`, `song.order_count`, `song.channel_count`.
- Inspect pattern 0 cells to verify they contain non-empty data.
- Compare against the expected data from the source module.

---

### 2.5 Playback

#### P2.5.1 Play and Stop

**Preconditions:** A song with at least 1 pattern containing notes and a
valid instrument/sample pair. Playback subsystem initialized.

**Steps:**

1. On the Pattern screen, press START to begin playback.
2. Observe the playing row indicator (green highlight) advancing down.
3. Wait until the pattern loops (or advances to the next order entry).
4. Press START again to stop playback.

**Expected Results:**

- Step 1: `playback_is_playing()` returns true. The ARM7 receives
  `MT_CMD_PLAY` via FIFO. `mt_shared->playing` becomes 1.
- Step 2: The play row highlight advances at the rate determined by
  `song.initial_tempo` and `song.initial_speed`. If tempo=125 and speed=6,
  rows advance at 125/(6*2.5) = ~8.3 rows/sec.
- Step 3: On pattern end, ARM7 sends `MT_CMD_PATTERN_END`. ARM9 updates
  `mt_shared->cells` to point to the next pattern.
- Step 4: `playback_is_playing()` returns false. ARM7 stops the sequencer.
  `mt_shared->playing` becomes 0.

**no$gba Debugger Checks:**

- Watch `mt_shared->playing`, `mt_shared->row`, `mt_shared->position`.
- Set a breakpoint on the FIFO handler in `arm7/source/main.c` to verify
  `MT_CMD_PLAY` and `MT_CMD_STOP` commands are received.
- Monitor the ARM7 `mmPulse` call frequency (should be called at the
  hardware timer rate).

---

#### P2.5.2 Follow Mode

**Preconditions:** Song loaded, pattern screen visible.

**Steps:**

1. Press L + R simultaneously to enable follow mode.
2. Press START to begin playback.
3. Observe the cursor row tracking the playback row.
4. Manually press D-pad DOWN to move the cursor ahead.
5. Verify the cursor re-syncs to the playback position on the next row.
6. Press L + R to disable follow mode.
7. Verify the cursor no longer tracks playback.

**Expected Results:**

- Step 1: `cursor.follow` becomes `true`.
- Step 3: `cursor.row` matches `cursor.play_row` each frame. The view
  scrolls to keep the playing row centered.
- Step 5: After one row advance, `cursor.row` snaps back to `cursor.play_row`.
- Step 6: `cursor.follow` becomes `false`.
- Step 7: Cursor remains stationary while the play indicator continues to
  advance.

**no$gba Debugger Checks:**

- Watch `cursor.follow`, `cursor.row`, `cursor.play_row`.
- Verify `cursor.play_row` is updated from `mt_shared->row` in the
  `playback_update()` function.

---

### 2.6 Instrument Parameter Editing

#### P2.6.1 Edit Instrument Parameters

**Preconditions:** Press SHIFT + D-pad UP to switch to the Instrument screen
(`SCREEN_INSTRUMENT`). Instrument 1 is active.

**Steps:**

1. Verify the parameter list shows instrument 1's current values on the
   top screen (Global Volume, Fadeout, Panning, NNA, etc.).
2. Use D-pad UP/DOWN to highlight "Global Volume".
3. Press A to enter edit mode. Use D-pad LEFT/RIGHT to change the value.
4. Press A to confirm the change.
5. Navigate to "Panning" and change it.
6. Press L to switch to instrument 2. Verify the display updates.
7. Press R to switch back to instrument 1. Verify the previously changed
   values are retained.

**Expected Results:**

- Step 3: The value updates in real-time as D-pad is pressed.
- Step 7: Instrument 1 retains the modified Global Volume and Panning.

**no$gba Debugger Checks:**

- If `MT_Instrument` is implemented in `song.h`, watch
  `song.instruments[0].global_volume` and `song.instruments[0].panning`.
- Verify values change when the user modifies them.

---

### 2.7 Sample Loading (WAV from SD)

#### P2.7.1 Load a WAV Sample

**Preconditions:** A valid WAV file (`kick.wav`, 16-bit mono, 44100 Hz)
exists at `sd:/maxtracker/samples/kick.wav`. Press SHIFT + D-pad RIGHT
to switch to the Sample screen (`SCREEN_SAMPLE`).

**Steps:**

1. Navigate to the file browser (or use the load function within the
   Sample screen).
2. Select `kick.wav` and press A to load.
3. Verify the waveform is displayed on the top screen.
4. Check the sample info display: format, length, sample rate.

**Expected Results:**

- Step 2: `wav_load()` returns `WAV_OK`. The sample data is allocated in
  the sample pool. The status bar shows the sample name and size.
- Step 3: The waveform view shows the loaded audio data.
- Step 4: Display shows `16-bit`, correct length in samples, and `44100 Hz`.

**no$gba Debugger Checks:**

- Set a breakpoint on `wav_load()`. Verify the path argument is correct.
- After `wav_load()` returns, check `out_bits == 16`, `out_rate == 44100`,
  `out_len > 0`.
- Verify the sample pool usage increased by `out_len` bytes.
- Inspect the first few bytes of the allocated PCM data to verify they
  are non-zero (assuming the WAV is not silence).

---

### 2.8 Screen Switching

#### P2.8.1 All Six Screen Modes

**Preconditions:** maxtracker is running on the default Pattern screen.

**Steps:**

1. Press SHIFT + D-pad UP -- switch to Instrument screen.
2. Press SHIFT + D-pad DOWN -- switch to Song screen.
3. Press SHIFT + D-pad LEFT -- switch to Mixer screen.
4. Press SHIFT + D-pad RIGHT -- switch to Sample screen.
5. Press SHIFT + START -- switch to Disk screen.
6. Press B -- return to Pattern screen (from any non-Pattern screen).
7. Repeat steps 1-6 rapidly (stress test for screen transitions).

**Expected Results:**

| Step | Expected `current_screen` value |
|------|---------------------------------|
| 1 | `SCREEN_INSTRUMENT` (1) |
| 2 | `SCREEN_SONG` (3) |
| 3 | `SCREEN_MIXER` (4) |
| 4 | `SCREEN_SAMPLE` (2) |
| 5 | `SCREEN_DISK` (5) |
| 6 | `SCREEN_PATTERN` (0) |

- No visual glitches during transitions. Both screens update.
- No crash during rapid switching (step 7).

**no$gba Debugger Checks:**

- Watch `current_screen` (defined in `arm9/source/ui/screen.c`). Verify it
  matches the `ScreenMode` enum values after each switch.
- Monitor VRAM writes during transitions to ensure framebuffers are being
  updated (BG2 bitmap at `VRAM_A` for top screen, `VRAM_C` for bottom).
- Check for any memory access violations during rapid switching.

---

### 2.9 Memory Leak Detection

#### P2.9.1 Repeated Pattern Alloc/Free

**Preconditions:** Fresh song loaded. no$gba heap inspection available.

**Steps:**

1. Record the current heap state (free memory). In no$gba, use the memory
   viewer to note the heap metadata or set a watch on the heap pointer.
2. Perform 100 iterations of:
   a. `song_ensure_pattern(10)` -- allocate pattern 10.
   b. Write some data to the pattern.
   c. `free(song.patterns[10]); song.patterns[10] = NULL;` -- free it.
3. Record the heap state again.
4. Compare free memory before and after.

**Expected Results:**

- The free memory should be the same before and after the loop (within
  the tolerance of heap fragmentation metadata).
- No growing memory consumption over the iterations.

**no$gba Debugger Checks:**

- Use no$gba's ARM9 memory map view. Before the loop, note the value
  of the heap end pointer (typically at the top of the 4MB main RAM).
- After the loop, verify the heap end pointer has not advanced.
- Alternatively, track `malloc` and `free` calls by setting breakpoints
  on both functions. For each iteration, verify that each `malloc` is
  paired with exactly one `free` of the same pointer.

---

#### P2.9.2 Repeated Clipboard Copy/Free

**Preconditions:** A pattern with data is loaded.

**Steps:**

1. Record initial heap state.
2. Perform 200 iterations of:
   a. `clipboard_copy(pat, 0, 15, 0, 7)` -- copy a 16x8 block.
   b. `clipboard_free()` -- release clipboard.
3. Record final heap state.
4. Verify no leak.

**Expected Results:**

- `clipboard.data` is NULL after each `clipboard_free()`.
- Heap usage does not grow across iterations.

**no$gba Debugger Checks:**

- Breakpoint on `malloc` inside `clipboard_copy()`. Note the returned
  pointer.
- Breakpoint on `free` inside `clipboard_free()`. Verify the same pointer
  is freed.
- After 200 iterations, verify total `malloc` calls == total `free` calls.

---

#### P2.9.3 Repeated Undo Push/Pop

**Preconditions:** A pattern is allocated. Undo system initialized.

**Steps:**

1. Record initial heap state.
2. Push 128 undo entries (double the ring buffer size).
3. Pop all available entries (should be 64).
4. Repeat steps 2-3 five times.
5. Call `undo_free()`.
6. Record final heap state.

**Expected Results:**

- After `undo_free()`, all undo entry `old_data` pointers are freed.
- Heap usage returns to the initial level.

**no$gba Debugger Checks:**

- Watch the `ring[]` array's `old_data` pointers in memory. After
  `undo_free()`, all 64 entries should have `old_data == NULL`.
- Track `malloc`/`free` call counts. After full cleanup, the counts
  should be equal.

---

#### P2.9.4 Song New/Free Cycle

**Preconditions:** maxtracker is running.

**Steps:**

1. Record heap state.
2. Perform 20 iterations of:
   a. `song_init()` -- creates a new song, allocates pattern 0.
   b. Allocate patterns 1-9 via `song_ensure_pattern()`.
   c. `song_free()` -- free all patterns.
3. Record heap state.

**Expected Results:**

- Heap returns to initial state after `song_free()` on each iteration.
- No net growth after 20 cycles.

**no$gba Debugger Checks:**

- After each `song_free()`, verify all 256 `song.patterns[]` entries
  are NULL.
- Monitor total `malloc`/`free` counts: they should be balanced after
  each `song_free()`.

---

### 2.10 ARM7/ARM9 IPC Verification

#### P2.10.1 Shared State Communication

**Preconditions:** Playback system initialized. `mt_shared` is allocated.

**Steps:**

1. In no$gba debugger, locate `mt_shared` pointer (defined in
   `arm9/source/core/playback.c` or `arm9/source/core/playback.h`).
2. Verify `mt_shared->active == 1` (maxtracker mode enabled).
3. Start playback. Observe `mt_shared->playing` changes to 1.
4. Watch `mt_shared->row` incrementing as playback advances.
5. Stop playback. Verify `mt_shared->playing` returns to 0.

**Expected Results:**

- The `MT_SharedPatternState` struct in main RAM is accessible by both CPUs.
- ARM7 writes to `playing`, `row`, `position`, `tick` fields.
- ARM9 reads these fields in `playback_update()`.

**no$gba Debugger Checks:**

- Set a read watchpoint on `mt_shared->row` from ARM7 context. Verify
  ARM7 is writing to it.
- Set a read watchpoint on `mt_shared->row` from ARM9 context. Verify
  ARM9 is reading it in `playback_update()`.
- Verify `DC_FlushRange()` is called by ARM9 after modifying
  `mt_shared->cells` or `mt_shared->nrows`.

---

#### P2.10.2 FIFO Command Delivery

**Preconditions:** Both ARM7 and ARM9 FIFO handlers are initialized.

**Steps:**

1. Set a breakpoint in ARM7's FIFO handler (in `arm7/source/main.c`)
   on the switch/case for `MT_CMD_PLAY`.
2. On ARM9, trigger `playback_play(0)`.
3. Verify the breakpoint hits on ARM7 with the correct command type and
   parameter.
4. Repeat for `MT_CMD_STOP`, `MT_CMD_PREVIEW_NOTE`, `MT_CMD_SET_MUTE`.

**Expected Results:**

- Each command arrives at ARM7's FIFO handler with `MT_CMD_TYPE(val)`
  matching the sent command and `MT_CMD_PARAM(val)` matching the parameter.
- No commands are lost or duplicated.

**no$gba Debugger Checks:**

- Step through the ARM7 FIFO callback. Verify the received 32-bit value
  decodes correctly via `MT_CMD_TYPE()` and `MT_CMD_PARAM()`.
- Check that `fifoSendValue32(FIFO_MT, ...)` on ARM9 side uses the correct
  channel `FIFO_MT` (= `FIFO_USER_08`).
