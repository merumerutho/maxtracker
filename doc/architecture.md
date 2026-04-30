# maxtracker -- Architecture

Parent: [DESIGN.md](../DESIGN.md)

---

## 0. What this document is

A code-level architecture reference for developers. Where the design documents in this directory describe what maxtracker is *supposed* to be, this one describes what it currently *is*: how the source files are organized, how data flows through the running program, what runs on which CPU, and which invariants the code depends on without enforcing them at the type system level.

If you only read one document before touching the codebase, read this one. Then read [hardware_quirks.md](hardware_quirks.md), because most of the bugs that escape host tests come from violating one of the rules described there.

---

## 1. The pattern in one paragraph

Maxtracker is a layered C codebase organized as a loose Model-View-Controller, where the views render in immediate mode inside a single ARM9 frame loop, and audio runs on a second CPU (ARM7) reading song data through a shared-memory contract that has its own atomicity rules. The model is pure data (`song.c`, `clipboard.c`, `undo.c`, `scale.c`) with no UI dependencies and is fully host-testable. The views (`pattern_view.c`, `instrument_view.c`, `mixer_view.c`, `sample_view.c`, `project_view.c`) read state and rebuild the framebuffer every frame; they hold no widget tree and no per-frame state of their own. The controller is concentrated in `main.c` (per-screen input handlers plus the scene-transition logic), which mutates the model and the editor cursor in response to button input. The editor cursor itself lives in `editor_state.c` because it isn't really part of the song model and isn't really part of any single view. The audio path is structurally separate: ARM9's `playback.c` is a thin shim that flushes shared state and sends FIFO commands; ARM7's `main.c` runs a patched maxmod that reads pattern data from that shared state in real time.

This isn't a famous textbook pattern by name, but each piece is doing something well-understood, and the combination is appropriate for a tracker on a 66 MHz embedded system with 4 MB of RAM.

---

## 2. Layered module split

The source tree is divided into four buckets, each with a clear allowed-dependency direction. Anything below can be used by anything above; nothing below should reach upward.

```
arm9/source/ui/         ← can call core/, can call io/, cannot call other ui/ siblings except via editor_state
arm9/source/io/         ← can call core/, cannot call ui/
arm9/source/core/       ← can call other core/, cannot call io/ or ui/
include/                ← types and constants shared between ARM9 and ARM7
arm7/source/            ← lives on the other CPU, talks to ARM9 only via FIFO + shared memory
```

The `core` layer holds the song model and the pure-logic subsystems: `song.c`, `clipboard.c`, `undo.c`, `scale.c`, `playback.c`, `memtrack.c`. With the recent restructuring, none of these files include any UI header. `playback.c` is in core because it's part of the editor runtime (not because it does I/O), and it has the only legitimate two-way relationship with the model: it registers lifecycle hooks via `song_set_pattern_lifecycle` so the song module can notify it before freeing patterns without taking a static dependency on it. See section 9 below.

The `io` layer holds anything that touches the SD card or external file formats: `mas_write.c`, `mas_load.c`, `wav_load.c`, `filebrowser.c`. These depend on `core` (they read and write `MT_Song`) but not on `ui`.

The `ui` layer holds the views and rendering primitives. Each view file owns a single screen mode and exposes a small public API of `_input` and `_draw` functions. `screen.c` and `font.c` are the rendering primitives. `editor_state.h/c` holds the global editor cursor.

The `include/` directory holds the cross-CPU contract: `mt_ipc.h` defines the FIFO command opcodes and `mt_shared.h` defines the shared memory layout. Both are includable from ARM7 and ARM9.

The `arm7/source/` tree is its own world. It includes the cross-CPU headers but nothing else from the ARM9 side. Most of the audio engine itself lives in `lib/maxmod/` as a patched submodule; ARM7's `main.c` is a thin wrapper that initializes maxmod, installs the FIFO handlers, and forwards tick events.

---

## 3. The frame loop

Every interactive ARM9 frame is the same shape, driven by `main()` in `arm9/source/core/main.c`:

```
while (1) {
    scanKeys();                          // libnds: latch button state
    switch (current_screen) {
        case SCREEN_PATTERN:    handle_input_pattern(); pattern_view_draw(top_fb); ...
        case SCREEN_INSTRUMENT: instrument_view_input(...); instrument_view_draw(...);
        case SCREEN_SAMPLE:     sample_view_input(...); sample_view_draw(...);
        case SCREEN_MIXER:      mixer_view_input(...); mixer_view_draw(...);
        case SCREEN_PROJECT:    project_view_input(...); project_view_draw(...);
        case SCREEN_DISK:       filebrowser_input(...); filebrowser_draw(...);
        case SCREEN_SONG:       handle_input_song(...); draw_song_screen();
    }
    if (status_timer > 0) status_timer--;
    playback_update();                   // pull ARM7 position into editor cursor
    autosave_timer++; if (...) mas_write(autosave path, ...);
    swiWaitForVBlank();
}
```

Three things to internalize about this loop:

**The dispatch is a flat switch on `current_screen`.** There is no scene-transition framework, no enter/exit hooks, no event subscription. To navigate to a different screen you call `screen_set_mode(NEW_SCREEN)` and then return; the next iteration of the loop calls the new screen's input and draw functions. This means a screen's `_draw` function is called from a cold start every frame and has no opportunity to do "first-time setup". If a screen needs persistent state, that state lives at file scope inside the view's `.c` file, not inside a widget object.

**Every frame redraws everything.** None of the views are dirty-rectangle-aware or repaint-on-demand. They read whatever's currently in `song`, `cursor`, and `playback`'s state and rebuild the framebuffer pixel by pixel from scratch. This is the immediate-mode pattern; it's covered in detail in section 6.

**`playback_update()` runs unconditionally at the end of every frame.** This is how ARM9 learns about playback position changes: there's no callback or interrupt for it. ARM7 writes to `mt_shared->pos_state` whenever the song advances; ARM9 reads it once per frame through the uncached mirror. The cursor's `play_row`, `play_order`, and `playing` flag are populated here. If you're in follow mode (`cursor.follow` is true and `cursor.playing` is true), `cursor.row` and `cursor.order_pos` are also overwritten with the playback position.

The loop is timed by `swiWaitForVBlank()`, which gives a steady ~59.83 Hz on real hardware. Anything you put inside the loop must complete in less than one frame budget, including the worst-case redraw of the busiest screen. The pattern view is the heaviest at the moment because it can draw 8 channels x 28 visible rows of text.

---

## 4. Model-View-Controller mapping

The MVC labels are loose but consistent. Use them as vocabulary, not as a doctrine.

| Role | Files | Notes |
|------|-------|-------|
| **Model** | `song.c`, `clipboard.c`, `undo.c`, `scale.c` | Pure data, host-testable, no UI includes. `song.c` depends on no other module except via the lifecycle hook callbacks. |
| **View** | `pattern_view.c`, `instrument_view.c`, `mixer_view.c`, `sample_view.c`, `project_view.c`, plus `screen.c` and `font.c` as rendering primitives | Read state, write framebuffer. Should not modify the model. May call into `playback.c` to read mute state and similar live values, but should not start/stop playback (that's the controller's job). |
| **Controller** | `main.c` (the per-screen `handle_input_*` and `*_view_input` functions, plus the scene-transition `screen_set_mode` calls), and the file-loading code in the disk screen | Mutates model and cursor in response to input. Owns the scene state machine. |
| **Editor state** | `editor_state.c` | Sits between controller and view. The cursor is read by every view and written by the controller. It used to live in `pattern_view.c`, which made `pattern_view` a de-facto state holder; it was extracted into its own module. |
| **Audio shim** | `playback.c`, `arm7/source/main.c`, `lib/maxmod/` (patched) | The audio engine. Cross-CPU. Has its own contract — see section 8. |
| **I/O** | `mas_write.c`, `mas_load.c`, `wav_load.c`, `filebrowser.c` | Sits beside the model. Reads and writes the song from disk. Called from the controller (mostly from the disk screen and from the autosave path in the frame loop). |

Two specific consequences of this mapping that aren't obvious:

**The view is allowed to query `playback.c`.** A view like `pattern_view` calls `playback_get_mute_mask()` to color muted channels. This is technically a model-to-view leak, but `playback.c` is the canonical source of mute state at runtime, and threading it through some intermediate object would be ceremony without value at this scale. The rule is: views may *read* from `playback.c`, but should not *call* anything that mutates audio state. Starting and stopping playback is the controller's job.

**The controller is concentrated in `main.c`.** All input dispatch lives in one file. This is why `main.c` is currently 1500+ lines and why it's marked for a future split into `scene_manager.c` and `input_router.c`. When you make that split, the rule of thumb is: code that decides *which screen we're on* belongs in scene management; code that interprets buttons *for the current screen* belongs in the per-screen handlers (which already live in the view files in some cases, e.g. `instrument_view_input`).

---

## 5. Module inventory

A one-paragraph orientation per file. A more detailed per-file reference will live in `module_reference.md` in a later doc pass.

### Core (`arm9/source/core/`)

- **`song.h/.c`** -- The `MT_Song` struct and its allocator. `song_init` zeroes everything and seeds defaults including a square-wave sample so playback works out of the box. `song_alloc_pattern` allocates the variable-size `MT_Pattern` and notifies the registered lifecycle hooks. `song_free` is symmetric. The model has zero behavior beyond storage and lifecycle.
- **`playback.h/.c`** -- ARM9's audio shim. Maintains the `shared_state` struct that ARM7 reads, sends FIFO commands, registers lifecycle hooks at init time, and provides `playback_update()` for the frame loop to pull the latest position. Also owns `playback_preview_note` for non-sequenced note auditioning.
- **`clipboard.h/.c`** -- Two clipboards: a block clipboard for multi-cell selections (`MT_Clipboard`, malloc-backed) and a single-slot note clipboard for M8-style A-press copy/paste (`MT_NoteClipboard`, value-typed). They are independent so single-slot operations don't clobber multi-cell selections.
- **`undo.h/.c`** -- Fixed-depth ring buffer of pattern edits. Each entry is a malloc'd snapshot of the cells about to be modified. `count_rows` is u16 because patterns can be 256 rows tall and a u8 truncates to zero on full-pattern blocks. The ring is 64 entries deep (`MT_UNDO_DEPTH`).
- **`scale.h/.c`** -- Music theory utilities: note name lookup, semitone math, scale-aware key navigation. Pure functions, no state.
- **`memtrack.h/.c`** -- Memory usage estimation and the sentinel-based "do you have enough RAM" check used by the loader. The check is soft; it sets a warning flag but doesn't block.
- **`main.c`** -- Entry point, frame loop, scene dispatch, every per-screen input handler that hasn't been moved into its view file. The biggest file in the project. Marked for future splitting.

### I/O (`arm9/source/io/`)

- **`mas_write.h/.c`** -- Serializes `MT_Song` to maxmod's `.mas` binary format. Pattern RLE compression follows mmutil's `Write_Pattern` exactly, including the field-caching and MF flag carry-forward logic. Envelope encoding follows mmutil's `Write_Instrument_Envelope`.
- **`mas_load.h/.c`** -- Inverse of `mas_write`. Reads a whole `.mas` file into a buffer, then parses it into the global `song`. All offset dereferences are bounds-checked against `file_size`. Returns 0 on success, 1 on success-with-warnings, negative on hard failure.
- **`wav_load.h/.c`** -- Loads a WAV file into raw PCM bytes. Used by the disk screen when the user picks a `.wav` file.
- **`filebrowser.h/.c`** -- Generic file browser used by the disk screen. Independent of any particular file format.

### UI (`arm9/source/ui/`)

- **`screen.h/.c`** -- Initializes both NDS screens in 8bpp bitmap mode, defines the global palette, exposes `top_fb`/`bot_fb` framebuffer pointers, and tracks `current_screen`. `screen_set_mode` is the scene-transition primitive.
- **`font.h/.c`** -- Bitmap font renderer. The whole UI is text-mode. `font_puts`, `font_putc`, `font_printf`, `font_fill_row`, `font_clear` are the public primitives.
- **`editor_state.h/.c`** -- The global `EditorCursor` struct. Read by every view, written by the controller. See section 7.
- **`navigation.h/.c`** -- Centralized SHIFT-chord dispatcher (LSDJ "rooms in a house"). Every view calls `navigation_handle_shift(down, held)` first; if it returns true the view skips its own input handling for that frame. Owns SHIFT+L/R/UP/DOWN screen transitions, SHIFT+START playback, SHIFT+B/A clipboard.
- **`pattern_view.h/.c`** -- The pattern grid. Two display modes: overview (8 channels with note+inst per cell) and inside (single channel with all 5 fields). The pattern view is the only one that doesn't have its own `_input` function; its input handler is `handle_input_pattern` in `main.c`, which is the largest input handler in the project and the one we extended for the new A-press clipboard behavior.
- **`instrument_view.h/.c`** -- Instrument parameter editor. Reads `cursor.instrument` to know which instrument is being edited. B+A reset is two-tap-confirmed.
- **`sample_view.h/.c`** -- Sample editor with waveform display and on-screen action rows (Load / Save / Rename). B+A delete is two-tap-confirmed.
- **`mixer_view.h/.c`** -- Per-channel mute/solo and volume.
- **`song_view.h/.c`** -- Order-table editor.
- **`project_view.h/.c`** -- Song-level metadata + on-screen action rows (New / Load / Save / Save As / Compact). The Song Name row opens the `text_input` keyboard.
- **`waveform_view.h/.c`** / **`lfe_fx_view.h/.c`** -- LFE (LFE) rooms. Gated by `MAXTRACKER_LFE`. Backed by `lib/lfe/`.
- **`waveform_render.h/.c`** -- Shared scope-fill renderer used by every view that draws PCM (sample, LFE editor, LFE FX).
- **`debug_view.h/.c`** -- Tier-1 always-on debug overlay. Auto-disabled in BIG font mode (the 5-row footer would fall off the 24-row grid).
- **`text_input.h/.c`** -- Modal on-screen QWERTY keyboard. Single-instance; while active, owns both screens and steals input from whatever view called `text_input_open()`. Currently used for song + sample renames. See `doc/ui_ux.md § 13`.
- **`draw_util.h`** -- Small drawing helpers shared between views.

### Tests (`arm9/source/test/`)

- **`test.h/.c`** -- Unit tests that can run on either device or host. Compiled into the device build when `UNIT_TESTING` is defined; compiled into the host test binary unconditionally. Houses ~50 test functions covering song model, clipboard, undo, MAS roundtrip, panning encoding, instrument-sample mapping, and grooves.

### ARM7 (`arm7/source/`)

- **`main.c`** -- ARM7 entry point. Initializes maxmod, installs the FIFO command and address handlers, registers the tick event callback after `mmIsInitialized` returns true (the order matters; see hardware_quirks). The main loop drains "pending" flags set by FIFO handlers in IRQ context.

### Shared headers (`include/`)

- **`mt_ipc.h`** -- FIFO command opcodes (`MT_CMD_PLAY`, `MT_CMD_STOP`, `MT_CMD_SET_SHARED`, etc.) and the encoding macros that pack a command type plus a 24-bit parameter into a single u32. ARM9 sends these via `fifoSendValue32`; ARM7 decodes them in `mt_ValueHandler`.
- **`mt_shared.h`** -- The `MT_SharedPatternState` struct that lives in main RAM and is read by ARM7 / written by ARM9 (for cells, mute mask, and active flag) and read by ARM9 / written by ARM7 (for `pos_state`). Also defines the `MT_POS_PACK`/`MT_POS_POSITION`/`MT_POS_ROW`/`MT_POS_TICK` macros for the packed playback position field.

### Patched maxmod (`lib/maxmod/`)

A submodule of maxmod with one targeted modification: when compiled with `MAXTRACKER_MODE`, `mmReadPattern` reads from a flat `MT_Cell` array supplied by ARM9 via the shared state struct, instead of decoding the RLE pattern stream embedded in the MAS file. This is what makes "edit a note, hear it on the next playback pass" work without any serialize/deserialize cycle. Everything else (mixing, envelopes, effect processing) is stock maxmod.

---

## 6. Immediate-mode UI

Every view file follows the same shape:

```c
void some_view_input(u32 keys_down, u32 keys_held);   // mutate cursor / song
void some_view_draw(u8 *top_fb, u8 *bot_fb);          // read state, write framebuffer
```

The draw function is called from a cold start every frame. It reads the global `cursor`, the global `song`, sometimes `playback_*` state, and writes pixels into the framebuffer. There is no "construct a widget tree" step, no "is this widget dirty" check, no "subscribe to a model change event". Whatever state is in the model right now is what gets drawn right now.

This is the **immediate-mode UI** pattern, the same approach used by Dear ImGui and similar libraries. It is the right choice for an embedded tracker because:

- It keeps state in one place. If a value changes, every view that displays it will pick up the new value on the next frame, automatically. There is no risk of a view holding a stale copy.
- It eliminates whole classes of bugs that come from object-graph-based UI: forgotten event subscriptions, leaked widget references, double-drawing, dangling pointers to deleted widgets.
- It maps cleanly onto the NDS framebuffer model. The NDS doesn't have a window system; you write into a bitmap and the LCD shows it. Immediate mode and bitmap framebuffers are a natural fit.

It is not a free choice. Three rules follow from it that contributors must respect:

**Views must not hold persistent mutable state.** If you need to remember something across frames (a scroll position, a "currently editing this field" flag, a temporary input buffer), that state must live somewhere globally accessible (the cursor, the song, a file-scope static in the view), not as a private field of a "view object". There are no view objects.

**Views must not modify the model from inside `_draw`.** The draw function should be a pure function of state to pixels. If you find yourself wanting to change `song` or `cursor` from within a draw function, that means the work belongs in the input handler. The reason isn't aesthetic: drawing and input run in the same frame, but a draw-time mutation will not be visible until the *next* frame's draw, which produces a one-frame visual lag that's hard to debug.

**Drawing must be fast enough to fit in a frame budget.** At ~60 Hz that's about 16 ms per frame, minus whatever ARM7 needs. For text-mode views with a few hundred glyphs the budget is generous, but if you ever add a heavy effect (waveform rendering, scope) make sure it can complete in time on real hardware, not just in melonDS. Rendering that misses VBlank causes visible tearing and unpredictable input lag.

---

## 7. Editor state

The `EditorCursor` struct in `arm9/source/ui/editor_state.h` is shared mutable state between the controller and every view. It contains:

- **Position fields**: `row`, `channel`, `column`, `ch_group`, `order_pos`. These describe where in the song the user currently is.
- **Mode flags**: `inside` (overview vs single-channel zoom), `follow` (cursor tracks playback), `playing` (visual hint, distinct from `playback_is_playing`), `selecting` (block selection mode).
- **Edit state**: `octave`, `semitone`, `instrument`, `step`. These describe what would happen if the user pressed an edit key.
- **Playback hints**: `play_row`, `play_order`. These are mirrors of ARM7's reported position, refreshed in `playback_update`.
- **Selection anchor**: `sel_start_row`, `sel_start_col`. The other corner of the current block selection.

This struct used to live inside `pattern_view.h`, which made `pattern_view` an unwilling state holder for the rest of the UI. It now has its own module so views can include `editor_state.h` directly without dragging in the pattern renderer. If you find yourself reaching into `pattern_view.h` from a sibling view, you should be reaching into `editor_state.h` instead.

The cursor is the only writable global the controller and views share, aside from the song itself. A new contributor's first instinct is sometimes to add a second editor-state global ("just for this screen"); resist that. If a piece of state is editor-wide (read by more than one screen, or it survives across screen transitions), put it on the cursor. If it's truly screen-local, make it a file-scope static inside the view's `.c` file.

---

## 8. Cross-CPU IPC contract

This is the most error-prone area in the project and the one that is hardest to test. Read this section carefully and refer back to it whenever you touch `playback.c`, `arm7/source/main.c`, `mt_ipc.h`, or `mt_shared.h`.

### What runs where

ARM9 runs the editor, the song model, the UI, file I/O, and the playback shim. ARM9 has a data cache.

ARM7 runs maxmod, the FIFO handlers, and the tick callback. ARM7 has no data cache.

Both CPUs share access to main RAM. The ARM9 CPU sees main RAM through two address ranges: a cached mirror at `0x02000000` and an uncached mirror at `0x02400000`. They are the same physical RAM. Reads from the cached range may return stale data; reads from the uncached range always show what's actually in RAM right now.

### How they communicate

There are two independent channels:

**FIFO messages** for events. ARM9 sends a 32-bit value (or an address) on the `FIFO_MT` channel; ARM7's FIFO handler runs in IRQ context and sets a flag. The flag is processed on the next iteration of ARM7's main loop. The same mechanism works in reverse (ARM7 -> ARM9) for tick and pattern-end notifications. The encoding is in `mt_ipc.h`: bits 31:24 are the command type, bits 23:0 are a parameter. For commands that need to pass a pointer, ARM9 sends a value command with `MT_CMD_SET_SHARED` or `MT_CMD_SET_MAS`, then sends the pointer with `fifoSendAddress`. ARM7's address handler reads the previously-stored "what kind of address am I expecting" flag and acts accordingly.

**Shared memory** for streaming state. The `MT_SharedPatternState` struct in `mt_shared.h` lives in main RAM. ARM9 allocates it (it's a static in `playback.c`) and passes its address to ARM7 once at init via `MT_CMD_SET_SHARED`. After that, both CPUs read and write fields directly. There is no IPC overhead for accessing the shared state; the cost is entirely in the cache management.

### The cache rules

ARM7 has no data cache, so its writes are visible on the bus immediately. No flushing required from ARM7's side.

ARM9 has a data cache, so its writes might sit in cache lines that haven't been flushed to main RAM. After ARM9 modifies the shared state, it must call `DC_FlushRange((void *)&shared_state, sizeof(shared_state))` to push those cache lines to main RAM where ARM7 will see them. This is the rule that's easiest to forget. If ARM7 starts playing back a pattern that ARM9 just edited and you don't hear the edit, suspect a missing flush.

ARM9 reads from the shared state through the **uncached mirror** to bypass its own data cache. The helper `get_uncached_shared()` in `playback.c` ORs `0x00400000` into the address to translate from cached to uncached. ARM9 reads ARM7's writes through this uncached pointer, so a cache invalidate is not required. If you add a new field that ARM9 reads from ARM7, read it through the uncached mirror, not directly.

### The atomicity rules

A single 32-bit aligned load or store on the NDS bus is atomic with respect to the other CPU. A multi-byte load or store made up of multiple bus accesses is not.

This matters for the playback position. ARM7 needs to publish three values per tick (position, row, tick) and ARM9 needs to read them all consistently. If ARM7 writes them as three separate u8 stores and ARM9 reads them as three separate u8 loads, ARM9 can see a torn state, for example a fresh tick paired with a stale row, for one frame.

To avoid this, position/row/tick are packed into a single `volatile u32 pos_state` in the shared struct, with `MT_POS_PACK` / `MT_POS_POSITION` / `MT_POS_ROW` / `MT_POS_TICK` macros to encode and decode. ARM7's tick callback writes `pos_state` in one 32-bit store; ARM9 reads it in one 32-bit load. The two are atomic with respect to each other.

The `playing` flag is its own u8. It's mostly written by ARM9 (at start/stop) and rarely written by ARM7 (only when the song reaches its end). A torn read between `playing` and `pos_state` is benign: if ARM9 sees `playing == 0` it doesn't care about the position fields, and if it sees a stale `playing == 1` it gets one extra frame of position display.

If you add a new field that needs to be read by one CPU and written by the other, the rules are: keep the field naturally aligned, keep it 32 bits or smaller, don't have both CPUs writing the same field, and on the writer side flush the cache (if writing from ARM9) or just write (if writing from ARM7). If you need to publish more than 32 bits atomically, use a sequence-counter pattern (writer increments before and after, reader retries if the counter changed) or pack the data into 32 bits like `pos_state` does.

### What runs in IRQ context

The FIFO handlers (`mt_ValueHandler` and `mt_AddressHandler` on ARM7) run in interrupt context. The same is true of `pb_fifo_value_handler` on ARM9. Code in IRQ context must not call functions that aren't reentrant (most libc), must not block, and must be quick.

The maxtracker convention for handling this is: the FIFO handler reads the message, sets a "pending" flag with the parameters captured into static volatile variables, and returns. The main loop on the same CPU drains those pending flags during normal execution. This is why ARM7's main loop is full of `if (pending_play) { ... pending_play = false; }` blocks. Do not put real work inside FIFO handlers.

The maxmod tick callback (`mt_EventCallback` on ARM7) also runs in tick context, which is similar to IRQ context for these purposes. The only thing it does is publish the playback position into shared memory and send a FIFO notification to ARM9. Both of those are quick and atomic.

### Debugging IPC

Bugs in this layer often don't reproduce in melonDS because melonDS is more forgiving about cache coherency than real hardware. If a feature works in the emulator but glitches on a flashcart, suspect an IPC issue first. The host test suite doesn't exercise the IPC path at all; it links a host stub for `playback.h` and runs everything single-CPU.

---

## 9. Pattern lifecycle hooks

A small but important pattern that exists because of section 8. When `song_alloc_pattern` is about to free an existing pattern and replace it with a new one, ARM7 might be reading that pattern's cells right now via the shared cells pointer. Freeing it underneath ARM7 would cause it to read garbage and play noise, or crash.

To prevent this, the song module doesn't include `playback.h` directly (which would be an upside-down dependency, the model knowing about its consumer). Instead, `song.h` declares a pair of callback registration functions:

```c
typedef void (*song_lifecycle_cb)(void);
void song_set_pattern_lifecycle(song_lifecycle_cb on_detach,
                                song_lifecycle_cb on_reattach);
```

`playback.c` registers itself in `playback_init()`:

```c
song_set_pattern_lifecycle(playback_detach_pattern,
                           playback_reattach_pattern);
```

When `song_alloc_pattern` is about to free a pattern, it calls the detach hook first, which sets `shared_state.cells = NULL` and flushes. Then the free is safe. After the new pattern is in place, it calls the reattach hook, which restores the cells pointer if playback is currently active. `playback_reattach_pattern` checks `pb_playing` itself and is a no-op when nothing is playing, so the song module never has to consult playback state.

This is also the reason `song.c` is host-testable in isolation: when there's no playback module linked, the hooks are NULL and the alloc/free paths skip them.

If you add another module that holds a pointer into pattern memory at runtime, for example a future "pattern visualization" subsystem, register it the same way.

---

## 9b. Cross-screen routing globals (Disk browser, modal keyboard)

Two patterns of cross-screen state coordination have settled into the
codebase. Both are conventions, not enforced by the type system; keep
them in sync if you change one side.

### Disk-browser routing

The disk screen used to be opened by a global SHIFT+START chord. As of
2026-04 it is opened only by on-screen action rows in PROJECT and SAMPLE
views. To make the cancel path return to the right place without
threading a "return screen" parameter through `screen_set_mode()`, two
globals in `main.c` carry the routing context:

```c
extern ScreenMode disk_return_screen;   // where B-at-root / SHIFT+DOWN go
extern u8         sample_load_target;   // 1-based sample slot for .wav route
```

Every disk-screen opener must set both before calling
`screen_set_mode(SCREEN_DISK)`:

| Opener | `disk_return_screen` | `sample_load_target` |
|--------|----------------------|----------------------|
| `project_view.c` `pv_open_load_browser()` | `SCREEN_PROJECT` | unchanged (0) |
| `sample_view.c` `sv_open_load_browser()` | `SCREEN_SAMPLE` | `sv.selected + 1` |

Every exit path must honor and clear them:

- `main.c` SCREEN_DISK case (B-at-root) -> `screen_set_mode(disk_return_screen);` then `sample_load_target = 0;`
- `navigation.c` SHIFT+DOWN from SCREEN_DISK -> same.
- After a successful `.wav` load with `sample_load_target > 0` the disk
  screen also auto-returns to `SCREEN_SAMPLE`.

If you add a new opener (say a future "load instrument" flow), follow
the same pattern: set both globals before the screen change, and clear
them on every exit you can reach.

### Modal keyboard (`text_input`)

`text_input` is a single-instance modal owned by whatever view called
`text_input_open()`. While `text_input_is_active()` returns true, the
caller is expected to forward input frames to `text_input_input()` and
draws to `text_input_draw()`, returning early from its own handlers
until the keyboard closes itself (OK or CANCEL).

This means the keyboard does NOT need its own `ScreenMode` value; it
piggybacks on whichever view opened it. A side benefit: the active
`current_screen` is preserved across the modal, so cancel-then-close
returns the user exactly where they were.

The trade-off is that any new view that wants to use the keyboard must
remember to add the forward block at the top of both its `_input` and
`_draw` functions. There's no compile-time enforcement; missing the
forward causes the keyboard to be drawn-but-not-input-fed (or
input-fed-but-not-drawn), which presents as a frozen modal. Watch for
this when adding the next consumer.

---

## 10. Things you must not do

These are the rules that aren't enforced by the compiler but will bite you eventually if you violate them.

**Do not write to shared state from ARM9 without flushing.** `DC_FlushRange((void *)&shared_state, sizeof(shared_state))` after every modification. If you forget, ARM7 will see stale data and you will get audible glitches that don't reproduce in melonDS.

**Do not read shared state from ARM9 through the cached pointer.** Use `get_uncached_shared()` or read the field through `(volatile u32 *)((u32)&shared_state.field | 0x00400000)`. The cache will lie to you.

**Do not write multi-byte fields from ARM7 expecting ARM9 to see them atomically.** The bus is atomic for naturally-aligned 32-bit accesses. Anything wider needs `pos_state`-style packing or a sequence counter.

**Do not put real work in a FIFO handler.** Set a pending flag and return. The main loop will drain it.

**Do not call `mmSetEventHandler` before `mmIsInitialized()` returns true on ARM7.** `mmInit7` overwrites the event handler with maxmod's internal forwarder, so any callback you set earlier will be silently replaced.

**Do not initialize maxmod before libfat.** `mmInit()` touches FIFO and DMA; if libfat isn't initialized first, SD card access breaks on real hardware. The order is `fatInitDefault()` then `mmInit()` then `playback_init()`.

**Do not write 8-bit values into VRAM.** NDS VRAM only accepts 16-bit and 32-bit writes. To write a single pixel into a 16-bit framebuffer, do a read-modify-write of the surrounding 16-bit word. The font renderer handles this for you; if you find yourself touching VRAM directly, look at how `font.c` does it.

**Do not modify song or cursor state from inside a `_draw` function.** Only the input handlers should mutate. Draw functions read.

**Do not include `pattern_view.h` from a sibling view just to access the cursor.** Include `editor_state.h` instead. `pattern_view.h` is for the renderer's public API.

**Do not give a view file persistent state in the form of "view objects".** There are no view objects. If a view needs state across frames, put it on the cursor (if it's editor-wide) or in a file-scope static (if it's truly screen-local).

**Do not call `playback_*` functions from within a `_draw`.** Reading `playback_get_mute_mask()` and similar query functions is fine; calling `playback_play`, `playback_stop`, or anything that mutates audio state is not.

**Do not set `MF_DVOL` on a `NOTE_OFF` or `NOTE_CUT` row.** This is a maxmod-specific bug we hit during MAS export; the flag means "default volume" and combining it with a note-off causes the channel to retrigger at full volume. The pattern serializer in `mas_write.c` handles this correctly; if you write code that sets MF flags, follow the same convention.

---

## 11. Where to extend

A few common change patterns and where to put the code.

**Adding a new screen.** Add an enum entry to `ScreenMode` in `screen.h`, create `arm9/source/ui/your_view.h/.c` with `your_view_input` and `your_view_draw` functions, add a case to the dispatch switch in `main.c`, and add navigation transitions (probably in `handle_shift` or wherever the new screen is reached from). The Makefile globs `source/ui/*.c` so the new file picks up automatically.

**Adding a new IPC command.** Add the opcode to `mt_ipc.h`, add a case to `mt_ValueHandler` in `arm7/source/main.c` that sets a pending flag, drain the pending flag in ARM7's main loop, and add a sender wrapper in `playback.c` (or wherever the ARM9 caller lives). Don't put real work in the handler.

**Adding a new shared state field.** Add it to `MT_SharedPatternState` in `mt_shared.h`. If only one CPU writes it, you don't need atomicity tricks. If both CPUs write it, redesign so only one does. If ARM9 writes it, flush after writing. If ARM9 reads it, read through `get_uncached_shared()`.

**Adding a new effect or instrument feature.** This is mostly a song-model change (`song.h`) plus serialization changes in `mas_write.c` and `mas_load.c`. The playback engine itself is stock maxmod; you only need to touch `lib/maxmod/source/core/mas.c` if you're adding an effect maxmod doesn't already implement, which is unlikely.

**Adding a host test.** Add a `MT_RUN(test_function)` call in `arm9/source/test/test.c`. The test must be pure-logic: no NDS hardware, no playback. If you need to stub something, look at `test/nds_shim.h` and `test/engine_stubs.c` for the existing pattern. See [DEVELOPING.md](DEVELOPING.md) for the test build commands.

**Adding a new file format reader.** Put it under `arm9/source/io/`. Follow the pattern from `wav_load.c`: the function takes a path and out-pointers, returns 0 or a negative error code, and never assumes the file is well-formed.

---

## 12. What this architecture deliberately does not have

A few things that a "proper" application architecture would have but maxtracker doesn't, and why:

**No event bus.** Input dispatch is direct: the input handler reads buttons, decides what changed, and calls the relevant function. There is no `EventBus.publish("note_inserted", ...)`. For a single-threaded ARM9 program with one input source, an event bus would add latency and indirection without benefit.

**No object orientation.** No `View` base class, no `EditorOperation` polymorphism. Each view is a `.c` file with a small public API. C makes object orientation expensive in syntax, and nothing in the design needs it.

**No dependency injection container.** Subsystems use globals (`song`, `cursor`, `shared_state`, `clipboard`, `note_clipboard`). For a single-instance embedded program where there will only ever be one editor running at a time, globals are the right answer; an injection framework would be ceremony.

**No widget hierarchy.** Immediate mode, see section 6.

**No threading.** ARM9 is single-threaded. The only "concurrency" is the IRQ handlers (which only set flags) and the other CPU (which has its own contract). No mutexes, no atomics in the C11 sense.

**No exceptions or longjmp-based unwinding.** Errors are return codes. Resource cleanup is explicit at each call site.

If you find yourself reaching for one of these, the project is probably trying to tell you that the change you're making belongs in a different layer, or that you're optimizing for a flexibility the project doesn't need.

---

## 13. Future restructuring

Two surgical fixes are queued for whenever the project becomes a proper repository:

**Splitting `main.c`.** It's 1500+ lines and conflates scene management, per-screen input handling, file I/O orchestration, and the autosave timer. The natural split is `scene_manager.c` (the dispatch switch and scene transitions), `input_router.c` (the per-screen handlers, including `handle_input_pattern` and the disk-screen logic), and a much smaller `main.c` that just initializes subsystems and runs the loop. The cost is high (many functions to relocate, plenty of opportunity to break input handling) and the benefit is medium (easier to navigate, easier to test individual handlers). It hasn't happened yet because there's no triggering reason.

**A UI/core facade.** Right now views read directly from `song`, `cursor`, and `playback_*` query functions. A facade would centralize those reads behind a smaller surface area, which would help if the project ever needs multiple "instances" of the editor (e.g. for testing, or for a hypothetical headless mode). It would not help if the project stays single-instance, which it currently is. Keep this in mind but don't build it preemptively.

A third item that's been considered and rejected: introducing a full event bus or message queue between subsystems. The current direct-call model is appropriate for the problem size and adding indirection would not pay for itself.

---

## 14. Glossary

A few terms used throughout the maxtracker codebase that might be unfamiliar.

- **Cell** -- One step in a pattern: `note + instrument + volume + effect + parameter`, 5 bytes.
- **Pattern** -- A 2D array of cells, one row per step and one column per channel. Patterns are variable-size and stored as `MT_Pattern` with a flexible array member.
- **Order** -- A sequence number in the song's playback list. The order table maps order positions to pattern indices, so the same pattern can be played multiple times.
- **Inside / Overview** -- The two display modes of the pattern view. Overview shows 8 channels at once with note+instrument per cell. Inside shows a single channel with all 5 fields per cell.
- **MAS** -- The maxmod binary module format. Files maxtracker saves and loads.
- **MAXTRACKER_MODE** -- The compile-time flag that activates the patched `mmReadPattern` in maxmod, switching it from RLE-stream reading to flat-cell reading.
- **MT_** -- Prefix for all maxtracker-defined types and constants (`MT_Cell`, `MT_Pattern`, `MT_Song`, `MT_MAX_CHANNELS`, etc.).
- **NOTE_EMPTY** -- Sentinel value (250) for "this cell has no note". Distinct from `NOTE_OFF` (255) and `NOTE_CUT` (254), both of which are real note events.
- **Cells pointer** -- The flat array of cells inside an `MT_Pattern`. ARM9 publishes a pointer to this in shared state for ARM7 to read.
- **Pos state** -- The packed u32 in shared state holding position, row, and tick for atomic cross-CPU access.
- **Lifecycle hooks** -- The detach/reattach callbacks the song module calls before freeing or replacing a pattern, registered by playback at init time.

---

See also: [hardware_quirks.md](hardware_quirks.md), [DEVELOPING.md](DEVELOPING.md), [conventions.md](conventions.md), [data_model.md](data_model.md), [audio_engine.md](audio_engine.md), [file_io.md](file_io.md), [DESIGN.md](../DESIGN.md).
