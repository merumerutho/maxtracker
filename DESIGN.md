# maxtracker -- Design Document

**A fast, button-driven music tracker for Nintendo DS that natively creates and edits .mas files for the maxmod sound system.**

Version: 0.1 (draft)
Date: 2026-04-08

---

## 1. Vision

maxtracker is a native Nintendo DS homebrew tracker in the tradition of LSDJ, M8 Tracker, and Piggy Tracker. It prioritizes:

- **Speed of input** over visual polish. Buttons + d-pad for all core editing. Muscle memory chains. No menus for common operations.
- **LSDJ-style "go inside"** navigation: zoom from song -> pattern overview -> single channel -> individual fields.
- **Touchscreen for spatial tasks** where it genuinely helps: drawing sample waveforms, shaping envelopes, dragging mixer faders.
- **Native .mas output**: files are directly playable by maxmod in any DS homebrew without conversion.
- **maxmod playback engine**: uses maxmod's existing effects, mixing, and envelope processing rather than reinventing them.

What maxtracker is NOT:
- Not a NitroTracker clone (no touchscreen-centric pattern editing)
- Not a general-purpose sample editor (basic trim/loop/draw only)
- Not a DAW (no automation lanes, no plugin chains)

---

## 2. Platform Constraints

| Resource | Available | Notes |
|----------|-----------|-------|
| CPU | ARM9 @ 66MHz + ARM7 @ 33MHz | ARM9 = UI + logic, ARM7 = audio |
| RAM | 4MB main + 32KB fast (DTCM/ITCM) | Shared between both CPUs via cache flush |
| VRAM | 656KB total | Banked; ~256KB usable per screen |
| Screens | 2 x 256x192 | Top: non-touch, Bottom: resistive touch |
| Buttons | D-pad, A, B, X, Y, L, R, START, SELECT | 10 buttons + touchscreen |
| Storage | SD card via libfat/DLDI | Standard POSIX file I/O |
| Audio | 16 hardware channels, maxmod software mixer | Up to 32 software-mixed channels |

---

## 3. Architecture Overview

```
┌──────────────────────────────────────────────────────┐
│                     ARM9 (66MHz)                      │
│                                                       │
│  Input ──> Editor Core ──> Song Model (shared RAM)    │
│                │                                      │
│         UI Renderer ──> Top Screen (pattern grid)     │
│                    ──> Bottom Screen (context panel)   │
│                │                                      │
│         MAS Serializer ──> libfat ──> SD card          │
│         WAV Loader     <── libfat <── SD card          │
│                                                       │
├───────────── FIFO IPC ────────────────────────────────┤
│                                                       │
│                     ARM7 (33MHz)                      │
│                                                       │
│  Patched maxmod:                                      │
│    mmReadPattern() reads flat MT_Cell arrays           │
│    Everything else (effects, mixer, envelopes) stock   │
│                                                       │
│  Note preview: single-shot sample trigger via mmEffect │
└──────────────────────────────────────────────────────┘
```

**Key design choice: Option B (patched maxmod).** The maxmod source (local submodule, ISC license) is modified to read pattern data from flat uncompressed arrays in shared RAM rather than from RLE-compressed MAS pattern streams. This enables:
- Instant audibility of edits (change a note, hear it on next playback pass)
- No serialize/deserialize cycle during editing
- Full maxmod effect and envelope processing for free

MAS serialization happens only on explicit save/export. MAS import (loading existing .mas files) uses parsing logic from the mas2xm project.

See [audio_engine.md](audio_engine.md) for the detailed patch strategy.

---

## 4. Sub-Documents

The documents in `doc/` split into a "design track" describing what maxtracker is supposed to be, and a "developer track" describing how the code is actually structured and how to work on it.

**Design track** (specifications and reference material):

| Document | Contents |
|----------|----------|
| [doc/data_model.md](doc/data_model.md) | Song, pattern, instrument, sample structures; memory pool; limits |
| [doc/ui_ux.md](doc/ui_ux.md) | Screen layouts, navigation model, button mappings, LSDJ-style workflow |
| [doc/screen_layout.md](doc/screen_layout.md) | Visual layouts and palette specifications |
| [doc/audio_engine.md](doc/audio_engine.md) | maxmod patch, ARM7/ARM9 split, IPC protocol, playback model |
| [doc/file_io.md](doc/file_io.md) | MAS serialization/import, WAV loading, sample drawing save |
| [doc/test_plan.md](doc/test_plan.md) | Test scenarios across all subsystems |

**Developer track** (architecture and "how to work on this code"):

| Document | Contents |
|----------|----------|
| [doc/DEVELOPING.md](doc/DEVELOPING.md) | Entry point: build, run, test, where to read first, how to add common things |
| [doc/architecture.md](doc/architecture.md) | Code-level architecture: layers, MVC mapping, frame loop, IPC contract, lifecycle hooks, extension points |
| [doc/hardware_quirks.md](doc/hardware_quirks.md) | NDS-specific gotchas: VRAM 16-bit writes, libfat init order, cache rules, atomicity, debugging tips |
| [doc/conventions.md](doc/conventions.md) | Coding conventions: comment policy, error handling, naming, no-premature-abstraction rule |

If you're new to the project, start with `doc/DEVELOPING.md`. If you're modifying audio or cross-CPU code, also read `doc/architecture.md` § 8 and `doc/hardware_quirks.md`.

---

## 5. Memory Budget

Total main RAM: 4,194,304 bytes (4MB).

| Component | Size | Notes |
|-----------|------|-------|
| ARM7 binary + maxmod | 64KB | Fixed, runs from ARM7 WRAM |
| ARM9 binary + code | 192KB | UI, editor, serializer |
| Song metadata | 2KB | Orders, tempo, flags, name |
| Instrument pool | 32KB | 128 instruments, envelopes, notemaps |
| Pattern pool | 640KB | See section 5.1 |
| Sample pool | 1,536KB | See section 5.2 |
| MAS export buffer | 256KB | Temporary during save |
| UI state + buffers | 64KB | Clipboard, undo, cursor state |
| VRAM (both screens) | 256KB | Tiles, maps, palettes, bitmaps |
| Stack + heap overhead | 64KB | malloc metadata, stack frames |
| **Total** | **~3,106KB** | |
| **Headroom** | **~1,088KB** | Available for larger songs |

### 5.1 Pattern Pool

Each pattern cell: 5 bytes (note + inst + vol + fx + param).
Each pattern row: 5 * 32 channels = 160 bytes.
Each pattern: variable rows, typical 64 = 10,240 bytes (~10KB).

**Budget: 640KB = ~64 patterns of 64 rows at 32 channels.**

Patterns are dynamically allocated from a pool. Empty patterns (all cells empty) are represented as NULL pointers and cost zero memory. The allocator tracks peak usage and warns when approaching the limit.

For songs that need more patterns with fewer channels, the per-pattern cost is lower since we only allocate the channel count used by the song (configurable: 4, 8, 16, 24, or 32).

### 5.2 Sample Pool

**Budget: 1,536KB (1.5MB) for all sample PCM data.**

Samples are allocated from a contiguous pool. 8-bit samples use 1 byte/sample; 16-bit use 2 bytes/sample. At 8-bit 8363Hz, 1.5MB holds ~3 minutes of sample data. At 16-bit, ~1.5 minutes.

Drawn waveforms (single-cycle) are tiny: 256 samples = 256 bytes each. The sample pool can hold thousands of these.

---

## 6. Development Phases

### Phase 1: Skeleton [COMPLETE]
DS project scaffold using devkitPro. Dual-screen setup with text consoles. Input loop. Song model struct definitions. Build system (Makefile based on MAXMXDS patterns).

### Phase 2: Pattern Editor [COMPLETE]
Hybrid classic/LSDJ editing model. **Overview mode** (default): 8-channel preview showing note+instrument per channel; navigation only, no data entry. **Inside mode** (press A): single-channel zoom with all 5 fields (Note/Ins/Vol/Fx/Param); all editing happens here. Note stamping, instrument selection, hex entry for vol/fx/param, delete. Copy/paste rows and blocks.

### Phase 3: Patched maxmod + Playback [COMPLETE]
Fork maxmod submodule. Implement flat pattern reader (`mmReadPattern` alternative). ARM7/ARM9 IPC for play/stop/position. Hear patterns while editing.

### Phase 4: Instruments + Samples [COMPLETE]
Instrument parameter editor (top screen). Envelope drawing on touchscreen (bottom screen). WAV sample loading from SD card. Sample waveform drawing on touchscreen. Basic sample trim/loop point editing.

### Phase 5: Song Arrangement [COMPLETE]
Order table editor (song screen). Pattern clone, insert, delete. Song-level playback (follow mode). Repeat position. Project settings screen (`SCREEN_PROJECT`) with song-level parameters and on-screen action rows: **New, Load, Save, Save As, Compact**. SAMPLE view gained matching **Load / Save / Rename** action rows. The disk browser is now opened exclusively from these on-screen actions; the SHIFT+START shortcut was removed.

### Phase 6: MAS Export [COMPLETE]
Serialize song model to .mas binary. Pattern RLE compression (port from mmutil). Envelope delta/range encoding. Sample alignment and padding. Write to SD card. Round-trip with mmutil verified by `test/mas_diff` (33/33 PASS).

### Phase 7: MAS Import [COMPLETE]
Load .mas files into editor (reuse mas2xm parsing: flat pattern decompression, envelope decoding, sample extraction). Enables editing existing maxmod music. WAV load supports 8 / 16 / 24-bit PCM and 32-bit float, mono + stereo (mixed to mono), at any rate; covered by `test/wav_test`.

### Phase 8: Polish [IN PROGRESS]
Shipped: undo/redo (ring buffer); visual feedback (playing row highlight, channel activity); note preview; channel solo/mute; live tempo/volume; debug overlay (toggleable from PROJECT settings, force-disabled in BIG font mode); on-screen QWERTY keyboard (`text_input`) for song + sample rename. Pending for v1: see `ROADMAP.md` for the blocker list (folder-picker save, save-overwrite confirm, the `mt_song_modified()` helper).

---

## 7. Project Structure

For a per-file walkthrough see [doc/DEVELOPING.md § 9](doc/DEVELOPING.md). For the architectural rationale behind this layout see [doc/architecture.md § 2](doc/architecture.md). The current tree:

```
maxtracker/
├── DESIGN.md                  # this document
├── Makefile                   # top-level dispatcher (emulator/native/test/host-test)
├── arm7/
│   ├── Makefile
│   └── source/main.c          # maxmod init, FIFO handlers, tick callback
├── arm9/
│   ├── Makefile
│   └── source/
│       ├── core/              # model + audio shim + utilities
│       │   ├── main.c         # entry point, frame loop, scene dispatch, input handlers
│       │   ├── song.[ch]      # MT_Song / MT_Pattern / MT_Instrument / MT_Sample model
│       │   ├── playback.[ch]  # ARM9 audio shim, ARM7 IPC, lifecycle hook owner
│       │   ├── clipboard.[ch] # block clipboard + single-slot note clipboard
│       │   ├── undo.[ch]      # undo ring buffer
│       │   ├── scale.[ch]     # music theory utilities
│       │   ├── memtrack.[ch]  # memory estimation
│       │   └── util.h         # small helpers
│       ├── io/                # SD card / external file formats
│       │   ├── mas_write.[ch] # MAS serializer
│       │   ├── mas_load.[ch]  # MAS deserializer
│       │   ├── wav_load.[ch]  # WAV sample loader
│       │   └── filebrowser.[ch] # generic file picker
│       ├── ui/                # views and rendering
│       │   ├── screen.[ch]    # framebuffer init + ScreenMode
│       │   ├── font.[ch]      # bitmap font renderer (SMALL 4×6 / BIG 6×8)
│       │   ├── editor_state.[ch] # global EditorCursor
│       │   ├── navigation.[ch] # SHIFT-chord dispatcher (LSDJ "rooms in a house")
│       │   ├── pattern_view.[ch] # pattern grid (input handler in main.c)
│       │   ├── instrument_view.[ch]
│       │   ├── sample_view.[ch]
│       │   ├── mixer_view.[ch]
│       │   ├── song_view.[ch]
│       │   ├── project_view.[ch] # song-level settings + on-screen action rows
│       │   ├── waveform_view.[ch] # LFE editor (LFE) — top of LFE chain
│       │   ├── lfe_fx_view.[ch]   # LFE effects room
│       │   ├── waveform_render.[ch] # shared scope-fill renderer
│       │   ├── debug_view.[ch]    # tier-1 always-on debug overlay
│       │   ├── text_input.[ch]    # modal on-screen QWERTY keyboard
│       │   └── draw_util.h
│       └── test/test.[ch]     # unit tests (run on host or device with UNIT_TESTING)
├── include/                   # cross-CPU shared headers
│   ├── mt_ipc.h               # FIFO opcodes
│   └── mt_shared.h            # shared state struct
├── lib/maxmod/                # patched maxmod submodule (MAXTRACKER_MODE)
├── data/                      # default data embedded via NitroFS
├── release/                   # build outputs (.nds files)
└── doc/                       # design + developer documentation
    ├── DEVELOPING.md          # entry point for developers
    ├── architecture.md        # code-level architecture
    ├── hardware_quirks.md     # NDS-specific rules
    ├── conventions.md         # coding conventions
    ├── data_model.md          # song struct reference
    ├── audio_engine.md        # maxmod patch and IPC
    ├── file_io.md             # MAS format reference
    ├── ui_ux.md               # navigation and button mappings
    ├── screen_layout.md       # visual layouts
    └── test_plan.md           # test scenarios
```

Notes on the layout:

- The project structure has settled into the `core` / `io` / `ui` split shown above. Earlier drafts of this document referenced files like `editor.h/c`, `pool.h/c`, `ipc.h/c`, and a separate per-screen `song.h/c` that were never built — the current code consolidates editor logic in `main.c` and `editor_state.c`, uses standard `malloc`/`free` instead of pool allocators, and centralizes IPC inside `playback.c`.
- The LFE chain (`waveform_view`, `lfe_fx_view`, `waveform_render`) is gated by `MAXTRACKER_LFE` and links against the standalone synthesis library at `lib/lfe/`. Disabling that define yields a tracker-only build with no LFE rooms; the `#ifdef` sprinkled through `main.c` and `navigation.c` is on the post-v1 cleanup list.
- `text_input` is a self-contained modal widget — see `doc/ui_ux.md § 13`. Any future view that needs string entry should reuse it rather than rolling a custom editor.
- `pattern_view.c` is unusual in that its input handler lives in `main.c` (`handle_input_pattern`) rather than in the view file itself. This is a historical accident from when pattern input was the only input. Other views (`instrument_view`, `mixer_view`, etc.) own their own `_input` functions. Both patterns coexist; either is acceptable for new screens.
- `main.c` is currently 1500+ lines and is on the future-work list to be split into `scene_manager.c` + `input_router.c`. Don't pre-emptively split it; the change is queued for whenever the project becomes a proper repository.

---

## 8. Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| devkitARM | Latest | ARM cross-compiler toolchain |
| libnds | Latest | DS hardware abstraction (graphics, input, FIFO, timers) |
| libfat | Latest | FAT filesystem on SD card |
| libfilesystem | Latest | NitroFS support |
| maxmod | Forked from blocksds | Audio playback (patched for flat pattern reading) |

Build system: GNU Make, following devkitPro `ds_rules` conventions (same as MAXMXDS).

---

## 9. Licensing

maxtracker is licensed under **GPL-3.0-or-later**. The full license text lives in [COPYING](COPYING) at the repository root.

Bundled libraries retain their original licenses:

- **maxmod** (in `lib/maxmod/`) is ISC-licensed and stays under its original terms. ISC is GPL-compatible, so linking against it from a GPL-licensed maxtracker is fine. Patches we apply to the maxmod source live in our fork of `lib/maxmod/` and inherit the ISC license unless explicitly noted otherwise.
- **lib/lfe** (the waveform editor library) is GPL-3.0-or-later, matching maxtracker. It is structurally a separate library to keep the algorithm code portable and host-testable, but it is part of maxtracker for licensing and distribution purposes.

The MAS format specification (in `C:\Projects\mas_spec\`) and the mas2xm converter are available as reference implementations.

The relicensing decision was made when planning the waveform editor: porting algorithms from Mutable Instruments Braids (which is GPLv3) into maxtracker would require maxtracker itself to be GPL-compatible. Going GPLv3 across the project resolves the license question once for any future GPL-licensed code we want to incorporate.

---

## 10. Related Projects

| Project | Path | Relationship |
|---------|------|-------------|
| MAXMXDS | `C:\Projects\MAXMXDS` | Reference for DS project structure, maxmod usage, IPC patterns |
| mas_spec | `C:\Projects\mas_spec` | Authoritative MAS format specification |
| mas2xm | `C:\Projects\mas2xm` | MAS parsing logic (reusable for import), XM export |
| mmutil | `C:\Projects\mmutil` | MAS serialization reference (pattern compression, envelope encoding) |
