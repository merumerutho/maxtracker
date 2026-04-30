# maxtracker -- File I/O

Parent: [DESIGN.md](../DESIGN.md)

---

## 1. Overview

maxtracker uses the SD card (via libfat/DLDI) as its primary storage medium. All file I/O uses standard POSIX functions (`fopen`, `fread`, `fwrite`, `fseek`, `opendir`, `readdir`).

File types handled:

| Extension | Direction | Description |
|-----------|-----------|-------------|
| `.mas` | Read + Write | Native format. Load existing maxmod songs; save/export completed songs. |
| `.wav` | Read | Load samples from WAV files. |
| `.raw` | Read + Write | Raw signed 8-bit PCM for drawn waveforms. |

---

## 2. Directory Structure on SD Card

```
sd:/
└── maxtracker/
    ├── songs/          # .mas files (user songs)
    ├── samples/        # .wav and .raw files
    └── backup/         # auto-save backups
```

On first run, maxtracker creates this directory structure if it doesn't exist.

---

## 3. MAS Export (Save)

Serializes the in-memory `MT_Song` to a standard `.mas` binary file. The output is fully compatible with stock maxmod -- any DS homebrew can play it via `mmPlayMAS()`.

### 3.1 Serialization Pipeline

```
MT_Song (editing format)
    │
    ├─ Header: tempo, speed, flags, orders, channel config
    │   -> Write mm_mas_prefix (8 bytes)
    │   -> Write mm_mas_head (276 bytes)
    │
    ├─ Reserve space for offset tables (inst + samp + patt) * 4 bytes
    │
    ├─ Instruments: for each MT_Instrument
    │   -> Align to 4 bytes
    │   -> Record offset
    │   -> Write fixed fields (12 bytes)
    │   -> Write envelopes (encode delta/base/range from absolute nodes)
    │   -> Write note map (if full map, else use 0x8000|sample shorthand)
    │
    ├─ Samples: for each MT_Sample
    │   -> Align to 4 bytes
    │   -> Record offset
    │   -> Write sample info (12 bytes)
    │   -> Write NDS sample header (16 bytes)
    │   -> Write PCM data (aligned, padded, with 4-byte wraparound)
    │
    ├─ Patterns: for each pattern in use
    │   -> Record offset
    │   -> Write row count byte (nrows - 1)
    │   -> RLE-compress pattern data (IT-style, with value caching + MF flags)
    │
    ├─ Align to 4 bytes at end
    │
    └─ Seek back and fill offset tables + prefix size field
```

### 3.2 Pattern Compression (RLE Encoder)

The encoder is the inverse of the decompressor documented in `mas_spec/patterns.md`. It must:

1. Track per-channel last values (mask, note, inst, vol, fx, param).
2. Mark row 0 and pattern-break targets for cache reset.
3. For each non-empty cell: compute the mask byte (COMPR_FLAGs for present fields + MF flags in upper nibble).
4. Suppress fields that match the cached value (clear COMPR_FLAG, keep MF flag).
5. Suppress the mask byte if it matches the last mask for this channel.
6. Write channel byte (with bit 7 set if new mask), optional mask, optional fields.
7. Write 0x00 at end of each row.

This logic is ported from mmutil's `Write_Pattern()` in `source/mas.c` (lines 371-511). The reference implementation and its detailed documentation are in `mas_spec/patterns.md`.

### 3.3 Envelope Encoding

Convert absolute `(tick, value)` nodes to MAS delta/base/range format. Ported from mmutil's `Write_Instrument_Envelope()`:

```
For each node i:
    base = node_y[i]
    if not last node:
        range = clamp(node_x[i+1] - node_x[i], 1, 511)
        delta = clamp(((node_y[i+1] - base) * 512 + range/2) / range, -32768, 32767)
        // Overflow correction
        while (base + (delta * range) >> 9) > 64: delta--
        while (base + (delta * range) >> 9) < 0:  delta++
    else:
        range = 0, delta = 0
    write_s16(delta)
    write_u16(base | (range << 7))
```

### 3.4 Sample Alignment

Before writing sample data to MAS, apply the same alignment rules as mmutil (documented in `mas_spec/samples.md` section 2.4):

- **Loop start**: pad beginning to align loop_start to word boundary (8-bit: mod 4, 16-bit: mod 2).
- **Loop length**: must be word-aligned. If not, unroll loop or resample.
- **Total length**: pad end to word boundary.
- **Sign conversion**: samples in MT_Sample are already signed (NDS native). No conversion needed.
- **Wraparound padding**: append 4 bytes after sample data (copy from loop start, or zeros).

### 3.5 Export Buffer

MAS serialization uses a temporary buffer (up to 256KB) in the sample pool's headroom or a separate malloc. The buffer is written to, then flushed to disk in one `fwrite` call. This avoids many small writes which are slow on FAT/SD.

For songs with large sample data (>256KB), the serializer writes in chunks: header + instruments first, then each sample's PCM data streamed directly to disk.

---

## 4. MAS Import (Load)

Loads an existing `.mas` file into the `MT_Song` editing model. This reuses the parsing logic developed for mas2xm.

### 4.1 Import Pipeline

```
.mas file on SD card
    │
    ├─ fopen + fread into temporary buffer
    │
    ├─ Parse prefix: verify type = MAS_TYPE_SONG, version = 0x18
    │
    ├─ Parse header: tempo, speed, flags, orders, channel config
    │   -> Populate MT_Song fields
    │
    ├─ Parse offset tables
    │
    ├─ For each instrument:
    │   -> Read fixed fields
    │   -> Decode envelopes (delta/base/range -> absolute nodes)
    │   -> Read note map (full or shorthand)
    │   -> Populate MT_Instrument
    │
    ├─ For each sample:
    │   -> Read sample info
    │   -> Read NDS sample header
    │   -> Allocate from sample pool
    │   -> Copy PCM data (strip alignment padding)
    │   -> Populate MT_Sample
    │
    ├─ For each pattern:
    │   -> Allocate from pattern pool
    │   -> Decompress RLE pattern data into flat MT_Cell array
    │      (using MF flag carry-forward logic -- critical!)
    │   -> Populate MT_Pattern
    │
    └─ Free temporary buffer
```

### 4.2 Pattern Decompression

Uses the corrected decompression algorithm with MF flag carry-forward, as documented in `mas_spec/patterns.md` section 5 and implemented in `mas2xm/src/mas_read.c`. This is the same logic that was validated against 46 XM files in the batch roundtrip tests.

### 4.3 Channel Count Detection

MAS files always have 32 channels in the header, but most songs use fewer. On import, scan all patterns to find the highest channel with any non-empty data, then set `song.channel_count` to the next standard size (4, 8, 16, 24, or 32).

### 4.4 MSL Soundbank Import (Future)

Version 1 only supports standalone .mas files (all samples embedded). MSL soundbank import (where samples are external) would require loading the soundbank file too. This is deferred.

---

## 5. WAV Loading

Load standard WAV files from SD card as samples.

### 5.1 Supported Formats

| Format | Support |
|--------|---------|
| 8-bit unsigned PCM | Yes (convert to signed) |
| 16-bit signed PCM | Yes |
| 24-bit PCM | Convert to 16-bit |
| 32-bit float | Convert to 16-bit |
| Mono | Yes |
| Stereo | Mix to mono (L+R)/2 |
| Any sample rate | Yes (stored as base_freq) |

### 5.2 WAV Parsing

The actual API is in `arm9/source/io/wav_load.{h,c}`:

```c
int wav_load(const char *path, u8 **out_data, u32 *out_len,
             u32 *out_rate, u8 *out_bits);
```

Minimal RIFF parser. Skips unknown chunks. Output is always 8-bit signed
or 16-bit signed mono PCM in a freshly malloc'd buffer the caller owns.
24-bit PCM and 32-bit float are quantized to 16-bit during parse.
Stereo is mixed to mono `(L + R) / 2` after format conversion.

Round-trip coverage for all supported bit depths and the canonical
sample rates (8 / 16 / 32 / 44.1 / 48 kHz) lives in `test/wav_test.c`.
A WAV writer counterpart for the SAMPLE view's `>> Save .wav` action is
at `arm9/source/io/wav_save.{h,c}`:

```c
int wav_save_mono16(const char *path, const s16 *pcm,
                    u32 num_samples, u32 sample_rate);
```

8-bit samples are upconverted to 16-bit (`<< 8`) on the fly before the
write; see `sv_do_save()` in `sample_view.c`.

### 5.3 Memory & lifecycle

WAV files can be large. The loader allocates the destination buffer
itself; the caller is responsible for `free()` (or for handing it off
to the song model, which takes ownership of `MT_Sample.pcm_data`).
Before any free of an existing `pcm_data` pointer, the disk-screen
`.wav` branch first stops playback and closes the LFE editor, since both
modules can hold pointers into the old buffer, and freeing without
detaching them was the source of a use-after-free fixed in 2026-04.

The `lib/lfe/` audio output path can also issue `wav_save_mono16`
calls for sample-export scenarios.

For large WAV files on DS (>256KB), loading is done in streaming fashion -- read a chunk, convert, write to pool, repeat. This avoids needing a full temporary copy in RAM.

---

## 6. Sample Drawing Save/Load

Drawn waveforms (created on the touchscreen) can be saved to SD card for reuse.

### 6.1 Format: Raw Signed 8-bit PCM

```
File: sd:/maxtracker/samples/mywave.raw
Contents: N bytes of signed 8-bit PCM data (-128 to +127)
No header. Length = file size.
```

This is the simplest possible format. The sample rate is assumed to be 8363 Hz (middle-C standard) unless the user changes it.

### 6.2 Save

```c
int mt_raw_save(const char *path, const MT_Sample *sample);
```

Writes `sample->length` bytes of 8-bit PCM data. If the sample is 16-bit, it's downsampled to 8-bit for the raw file.

### 6.3 Load

```c
int mt_raw_load(const char *path, MT_Sample *sample, MT_SamplePool *pool);
```

Reads the entire file as signed 8-bit PCM. Sets `base_freq = 8363`, `default_volume = 64`, `panning = 128`.

---

## 7. Auto-Save

maxtracker periodically auto-saves the current song to `sd:/maxtracker/backup/autosave.mas`. The interval is configurable (default: every 5 minutes of editing activity, not wall-clock time).

Auto-save only triggers if:
- The song has been modified since last save/auto-save.
- The user is not currently in the middle of an edit operation.
- The SD card is writable.

The auto-save file is overwritten each time. On startup, if `autosave.mas` exists and is newer than the last manually saved file, the user is prompted to recover it.

---

## 8. File Browser

### 8.1 Navigation

Standard directory browser using POSIX `opendir`/`readdir`. Sorted alphabetically with directories first. D-pad for scrolling, A to enter directory or load file, B to go up one level. **B at the configured root** is clamped: pressing B in an empty root no longer walks above it (an earlier bug let the user lock themselves out of the SHIFT+DOWN exit).

### 8.2 File Type Detection

By extension: `.mas` files are offered for full song load; `.wav` and `.raw` files are offered for sample import.

The destination sample slot is chosen by the disk-screen `.wav` branch in `main.c` per the routing globals in section 8.4: either the slot the SAMPLE view was on, or (legacy fallback) `cursor.instrument - 1`.

### 8.3 Path Management

Current directory is tracked as a string. Maximum path depth: 8 levels. Maximum filename display: 28 characters (truncated with `...` if longer). The browser caps at `FB_MAX_ENTRIES` (64) per directory; excess entries are silently dropped (known limitation, not surfaced to the user yet).

### 8.4 Disk-screen routing globals

The disk screen is no longer reachable by a global SHIFT+START shortcut. It is opened only by on-screen action rows in PROJECT view (`>> Load`, `>> Save`, `>> Save As`) and SAMPLE view (`>> Load .wav`, `>> Save .wav`). To make the cancel/exit path return to the right view without a parameter on `screen_set_mode`, two globals in `main.c` carry the routing context:

```c
extern ScreenMode disk_return_screen;   // exit returns here
extern u8         sample_load_target;   // 1-based sample slot for .wav route
                                        // (0 = legacy cursor.instrument path)
```

Each opener sets both before calling `screen_set_mode(SCREEN_DISK)`. The exit paths (B-at-root in `main.c`, SHIFT+DOWN in `navigation.c`, post-load auto-return) honor and reset them. See `doc/architecture.md section 9b` for the full convention and the per-opener table.

### 8.5 Naming flow (text input modal)

Filenames for the SAMPLE view's `>> Save .wav` action are derived
from `./data/sample_XX.wav` where XX is the 1-based sample slot. The
sample's user-facing name (32 chars max) is editable via the
`text_input` QWERTY keyboard widget (PROJECT Song Name row, SAMPLE
`>> Rename` action) but is **not** persisted in either the MAS file
(MAS has no name section) or the saved `.wav` filename. Names live in
RAM only, a documented v1 tradeoff.

---

## 9. Error Handling

All file operations return error codes. Errors are displayed as a one-line status message on the bottom screen (e.g., "Error: SD card not found", "Error: out of sample memory", "Saved: mysong.mas (45KB)"). The message auto-clears after 3 seconds.

Critical errors (SD card removed during write) trigger a full-screen error message that requires button acknowledgment before returning to the editor.
