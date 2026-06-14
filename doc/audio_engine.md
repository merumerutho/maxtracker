# maxtracker -- Audio Engine

Parent: [README.md](../README.md)

---

## 1. Architecture: Option B (Patched maxmod)

maxtracker modifies maxmod's pattern reader to consume flat `MT_Cell` arrays directly from shared RAM, bypassing the RLE-compressed MAS pattern stream. All other maxmod subsystems (effects processing, envelope evaluation, sample playback, software mixer) remain unmodified.

This gives:
- **Edit-and-hear**: change a note, hear it on the next playback pass without restarting.
- **Full effect fidelity**: all maxmod effects work exactly as documented because the effect processing code is untouched.
- **Low risk**: the patch surface is small (~100 lines), localized to one function.

---

## 2. The Patch: mmReadPattern

### 2.1 Current maxmod Behavior

In `source/core/mas_arm.c`, `mmReadPattern()` is called once per row during playback. It reads compressed pattern data from a byte stream pointed to by `mpp_layer->pattread`, decodes channel entries using IT-style compression (mask bytes, value caching, COMPR_FLAGs + MF flags), and populates `mm_module_channel` structs.

The function signature:

```c
IWRAM_CODE ARM_CODE mm_bool mmReadPattern(mpl_layer_information *mpp_layer);
```

It returns 1 on success, 0 if the pattern references more channels than available.

### 2.2 Patched Behavior

Under a compile-time flag `#define MAXTRACKER_MODE`, `mmReadPattern` is replaced with a version that reads from a flat `MT_Cell` array. The function prototype is unchanged -- the patch is entirely internal.

**Shared state (ARM9-writable, ARM7-readable):**

```c
// Defined in include/mt_shared.h, allocated by ARM9 in main RAM.
// cells is a FLAT array: cells[row * channel_count + ch], pointing
// directly into the current MT_Pattern's cells[] flexible array member.
typedef struct {
    volatile MT_Cell *cells;        // Flat current-pattern cell array (NULL = empty)
    volatile u16 nrows;             // Current pattern's row count
    volatile u8  channel_count;     // Active channel count
    volatile u8  active;            // 1 = maxtracker mode, 0 = standard MAS mode

    volatile u8  playing;           // 1 = playing (written by ARM9 start/stop)
    volatile u8  _pad_play[3];

    // Playback position packed into one 32-bit store so ARM9 never reads a
    // torn (tick,row,position). bits 7:0 = pos, 15:8 = row, 23:16 = tick.
    volatile u32 pos_state;

    volatile u32 mute_mask;         // bit N set = channel N muted

    // Order + pattern tables: ARM7 resolves the next cells pointer itself
    // on a pattern transition, with no FIFO round-trip.
    u8 order_count;
    u8 orders[MT_MAX_ORDERS];       // copy of song.orders[]
    u8 patt_count;
    MT_PatternEntry patterns[256];  // {cells, nrows} per pattern index
} MT_SharedPatternState;

// MT_PatternEntry: per-pattern lookup info for ARM7.
typedef struct {
    volatile MT_Cell *cells;        // flat cell array (NULL = empty)
    volatile u16      nrows;
    u16               pad;
} MT_PatternEntry;

// Single global instance, address communicated via FIFO (MT_CMD_SET_SHARED).
extern MT_SharedPatternState *mt_shared;
```

Position is packed/unpacked with the `MT_POS_PACK` / `MT_POS_POSITION` /
`MT_POS_ROW` / `MT_POS_TICK` helpers in `mt_shared.h`. The cells pointer is a
flat 1-D array indexed as `cells[row * channel_count + ch]`; the patched
`mmReadPattern` does that index arithmetic itself rather than relying on a
fixed `[256][32]` declaration.

**Patched mmReadPattern:**

```c
#ifdef MAXTRACKER_MODE
IWRAM_CODE ARM_CODE mm_bool mmReadPattern(mpl_layer_information *mpp_layer)
{
    if (!mt_shared || !mt_shared->active || !mt_shared->cells)
        return mmReadPattern_original(mpp_layer); // fallback to MAS mode

    int row = mpp_layer->row;
    mm_word update_bits = 0;
    mm_module_channel *module_channels = mpp_channels;

    const MT_Cell *row_data = &mt_shared->cells[row * mt_shared->channel_count];

    for (int ch = 0; ch < mt_shared->channel_count && ch < mpp_nchannels; ch++)
    {
        const MT_Cell *cell = &row_data[ch];
        mm_module_channel *mc = &module_channels[ch];

        // Skip completely empty cells
        if (cell->note == 250 && cell->inst == 0 &&
            cell->vol == 0 && cell->fx == 0 && cell->param == 0)
            continue;

        update_bits |= 1 << ch;
        mc->flags = 0;

        // Note
        if (cell->note != 250) {
            if (cell->note == 254)
                mc->flags |= MF_NOTECUT;
            else if (cell->note == 255)
                mc->flags |= MF_NOTEOFF;
            else {
                mc->pnoter = cell->note;
                mc->flags |= MF_START;
            }
        }

        // Instrument
        if (cell->inst != 0) {
            if (!(mc->flags & (MF_NOTECUT | MF_NOTEOFF))) {
                if (cell->inst <= mpp_layer->songadr->instr_count) {
                    if (mc->inst != cell->inst) {
                        if (mpp_layer->flags & MAS_HEADER_FLAG_OLD_MODE)
                            mc->flags |= MF_START;
                        mc->flags |= MF_NEWINSTR;
                    }
                    mc->inst = cell->inst;
                }
                mc->flags |= MF_DVOL;
            }
        }

        // Volume command
        if (cell->vol != 0)
            mc->volcmd = cell->vol;

        // Effect + param
        if (cell->fx != 0 || cell->param != 0) {
            mc->effect = cell->fx;
            mc->param = cell->param;
        }

        // Reconstruct MF flags for the rest of the engine
        if (cell->vol != 0)
            mc->flags |= MF_HASVCMD;
        if (cell->fx != 0 || cell->param != 0)
            mc->flags |= MF_HASFX;
    }

    mpp_layer->mch_update = update_bits;
    return 1;
}
#endif
```

### 2.3 What Stays Unmodified

Everything after `mmReadPattern` in the processing chain:

- `mmUpdateChannels` -- processes note starts, NNA, instrument lookup, envelope init
- `mmChannelProcess` -- evaluates effects (Axx speed, Bxx jump, Cxx break, Dxx volslide, etc.)
- `mmUpdateEnvelopes` -- evaluates volume/panning/pitch envelopes
- `mmMixerMix` -- software mixer, output to hardware sound channels
- `mmPulse` -- the top-level per-tick driver that calls all of the above

The patch only replaces how row data gets into `mm_module_channel` structs. The engine takes it from there.

### 2.4 Fallback: Standard MAS Mode

When `mt_shared->active == 0`, the patched function falls through to `mmReadPattern_original()` (the original compressed-stream reader, renamed). This allows maxtracker to also play standard .mas files without the editing infrastructure (e.g., for auditioning loaded files).

---

## 3. Song Metadata for Playback

maxmod's sequencer needs more than just pattern data. It needs:

- **Tempo and speed**: Tempo is set via `mmSetModuleTempo()` (ARM7 receives `MT_CMD_SET_TEMPO` carrying a Q10 multiplier and calls it). Speed (ticks/row) has **no** runtime API: `MT_CMD_SET_SPEED` is currently an unimplemented no-op on ARM7, so speed changes come only through pattern effect Axx.
- **Pattern row count**: `mt_shared->nrows` is read by the sequencer to know when a pattern ends.
- **Order table / song position**: The sequencer needs to know which pattern to play next.
- **Instrument and sample data**: The effect processor and mixer need instrument envelopes and sample PCM.

### 3.1 Approach: Minimal MAS Header + External Pattern Data

For playback, maxtracker constructs a **minimal MAS header** in RAM. This header contains:

- Module flags (freq_mode, xm_mode, old_mode, etc.)
- Instrument data (serialized in MAS format -- envelopes, note maps)
- Sample headers (info + DS sample struct), with `point` set to the existing PCM allocation
- **Stub pattern data** (one empty pattern per actual pattern, just row counts)
- Order table

PCM sample data is NOT copied into the MAS buffer. Each sample's `mm_mas_ds_sample.point` field is set to the address of the permanent `pcm_data` allocation in main RAM. The mixer checks `point` first and uses it directly if non-zero, falling back to inline `data[]` only when `point` is zero. This keeps the MAS buffer under 15 KB regardless of sample size.

All `pcm_data` allocations include 4 extra bytes for DS hardware interpolation wraparound. These bytes are patched by `patch_sample_wraparound()` at build time with a copy of the loop-start data (looping samples) or zeros (one-shot samples).

The actual pattern content is read from flat arrays via the patched `mmReadPattern`. The MAS header provides the "scaffolding" that maxmod needs for instrument/sample lookup, while patterns come from the editing buffers.

This hybrid approach avoids rewriting the instrument and sample loading code in maxmod while still getting live pattern editing.

```
┌──────────────────────────────────────────────┐
│ Minimal MAS in RAM (~10 KB)                  │
│  - Prefix (8 bytes)                          │
│  - Header (tempo, speed, flags, orders)      │
│  - Instrument offset table + instrument data │
│  - Sample offset table + sample headers      │
│    (point -> existing pcm_data allocations)  │
│  - Pattern offset table -> stub patterns     │
│    (each stub: just nrows byte, no data)     │
└──────────────────────────────────────────────┘
         │
         │ mmPlayMAS(addr) sets up the sequencer
         │ Instruments/samples resolved via offset tables
         │ Mixer reads PCM from pcm_data via ds->point
         │
         ▼
┌──────────────────────────────────────────────┐
│ Patched mmReadPattern                        │
│  - Ignores the stub pattern data             │
│  - Reads from mt_shared->cells[row*ncols+ch] │
│  - ARM9 updates mt_shared->cells pointer     │
│    when sequencer advances to next pattern    │
└──────────────────────────────────────────────┘
```

### 3.2 Rebuilding the MAS Header

The minimal MAS header is rebuilt by `playback_rebuild_mas()` in `playback.c`.
There is **no** granular per-section rebuild: the function stops playback if
running, calls `build_mas_buffer()` to regenerate the whole buffer from the
current song, and (if it was playing) resumes from the saved order position.

| Event | What changes | Action |
|-------|-------------|--------|
| Instrument parameters changed | Envelope data, note map | `playback_rebuild_mas()` (full rebuild) |
| Sample loaded/drawn/deleted | Sample headers (point updated) | `playback_rebuild_mas()` (full rebuild) |
| Channel count changed | Header flags | `playback_rebuild_mas()` (full rebuild) |
| Tempo changed | Q10 multiplier | `playback_set_tempo()` → `MT_CMD_SET_TEMPO`, no rebuild |
| Order table changed | Sequence array | `playback_rebuild_mas()` (full rebuild) |
| Pattern added/removed | Pattern stubs + offset table | `playback_rebuild_mas()` (full rebuild) |

The rebuild is cheap because the MAS buffer contains only metadata and sample
headers; PCM data is referenced via `point` pointers, not copied.

---

## 4. ARM7/ARM9 IPC Protocol

### 4.1 FIFO Channel

maxtracker uses `FIFO_USER_08` (same as MAXMXDS's `FIFO_XMX`) for custom commands. maxmod continues to use its own FIFO channel for internal communication.

### 4.2 Command Encoding

```c
#define MT_MKCMD(cmd, param)  ((((u32)(cmd)) << 24) | ((u32)(param) & 0x00FFFFFFu))
#define MT_CMD_TYPE(val)      (((val) >> 24) & 0xFFu)
#define MT_CMD_PARAM(val)     ((val) & 0x00FFFFFFu)
```

### 4.3 ARM9 -> ARM7 Commands

| Command | Code | Parameter | Description |
|---------|------|-----------|-------------|
| MT_CMD_PLAY | 0x01 | pos \| (row<<8) | Start playback at this order position and row |
| MT_CMD_STOP | 0x02 | -- | Stop playback |
| MT_CMD_SET_SHARED | 0x03 | -- | Followed by fifoSendAddress with MT_SharedPatternState* |
| MT_CMD_SET_MAS | 0x04 | -- | Followed by fifoSendAddress with MAS buffer* |
| MT_CMD_SET_PATTERN | 0x05 | pattern index | Update mt_shared->cells to point to this pattern's data |
| MT_CMD_PREVIEW_NOTE | 0x06 | note\|inst<<8 | Play a single note for preview (without affecting sequencer) |
| MT_CMD_STOP_PREVIEW | 0x07 | -- | Stop preview note |
| MT_CMD_SET_MUTE | 0x08 | ch\|mute<<8 | Mute/unmute a channel |
| MT_CMD_SET_TEMPO | 0x09 | Q10 tempo multiplier | `0x400` = base tempo. ARM9 converts from absolute BPM; ARM7 calls `mmSetModuleTempo()` |
| MT_CMD_SET_SPEED | 0x0A | ticks per row | **Unimplemented no-op** on ARM7; speed changes come via effect Axx |
| MT_CMD_REBUILD_DONE | 0x0B | -- | MAS header has been rebuilt; ARM7 should re-resolve pointers |

### 4.4 ARM7 -> ARM9 Commands

| Command | Code | Parameter | Description |
|---------|------|-----------|-------------|
| MT_CMD_TICK | 0x80 | pos \| (row<<8) | Current playback position, sent on tick 0 of each row. The tick travels in `mt_shared->pos_state`, **not** in this param. |
| MT_CMD_PATTERN_END | 0x81 | next_order_pos | Notifies ARM9 that the order position changed (ARM9 uses it to follow the cursor); ARM7 itself resolves the next pattern's cells |
| MT_CMD_SONG_END | 0x82 | -- | Song reached end (order 0xFF) |

### 4.5 Pattern Advance Protocol

Pattern transitions are resolved **on ARM7**, using the order/pattern tables in
`MT_SharedPatternState` — there is no FIFO round-trip in the hot path. This
avoids a race where the cells pointer could change underneath a tick callback
(see `feedback_pattern_transition_ipc`).

When the sequencer crosses into a new order position:

1. ARM7 sees `pos` change in its tick callback.
2. ARM7 indexes `mt_shared->orders[pos]` to get the next pattern index, then
   reads `mt_shared->patterns[pattern_index]` for the new `cells` pointer and
   `nrows`, and updates its local view directly.
3. ARM7 sends `MT_CMD_PATTERN_END` to ARM9 as a notification only (so the
   editor can follow the playback position); ARM9 does not need to push a new
   cells pointer back.

ARM9 populates `orders[]` and `patterns[]` (and flushes the cache) when playback
starts and whenever the song changes via `playback_rebuild_mas()`.

---

## 5. Note Preview

When the user presses X+A (preview note), ARM9 sends `MT_CMD_PREVIEW_NOTE` to ARM7. ARM7 uses maxmod's `mmEffect()` or `mmEffectEx()` to play a single sample hit on a reserved channel, independent of the sequencer. This avoids interfering with pattern playback.

The preview uses the currently selected instrument's sample with the note at the cursor position.

---

## 6. Follow Mode

When follow mode is active (L+R toggle), the ARM9 UI tracks the playback position:

1. ARM7 sends `MT_CMD_TICK` every tick with position/row info.
2. ARM9 updates the cursor row to match the playing row.
3. On `MT_CMD_PATTERN_END`, ARM9 switches the editor view to the next pattern.

The user can still edit during follow mode -- the cursor snaps to the playback position but can be temporarily moved away. It re-syncs on the next row.

---

## 7. MAS Compatibility Note

The patched maxmod is **not** a general maxmod replacement. It's a maxmod fork specifically for maxtracker. Games that use maxmod play standard .mas files and don't need the patch. The fork only affects `mmReadPattern` under the `MAXTRACKER_MODE` compile flag, which is only enabled in the maxtracker ARM7 build.

The .mas files that maxtracker exports are **standard, unmodified MAS files** that any stock maxmod can play. The patch only affects the editing/playback workflow within maxtracker itself.

---

## 8. ARM7 Initialization Order (Critical)

maxmod's ARM7 initialization is **asynchronous**. The sequence is:

1. ARM7 calls `mmInstall(FIFO_MAXMOD)`: registers FIFO comms, but does NOT
   initialize the mixer, timers, or event system. `mmIsInitialized()` returns false.

2. ARM9 calls `mmInit(&sys)`: sends `MSG_BANK` via FIFO to ARM7.

3. ARM7's FIFO datamsg handler receives `MSG_BANK` and calls `mmInit7()`, which:
   - Sets up Timer 0 ISR (`mmFrame`)
   - Initializes the mixer, volumes, channels
   - Calls `mmSetEventHandler(mmEventForwarder)` (default forwarder)
   - Calls `mmInitialize(1)`, marking maxmod as initialized

**Critical gotcha:** Any call to `mmSetEventHandler()` BEFORE step 3 completes
will be **silently overwritten** by `mmInit7()`, which always sets the handler
to `mmEventForwarder`. This caused a bug where the tick callback never fired
because it was registered before `mmInit7()` ran.

**Correct pattern:**
```c
// ARM7 main():
mmInstall(FIFO_MAXMOD);
// DON'T call mmSetEventHandler here -- mmInit7 hasn't run yet

// Wait for ARM9's mmInit to trigger mmInit7 via FIFO:
while (!mmIsInitialized())
    swiWaitForVBlank();

// NOW it's safe to set our callback:
mmSetEventHandler(mt_EventCallback);
```

**General rule:** On ARM7, never call any maxmod API that depends on initialized
state (event handlers, tempo, volume, playback) until `mmIsInitialized()` returns
true. The only exception is `mmInstall()` itself.
