# maxtracker -- Developer Guide

Parent: [DESIGN.md](../DESIGN.md)

---

## 0. What this document is

The entry point for working on maxtracker. If you've just cloned the repository (or returned to it after a long absence), start here. This file tells you how to build and run the project, where to read next, and how to make common kinds of changes.

If you want to understand *why* the code is structured the way it is, read [architecture.md](architecture.md). If you're chasing a bug that only happens on real hardware, read [hardware_quirks.md](hardware_quirks.md). If you're worried you might be writing code in a style the project doesn't follow, read [conventions.md](conventions.md). The four files are designed to be read in any order, but if you're cold on the project, do them in this order: this file, then `architecture.md`, then `hardware_quirks.md`, then `conventions.md`.

---

## 1. What is maxtracker

A music tracker for the Nintendo DS, in the tradition of LSDJ, M8, and Piggy Tracker. It edits and plays maxmod `.mas` files natively. Songs are kept in memory as a flat editable model and serialized to `.mas` only on save, which means edits are audible immediately during playback without a re-encode. The audio engine is a patched maxmod running on the DS's ARM7, talking to the editor on ARM9 through shared memory.

The vision document is `DESIGN.md` in the repository root. Read its sections 1, 2, and 3 if you've never seen the project before; they cover the goals, the platform constraints, and the high-level architecture choice (the maxmod patch, the cross-CPU split, the LSDJ-style navigation model).

---

## 2. Prerequisites

You will need:

- **devkitPro / devkitARM** for cross-compiling to the NDS. Install via the standard devkitPro installer for your platform. Set the `DEVKITARM` environment variable to point at it (the project's top-level Makefile will refuse to build without this).
- **GNU Make**. Comes with devkitPro on Windows; preinstalled on Linux/macOS.
- **A host C compiler (`gcc`)** for the host-native test suite. On Windows the project has been built against MSYS2 mingw64 `gcc`; any reasonably current `gcc` or `clang` should work. The host tests do not need devkitARM.
- **no$gba** for emulated testing. This is the only supported emulator; it is the closest to real hardware behavior. See [hardware_quirks.md § 14](hardware_quirks.md).
- **A real DS or flashcart** for hardware verification. Even no$gba is more forgiving than real hardware in some areas; anything that touches audio, FIFO, or the SD card path needs to be tested on hardware before being declared done.

You do not need:

- A separate maxmod install. The patched maxmod source is included as a submodule under `lib/maxmod/`. The build system compiles it from source.
- A symbolic debugger. There is no production debugger for the running NDS that handles cross-CPU code well. The debugging conventions are in [hardware_quirks.md § 17](hardware_quirks.md): `iprintf` and shared-memory tracing.

---

## 3. Building

The top-level `Makefile` is the dispatcher. Run `make` with no arguments to see the help:

```
make emulator   - build .nds with data/ embedded via NitroFS (for emulators)
make native     - build .nds without embedded data (for flashcarts, songs on SD)
make test       - build emulator .nds that runs unit tests on boot
make host-test  - compile and run unit tests natively on host (no emulator)
make clean      - remove all build artifacts
```

In practice you'll use three of these:

**`make emulator`** for day-to-day development. Produces `release/maxtracker.nds` with the contents of `data/` embedded via NitroFS. Load it in no$gba and you're running. The data files (sample songs, default sounds) are baked into the ROM, so file loading works without an emulated SD card.

**`make native`** for testing on hardware. Produces `release/maxtracker.nds` *without* embedded data. Copy it to your flashcart along with a `data/` directory containing whatever sample songs you want. The build is otherwise identical to `emulator`.

**`make host-test`** for fast iteration on logic. Compiles the host test binary in `test/maxtracker_tests` and runs it. This is the fastest feedback loop in the project: under a second from edit to test result on a typical machine, no emulator required. Use this whenever you change `song.c`, `clipboard.c`, `undo.c`, `mas_load.c`, `mas_write.c`, `scale.c`, or `test.c`. See section 5 for more on the test suite.

A few build details that aren't obvious:

- `make clean` removes everything including the maxmod build artifacts. After a clean, the next build will recompile maxmod, which adds 30+ seconds. If you only want to rebuild maxtracker code, run `make -C arm9 clean && make -C arm7 clean && make emulator`.
- The `make test` target builds an NDS ROM that runs the unit tests on boot using `consoleDemoInit()`. It's the on-device counterpart to `host-test` and exists for testing things that can't run on the host (currently nothing; the ROM just prints "no tests yet"). Most testing should use `host-test`.
- The `EXTRA_CFLAGS="-DUNIT_TESTING"` flag is what activates the on-device test mode. If you ever want a one-off debug build, you can pass `EXTRA_CFLAGS=-DSOMETHING` to `make -C arm9` directly.
- After making cross-CPU IPC changes (anything in `mt_shared.h`, `playback.c`, `arm7/source/main.c`), you must rebuild *both* CPUs from clean, otherwise the two halves can disagree about the shared struct layout. `make clean && make emulator` is the safe option.

---

## 4. Running

### In no$gba

Load `release/maxtracker.nds` in no$gba. The first screen is the pattern editor. Pull up the controls reference (`doc/ui_ux.md` or just experiment) for navigation. The bottom screen shows context-sensitive hints.

no$gba is the closest emulator to real hardware and the only one currently supported. It catches most cache-coherency and FIFO timing issues that other emulators miss. **What no$gba will not catch** is documented in [hardware_quirks.md § 14](hardware_quirks.md).

### On hardware

Build with `make native` and copy `release/maxtracker.nds` to your flashcart's SD card. Create a `data/` directory next to it for songs and samples, or let maxtracker create the autosave directory on first run.

Hardware testing is mandatory after any change in the following areas:
- Anything in `arm7/source/`
- Anything in `arm9/source/core/playback.c`
- Anything in `include/mt_shared.h` or `include/mt_ipc.h`
- File I/O paths (`mas_load.c`, `mas_write.c`, `wav_load.c`, `filebrowser.c`)
- Any code that writes to VRAM directly

If your change is purely in the song model, clipboard, undo, or scale modules, host tests are sufficient.


---

## 5. The test suite

There are two test infrastructures in this project, and they cover different things.

### Host tests

Located in `test/`, built with `make host-test`. Compiles four binaries:

- **`maxtracker_tests`** -- The main unit test runner. Links the host shim layer (`nds_shim.h`, `memtrack_stub.c`) with the pure-logic modules (`song.c`, `clipboard.c`, `undo.c`, `scale.c`, `mas_write.c`, `mas_load.c`) and the test harness in `arm9/source/test/test.c`. Runs ~50 test groups covering song initialization, clipboard operations, undo ring behavior, MAS roundtrip, instrument-sample mapping, panning encoding, groove tables, and scale navigation. As of this writing it produces 666 assertions.
- **`mas_diff`** -- A roundtrip validation tool. Loads a `.mas` file, serializes it back, and byte-compares the result against the original. Used to catch any encoding regression in `mas_write.c` or decoding regression in `mas_load.c`.
- **`playback_cmp`** -- A tick-by-tick playback comparison tool. Compiles the maxmod engine on the host alongside `engine_stubs.c`, runs the same `.mas` file through both the original RLE pattern reader and the patched flat-cell reader, and asserts they produce identical engine state at every tick.
- **`reader_cmp`** -- Similar to `playback_cmp` but focused on the pattern reader's flag computation alone, not the full engine.

Run them with:

```sh
make host-test                          # builds and runs maxtracker_tests
make -C test playback_cmp               # builds playback_cmp
./test/playback_cmp data/sample.mas 5000 # run with specific file and tick count
make -C test run-playback-cmp           # runs playback_cmp against every .mas in data/BestOf
make -C test mas_diff                   # builds mas_diff
./test/mas_diff data/sample.mas         # compares roundtrip
```

The host test infrastructure is described in detail in [test_plan.md](test_plan.md). The shims that make ARM9 source compile on a host system are in `test/nds_shim.h` (provides `u8`/`u16`/`u32`, stubs FIFO and DMA), `test/mm_engine_shim.h` (provides maxmod engine globals when compiling the engine source), and `test/memtrack_stub.c` (provides a sentinel implementation of `mt_mem_estimate_mas` so the loader's memory check passes). If you add a new file to a host test target's source list, check whether it pulls in any new ARM-only headers and, if so, add the relevant stub to the shim.

### On-device tests

Located in `arm9/source/test/test.c`, built with `make test`. The same test functions that the host test runs are also runnable on the device when `UNIT_TESTING` is defined. Currently the device test build just prints a placeholder ("no tests yet") because all the existing tests are pure logic and run faster on the host. The on-device test mode exists for tests that genuinely need NDS hardware: VRAM rendering, FIFO timing, real audio output. None of those have been written yet.

### Adding a new host test

The simplest pattern: add a function to `arm9/source/test/test.c` and register it in the appropriate `MT_RUN(...)` block. The test functions use the `MT_ASSERT` and `MT_ASSERT_EQ` macros for assertions. Example:

```c
static void test_my_thing(void)
{
    MT_TEST_HEADER("My Thing");
    /* setup */
    int rc = something(...);
    MT_ASSERT_EQ(rc, 0, "something succeeds");
    MT_ASSERT(some_invariant, "invariant holds");
    /* teardown */
}
```

Then add `MT_RUN(test_my_thing);` near the bottom of `mt_test_run_all()`. The next `make host-test` will compile and run it.

Tests should be deterministic, fast (under 10 ms each), and self-contained (set up state, exercise behavior, tear down). Avoid tests that depend on the order of other tests, on the file system, or on any external state. When you need to test loading or saving, the test shims handle the relevant FIFO/cache stubs but you still write to a real path; use the `TEST_MAS_PATH_*` constants in `test.c` so paths are consistent.

If your test doesn't fit cleanly into the existing harness (say, you need a more complex differential check between two implementations), model it on `playback_cmp.c` or `mas_diff.c` rather than shoehorning it into `test.c`. Add a new target to `test/Makefile` for it.

---

## 6. Where to read first

Once you've built the project and run it, the recommended reading order for understanding the code is:

1. **`DESIGN.md` § 3 (Architecture Overview)** -- the ASCII diagram and the "Option B (patched maxmod)" decision. This gives you the cross-CPU model in 200 words.
2. **`doc/architecture.md`** -- the full code-level architecture. Read sections 1 through 8 carefully; sections 9 through 14 are reference material you can skim.
3. **`arm9/source/core/main.c`** -- the entry point. Read `main()` first (around line 1189) for the init sequence, then the frame loop (around line 1273) for the dispatch shape. Don't try to read the input handlers yet; they're 800+ lines and don't make sense until you know the data model.
4. **`arm9/source/core/song.h`** -- the data model. Skim the type definitions (`MT_Cell`, `MT_Pattern`, `MT_Instrument`, `MT_Sample`, `MT_Song`).
5. **`arm9/source/ui/editor_state.h`** -- the editor cursor. This is the second piece of state every view reads.
6. **`include/mt_shared.h` and `include/mt_ipc.h`** -- the cross-CPU contract.
7. **`arm9/source/core/playback.c`** -- ARM9's audio shim. The interesting functions are `playback_play`, `playback_stop`, `playback_update`, `playback_detach_pattern`, `playback_reattach_pattern`, and `get_uncached_shared`.
8. **`arm7/source/main.c`** -- ARM7's entire ARM7-side code. It's small. Read `mt_EventCallback`, the FIFO handlers, and the main loop.
9. **`doc/hardware_quirks.md`** -- at this point the rules will make sense.

Then pick one feature you want to add or one bug you want to chase, and let curiosity drive the rest.

---

## 7. How to make common changes

A few recipes for things that come up often. These are not exhaustive; they're starting points so you don't have to reverse-engineer the convention from scratch.

### Adding a new screen

A new top-level UI mode (like `SCREEN_INSTRUMENT`, `SCREEN_MIXER`, etc.) is about a half day of work.

1. Add an enum entry to `ScreenMode` in `arm9/source/ui/screen.h`. Put it before `SCREEN_COUNT`.
2. Create `arm9/source/ui/your_view.h` with prototypes for `your_view_input(u32 keys_down, u32 keys_held)` and `your_view_draw(u8 *top_fb, u8 *bot_fb)`. Follow the existing view headers for naming.
3. Create `arm9/source/ui/your_view.c`. Include `editor_state.h` if you need the cursor, `song.h` if you need the model, `screen.h` and `font.h` for rendering primitives. Implement the input handler to mutate cursor/song based on buttons. Implement the draw function as a pure read-only function that builds the framebuffer from current state. **If your view mutates `song.*` state, set `song_modified = true; autosave_dirty = true;` at every mutation site** because autosave depends on it, and missed sites silently lose work. Both flags are externs from `main.c`. (A `mt_song_modified()` helper is on the post-v1 list to make this structurally enforceable.)
4. Add a case to the dispatch switch in `arm9/source/core/main.c` `main()`'s frame loop. Follow the existing pattern: call `navigation_handle_shift(kd, kh)` first (returns true if SELECT-modifier handled the input), then your `your_view_input`, then your `your_view_draw`. Always guard the draw with `if (current_screen == SCREEN_X)` because the input handler may have transitioned away from your screen, and drawing stale view content over the new screen is a classic regression here.
5. Add navigation transitions wherever the new screen should be reachable from. Most screens are reached via `SELECT + direction` combos handled in `navigation.c`. If the new screen is reached from a specific other screen via an on-screen action (the modern pattern; see PROJECT and SAMPLE views), add the transition in that view's input handler.
6. Build with `make emulator` and test. The Makefile globs `source/ui/*.c` so you don't need to edit it.

**Font-mode safety**: SMALL is 64x32, BIG is 42x24. Anchor any near-footer content to `font_scale_row(30)` (help) and `font_scale_row(31)` (transport), and never compute rows as `font_scale_row(N) + K` with an unscaled offset, which drifts into the footer strip in BIG mode. See `memory/feedback_scaled_offset_drift.md` for the bug class and `sample_view.c`'s action-row anchoring for the fix pattern.

The view should not have its own `_init()` function; immediate-mode views don't have setup. If you find yourself wanting one, the state belongs on the cursor or in a file-scope static.

### Adding a string-entry field (reuse the on-screen QWERTY)

Don't roll a custom text editor. Use the `text_input` modal at `arm9/source/ui/text_input.{h,c}`. It handles the whole keyboard, snapshot-on-open, and CANCEL restore for you.

1. `#include "text_input.h"` in your view.
2. At the top of your `_input` function, before any of your own logic:
   ```c
   if (text_input_is_active()) {
       text_input_input(down, held);
       return;
   }
   ```
3. At the top of your `_draw` function, mirror it:
   ```c
   if (text_input_is_active()) {
       text_input_draw(top_fb, bot_fb);
       return;
   }
   ```
4. Open the keyboard from wherever the user requests an edit:
   ```c
   text_input_open(target_buf, max_len, "Title shown in header");
   ```
   `target_buf` must be writable for `max_len + 1` bytes (the +1 is the NUL). The widget edits in place; CANCEL restores the snapshot it took at open.

Steps 2 and 3 are easy to forget. Missing the input forward presents as a frozen modal (you can see the keyboard but can't type); missing the draw forward presents as the keyboard appearing only for one frame. If you observe either, check both forwards.

For destructive action prompts (delete / overwrite / discard), don't open a modal. Use the in-row two-tap confirm pattern from `instrument_view.c` and `sample_view.c` (a `*_confirm_pending` bool, prompt rendered on the transport row, second tap commits, B or any unrelated key cancels).

### Opening the disk browser from a new view

The disk browser is reached only via on-screen actions; the SHIFT+START shortcut was retired. To open it:

1. Set the routing globals first (both extern from `main.c`):
   ```c
   extern ScreenMode disk_return_screen;   // where exit goes
   extern u8         sample_load_target;   // 1-based slot for .wav route
   ```
2. Initialize the browser and switch screen:
   ```c
   filebrowser_init(&disk_browser, fs_browse_root);
   disk_browser_inited = true;
   disk_return_screen = SCREEN_YOUR_VIEW;
   sample_load_target = 0;          // or sv.selected + 1 for sample LOAD
   screen_set_mode(SCREEN_DISK);
   font_clear(top_fb, PAL_BG);
   font_clear(bot_fb, PAL_BG);
   ```
3. Don't forget to clear `sample_load_target` after the load (the disk-screen `.wav` branch already does, but if you add a new file-type branch, clear it there too). Otherwise a later disk entry from a different view inherits the redirect and routes to the wrong slot.

See `pv_open_load_browser` in `project_view.c` and `sv_open_load_browser` in `sample_view.c` for the canonical examples. The full convention is documented in `architecture.md § 9b`.

### Adding a new IPC command (ARM9 -> ARM7)

1. Add the opcode to `include/mt_ipc.h`. Pick a free value in the ARM9->ARM7 range (currently `0x01` through `0x0B` are taken).
2. Add a handler case in `arm7/source/main.c` `mt_ValueHandler`. The handler should set a static volatile flag and capture any parameters into static volatile variables. **Do not do real work in the handler**; see [hardware_quirks.md § 7](hardware_quirks.md).
3. Add code to drain the flag in ARM7's main loop. Look at how `pending_play`, `pending_stop`, and `pending_preview` are handled.
4. Add a sender wrapper in `arm9/source/core/playback.c` (or wherever the ARM9 caller lives). The sender does:
   ```c
   fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_YOUR_THING, your_param));
   ```
5. If your command needs to pass a pointer (more than 24 bits of parameter), use the two-step pattern: send a value command marking the address type, then send the address with `fifoSendAddress`. See `MT_CMD_SET_SHARED` and `MT_CMD_SET_MAS` for the existing convention.
6. Test on hardware as well as in no$gba. FIFO bugs may not reproduce even in no$gba.

### Adding a new shared state field

1. Add the field to `MT_SharedPatternState` in `include/mt_shared.h`. Mark it `volatile`.
2. Decide who writes it. If only ARM9 writes, no atomicity tricks needed; just `DC_FlushRange` after writing. If only ARM7 writes, no flushing needed (ARM7 has no cache). If both write, redesign so only one does, or use a sequence-counter pattern.
3. If ARM9 reads it, route the read through `get_uncached_shared()` (defined in `playback.c`). Don't read it through the cached `&shared_state` pointer.
4. If the field is multi-byte and needs to be consistent across writes, pack it into a u32 and access with bit-shift macros. See `MT_POS_PACK` / `MT_POS_POSITION` / etc. for the pattern.
5. Rebuild *both* CPUs from clean (`make clean && make emulator`). The struct layout has changed and stale `.o` files will produce silent corruption.

### Adding a new effect parameter or instrument feature

Most effect work is in the song model and serializers. The audio engine itself is stock maxmod and you only need to touch `lib/maxmod/source/core/mas.c` if you're adding an effect maxmod doesn't already implement, which is unlikely.

1. Add the field to the relevant struct in `arm9/source/core/song.h` (`MT_Instrument`, `MT_Sample`, `MT_Cell`, etc.).
2. Initialize the field in `song_init()` if needed.
3. Update `arm9/source/io/mas_write.c` to serialize the field. Match mmutil's encoding exactly; see [hardware_quirks.md § 8](hardware_quirks.md). The host test `mas_diff` will catch any mismatch.
4. Update `arm9/source/io/mas_load.c` to deserialize the field. The two sides must use the same field order, the same byte width, and the same endianness convention.
5. Update the instrument view (`instrument_view.c`) or pattern view (`pattern_view.c`) to expose the new field for editing.
6. Add a host test that exercises the field: set it, save, load, check it matches.

### Adding a new file format

Put it in `arm9/source/io/` next to `wav_load.c`. Follow the existing pattern: a function that takes a path and out-pointers, returns 0 on success or a negative error code, and never trusts the file's contents (always bounds-check offsets, always validate magic numbers).

```c
int your_loader(const char *path,
                u8 **out_data, u32 *out_len, /* etc */)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    /* validate, allocate, parse, return */
    fclose(f);
    return 0;
}
```

If the format is large (megabytes), consider streaming rather than loading the whole file into RAM. The DS only has 4 MB total and `malloc` failures are real.

---

## 8. Debugging

There is no symbolic debugger that handles the cross-CPU NDS architecture well. The debugging tools you have are:

**`iprintf`** -- Print to a console. Requires `consoleDemoInit()` mode, which conflicts with the bitmap framebuffer the editor normally uses. Useful for one-off debugging when you're already in a state where you can swap out the UI temporarily (or for a build like `make test` that runs in console mode anyway).

**Shared-memory tracing** -- Add a logging field (a counter, a most-recent-value, etc.) to `MT_SharedPatternState`, write it from ARM7 in the place you want to observe, read it from ARM9 through the uncached mirror in the next frame, and display the value somewhere on screen. This is the standard technique for ARM7-side debugging because there's no other reliable way to get a value out of ARM7 in real time.

**Host tests** -- For any logic that doesn't depend on hardware, write a host test instead of debugging on-device. The feedback loop is 100x faster.

**`playback_cmp` and `mas_diff`** -- For audio engine and serialization bugs, these tools tell you exactly which tick or byte diverges, which is much faster than guessing.

**no$gba debugger** -- no$gba has a built-in debugger with breakpoints, memory inspection, and I/O register views. It is the closest emulator to real hardware and the only one currently supported. Use it as the first line of defense before going to a flashcart.

The general debugging workflow for a hardware-only bug is:

1. Reproduce in no$gba first to make sure you have a clean repro (most bugs reproduce there).
2. If it doesn't reproduce in no$gba, you're in real-hardware-only territory. Add a logging field to shared state, instrument the suspected code path, capture the symptom on hardware, read the log on the next no$gba run.
4. Form a hypothesis. Check the hypothesis against [hardware_quirks.md](hardware_quirks.md); most cross-CPU bugs are one of those rules being violated.
5. Fix it, rebuild *both* CPUs from clean, retest on hardware.

---

## 9. Source organization

A quick orientation. For a per-file reference, see `module_reference.md` (planned in a later doc pass). For the architectural view, see [architecture.md § 5](architecture.md).

```
maxtracker/
├── DESIGN.md                  # high-level vision
├── Makefile                   # top-level dispatcher
├── doc/                       # design + developer documentation
│   ├── architecture.md        # code-level architecture (read this)
│   ├── hardware_quirks.md     # NDS-specific rules (read this when debugging)
│   ├── DEVELOPING.md          # this file
│   ├── conventions.md         # coding style and conventions
│   ├── data_model.md          # song struct reference
│   ├── audio_engine.md        # maxmod patch and IPC
│   ├── file_io.md             # MAS format reference
│   ├── ui_ux.md               # navigation and button mappings
│   ├── screen_layout.md       # visual layouts
│   └── test_plan.md           # test plan
├── arm7/source/
│   └── main.c                 # ARM7 audio + IPC handlers
├── arm9/source/
│   ├── core/                  # model + audio shim + utilities
│   │   ├── main.c             # entry point and frame loop
│   │   ├── song.[ch]          # song model
│   │   ├── playback.[ch]      # ARM9 audio shim
│   │   ├── clipboard.[ch]     # block + single-slot clipboards
│   │   ├── undo.[ch]          # undo ring
│   │   ├── scale.[ch]         # music theory utilities
│   │   ├── memtrack.[ch]      # memory estimation
│   │   └── util.h             # small helpers
│   ├── io/
│   │   ├── mas_write.[ch]     # MAS serialization
│   │   ├── mas_load.[ch]      # MAS deserialization
│   │   ├── wav_load.[ch]      # WAV loader
│   │   └── filebrowser.[ch]   # generic file picker
│   ├── ui/
│   │   ├── screen.[ch]        # framebuffer + scene state
│   │   ├── font.[ch]          # bitmap font renderer
│   │   ├── editor_state.[ch]  # global EditorCursor
│   │   ├── pattern_view.[ch]  # pattern grid renderer
│   │   ├── instrument_view.[ch]
│   │   ├── sample_view.[ch]
│   │   ├── mixer_view.[ch]
│   │   ├── project_view.[ch]
│   │   └── draw_util.h
│   └── test/
│       └── test.[ch]          # unit tests
├── include/
│   ├── mt_ipc.h               # cross-CPU FIFO opcodes
│   └── mt_shared.h            # cross-CPU shared state struct
├── lib/maxmod/                # patched maxmod submodule
└── test/                      # host-native test suite
    ├── Makefile               # host build
    ├── host_main.c            # entry point for maxtracker_tests
    ├── nds_shim.h             # type and function stubs for host build
    ├── mm_engine_shim.h       # maxmod engine shim for playback_cmp
    ├── memtrack_stub.c        # stub for mt_mem_estimate_mas
    ├── engine_stubs.c         # stubs for the engine-linking targets
    ├── mas_diff.c             # MAS roundtrip tool
    ├── playback_cmp.c         # tick-by-tick engine comparison
    └── reader_cmp.c           # pattern reader flag comparison
```

The `data/` directory at the repository root holds sample songs and is embedded into the ROM by `make emulator`. If it doesn't exist, create it before building.

The `release/` directory is created by the build system and holds the output `.nds` file. Don't commit it.

---

## 10. Conventions you should know about before writing code

A short list. The full set is in [conventions.md](conventions.md).

- **Default to no comments.** Comments explain *why*, not *what*. A well-named function and a clean signature should make the *what* obvious. If you find yourself writing a comment that explains what the next line does, the line probably needs a better name instead.
- **No comments that reference the current task or PR.** "Added for issue #42" or "used by the new sample editor" rots immediately. The PR description and the commit message are the right place for that.
- **Trust internal calls; validate at boundaries.** If a function is called only from other functions in the same module, you can rely on the caller's invariants. If a function takes input from the user, the SD card, or ARM7, validate aggressively. The boundary is where the trust changes.
- **No premature abstraction.** Three similar lines is not yet a pattern. Don't introduce a helper or a struct or a typedef "in case we need it later". Add the abstraction when the third caller actually exists.
- **Match the pattern of nearby code.** If a file uses one convention for naming, brace style, or error handling, follow it. Mixing conventions makes diffs harder to read.
- **Avoid emojis in source files.** Don't add them unless the user explicitly asks.
- **The user handles builds.** Don't run `make` from agent code unless asked. The user is at the keyboard and will tell you when to build.

---

## 11. When you're stuck

In rough priority order:

1. **Read [hardware_quirks.md](hardware_quirks.md).** If your bug only reproduces on hardware, the answer is almost certainly there.
2. **Check the host tests.** If your change broke a host test, the test name will point at the affected subsystem. Run `make host-test` after every meaningful change so you catch breakage early.
3. **Read the relevant module's source.** With ~7000 lines of code total, the project is small enough to read end-to-end in an afternoon. Don't be afraid to grep around.
4. **Compare against `MAXMXDS`.** If your bug is in audio output, the sibling project `C:\Projects\MAXMXDS` is a working reference using stock maxmod. If MAXMXDS plays a file correctly that maxtracker doesn't, the bug is in our patch or our IPC.
5. **Compare against `mmutil`.** If your bug is in serialization, the sibling project `C:\Projects\mmutil` is the canonical reference. The host test `mas_diff` already validates roundtrips against mmutil-produced files.
6. **Check the auto-memory file.** The `.claude/projects/.../memory/MEMORY.md` file in the user's home directory holds a record of past hard-won lessons about this project. They're also captured in `hardware_quirks.md` but the memory file may have additional context.

---

## 12. What's not done yet

A few things on the project's future-work list, captured here so you don't reinvent them.

- **Splitting `main.c`** -- currently 1500+ lines. Will become `scene_manager.c` + `input_router.c` + a much smaller `main.c` once the project becomes a proper repository. See [architecture.md § 13](architecture.md).
- **A UI/core facade** -- UI views currently call into `playback.c` and read `song.*` directly. A facade layer would centralize these calls. Not urgent at the current size; would help if multiple editor instances ever become a thing (e.g. for headless testing).
- **Per-file documentation** -- `doc/module_reference.md` is planned for a future doc pass. The high-level architecture is documented here and in [architecture.md](architecture.md), but per-file detail is not.
- **Effect-encoding test coverage** -- `mas_diff` catches roundtrip mismatches but doesn't check that specific effects produce specific audio results. A targeted test would help.
- **Hardware test on-device suite** -- `make test` builds an on-device test runner that currently has no tests in it. VRAM rendering, FIFO timing, and audio output are the obvious candidates.

---

See also: [architecture.md](architecture.md), [hardware_quirks.md](hardware_quirks.md), [conventions.md](conventions.md), [DESIGN.md](../DESIGN.md), [data_model.md](data_model.md), [audio_engine.md](audio_engine.md), [file_io.md](file_io.md), [test_plan.md](test_plan.md).
