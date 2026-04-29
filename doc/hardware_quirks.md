# maxtracker -- Hardware Quirks and NDS-Specific Rules

Parent: [DESIGN.md](../DESIGN.md)

---

## 0. What this document is

A reference for the gotchas that come from running on real Nintendo DS hardware. None of these are obvious from reading C source, and most of them either don't reproduce in melonDS or only fail intermittently. They are the kind of bug you can spend an afternoon chasing before realizing the answer is "the bus doesn't work the way you assumed."

Each entry has the same shape:

- **The rule**, in one sentence.
- **Why** the rule exists, with enough hardware context to make it stick.
- **How to detect violation**, so you can recognize the symptom when you see it again.
- **Where it shows up in the code**, when there's a specific function or file that has to honor it.

If you're reading code in `playback.c`, `arm7/source/main.c`, `mt_shared.h`, or anywhere that touches VRAM or the FAT filesystem, you should have already read [architecture.md § 8](architecture.md) and this document.

---

## 1. VRAM only accepts 16-bit and 32-bit writes

**Rule:** Never write a single byte to VRAM. Use 16-bit or 32-bit accesses only.

**Why:** The NDS VRAM banks are wired to a bus that decodes 16-bit and 32-bit transactions. An 8-bit write to VRAM is silently dropped — the bus completes the cycle but no bits change. There's no exception, no warning, and no fault. The pixel just doesn't update.

This is a property of every NDS framebuffer, every background tile map, every OAM entry, and every palette entry. It is not specific to one display mode. Reading from VRAM with 8-bit accesses *does* return the correct value, which makes the asymmetry doubly confusing.

**How to detect violation:** A pixel that should update doesn't. The framebuffer state in your debugger looks correct because the C-level pointer arithmetic computed the right address, but the LCD shows a stale value. If you can tell that "every other pixel in this row got drawn correctly" you're looking at this bug.

**Where it shows up:** The font renderer in `arm9/source/ui/font.c` does its own read-modify-write to handle this safely. Anywhere you find yourself wanting to do `framebuffer[x + y * stride] = color;` in 8bpp mode, instead read the surrounding 16-bit word, mask in the new byte, and write the word back. Look at how `font_putc` does it.

---

## 2. Initialize libfat before maxmod

**Rule:** Call `fatInitDefault()` before `mmInit()`. The order matters.

**Why:** Both libfat and maxmod use the FIFO and DMA subsystems. Libfat's initialization needs to claim certain hardware resources and set up its IRQ handlers. If maxmod gets there first, the FIFO state is already in a configuration that prevents libfat's SD driver from talking to the cartridge slot. The result is that `fatInitDefault()` returns false on real hardware (works fine in melonDS, which doesn't simulate the resource conflict), and the user can't load or save files.

This rule is specific to the patched maxmod build that maxtracker uses. Stock maxmod might tolerate the wrong order; we have not tested it.

**How to detect violation:** On real hardware (flashcart), file loading fails immediately at startup. The disk screen shows an empty file list. Autosave reports errors. In melonDS everything works.

**Where it shows up:** `arm9/source/core/main.c` `main()` does `fatInitDefault()` and then `mmInit()`. Don't reorder them. If you ever add a third subsystem that touches FIFO at init, add it after `mmInit()` unless you've verified it doesn't conflict.

```c
if (fatInitDefault()) {
    using_nitrofs = false;
    fs_browse_root = "./data/";
} else if (nitroFSInit(NULL)) {
    using_nitrofs = true;
    fs_browse_root = "./";
}
mmInit(&sys);
mmSelectMode(MM_MODE_C);
```

---

## 3. Install the maxmod event handler after `mmIsInitialized` returns true

**Rule:** On ARM7, do not call `mmSetEventHandler` until `mmIsInitialized()` returns true.

**Why:** Maxmod's ARM7 initialization runs asynchronously. ARM9 calls `mmInit()`, which sends a FIFO message to ARM7, which eventually runs `mmInit7()` over there. Inside `mmInit7`, maxmod overwrites the user-installed event handler with its own internal forwarder (`mmEventForwarder`). If you set your callback before `mmInit7` runs, it gets clobbered and your tick handler is never called.

The symptom is silent: `mmSetEventHandler` succeeds, the function pointer is stored, then later it gets replaced and you never know. There is no error path.

**How to detect violation:** Playback starts, the song plays audibly, but ARM9 never receives tick callbacks. The playback position display on screen freezes at 0/0. `cursor.play_row` never advances. `playback_update()` reads stale `pos_state` every frame.

**Where it shows up:** `arm7/source/main.c`'s `main()` busy-waits on `mmIsInitialized()` before installing the callback:

```c
while (!mmIsInitialized())
    swiWaitForVBlank();
mmSetEventHandler(mt_EventCallback);
```

If you add another callback installation in the future, do it in the same place. Don't try to install it earlier just because "we already have the function pointer".

---

## 4. Pattern transition uses ARM7-resolved cells pointer, not FIFO

**Rule:** When playback advances to a new pattern, ARM7 looks up the new cells pointer from the shared pattern table itself, not by waiting for ARM9 to send it via FIFO.

**Why:** This is a defensive design that earlier versions of the code got wrong. The naive approach is: ARM7 reaches the end of pattern N, sends a FIFO message to ARM9, ARM9 looks up pattern N+1's cells pointer and writes it into shared state, then sends the address back. This has a race: ARM7 reads the cells pointer once per row, and the FIFO round-trip can take multiple rows on real hardware. ARM7 ends up reading the old (now-freed or -repurposed) cells pointer for some number of rows after the transition.

The fix was to put the entire pattern table directly into `MT_SharedPatternState`. ARM9 populates it once at playback start (an array of `MT_PatternEntry { cells, nrows }` indexed by pattern index, plus the order table). ARM7 then walks it itself when the order position changes — no FIFO needed for the transition. ARM7 still sends `MT_CMD_PATTERN_END` for ARM9's display update, but the actual cells lookup has already happened locally.

**How to detect violation:** Playback at a pattern boundary is glitchy: notes from the wrong pattern, audible clicks, or a brief silence. Reproduces on real hardware but is much less obvious in melonDS because the FIFO latency there is artificially low.

**Where it shows up:** The `patterns[]` and `orders[]` arrays in `MT_SharedPatternState` (`include/mt_shared.h`) exist for this reason. ARM7's tick callback in `arm7/source/main.c` indexes them when `pos != last_reported_pos`. If you ever consider "simplifying" this by removing the shared tables and going back to FIFO-driven transitions, don't.

---

## 5. ARM7 has no data cache

**Rule:** ARM7 writes to main RAM are visible immediately. ARM9 writes to main RAM may sit in cache lines until you flush them.

**Why:** The DS has an asymmetric memory architecture. ARM9 has a 4KB data cache; ARM7 doesn't. When ARM7 writes to main RAM the value goes straight to RAM, and when ARM9 reads from main RAM through the cached pointer it might get a stale value from its own cache instead of the live value in RAM.

This rule has two practical consequences. First, after ARM9 modifies anything in shared memory, it must call `DC_FlushRange((void *)&shared_state, sizeof(shared_state))` to push the cache lines out. Second, when ARM9 wants to read something ARM7 just wrote, it must read through the *uncached mirror* at `0x02400000`, which bypasses the data cache entirely.

The uncached mirror is the same physical RAM viewed through a different address range. There is no separate copy. The mirror trick works because the cache is address-range-keyed: ARM9 only caches addresses in the cached range. Reads through the uncached range hit RAM directly.

**How to detect violation:** Edit a note in the editor while playback is running. If the next pass plays the *old* note instead of the new one, suspect a missing flush. Conversely, if the playback position display freezes or jumps backward, suspect a cached read that should have been uncached.

**Where it shows up:** `arm9/source/core/playback.c` has `get_uncached_shared()`, used by `playback_update()` to read `pos_state` and `playing`. Every function that mutates `shared_state` (`playback_play`, `playback_stop`, `playback_detach_pattern`, `playback_reattach_pattern`, `playback_set_mute`, `playback_refresh_shared_tables`, etc.) calls `DC_FlushRange` before returning. If you add a new mutator, follow the same pattern. If you add a new reader, route it through `get_uncached_shared()`.

---

## 6. Multi-byte cross-CPU writes are not atomic

**Rule:** Only naturally-aligned 32-bit-or-smaller writes are atomic with respect to the other CPU. If you need to publish more than that consistently, pack it.

**Why:** The bus completes one transaction at a time. A 32-bit aligned access is one transaction. A 64-bit access is two transactions and the other CPU can interpose between them. Three separate u8 stores are three transactions and the other CPU can read between any of them.

Maxtracker hit this with the playback position. ARM7 used to write `position`, `row`, and `tick` as three separate u8 stores in its tick callback, and ARM9 used to read them as three separate u8 loads in `playback_update`. Most of the time this worked because ARM7 wrote them in order and ARM9 read them in order; but occasionally ARM9 would land in the middle of ARM7's write and see "new tick + old row", which displayed as a one-frame position jitter.

The fix was to pack `position`, `row`, and `tick` into a single `volatile u32 pos_state`, with `MT_POS_PACK` / `MT_POS_POSITION` / `MT_POS_ROW` / `MT_POS_TICK` macros for encode and decode. ARM7's callback writes `pos_state` in one bus cycle; ARM9 reads it in one bus cycle. They're now atomic.

**How to detect violation:** Playback display shows occasional one-frame glitches: the row counter blinks back, or the tick value resets just as the row changes. Hard to catch in person, easy to spot by capturing video and stepping through frames.

**Where it shows up:** `include/mt_shared.h` defines `pos_state` and the pack/unpack macros. ARM7 writes it in `mt_EventCallback`. ARM9 reads it in `playback_update`. If you add another field that needs cross-CPU atomicity, follow the same packing approach. If you genuinely need more than 32 bits, use a sequence-counter pattern: writer increments a `seqlock` u32 before and after the write, reader retries if the count changed.

---

## 7. Do not put real work in a FIFO handler

**Rule:** FIFO callbacks run in interrupt context. They must do as little as possible — typically just storing a parameter into a static volatile variable and setting a "pending" flag.

**Why:** Interrupt context is restrictive. Most of libc is not reentrant, so calling printf, malloc, or any standard library function risks corrupting state used by the main thread. Anything that blocks (waiting on FIFO, on disk, on a timer) prevents the interrupted code from making progress and can deadlock. And the longer your handler runs, the more interrupts you delay; a slow FIFO handler can starve VBlank, audio mixing, or input scanning.

**How to detect violation:** Symptoms vary widely. Audio glitches (mixer fell behind because IRQs were delayed). Input loss (button polling missed a frame). Random crashes (libc state corrupted). Heisenbugs that move when you change unrelated code.

**Where it shows up:** ARM7's `mt_ValueHandler` and `mt_AddressHandler` in `arm7/source/main.c` follow this rule strictly:

```c
case MT_CMD_PLAY:
    pending_play_pos = (u8)param;
    pending_play = true;
    break;
```

The actual work (`mmStop()`, `mmPlayMAS(...)`, etc.) happens later in the main loop:

```c
if (pending_play) {
    pending_play = false;
    /* ... real work here ... */
}
```

ARM9's `pb_fifo_value_handler` follows the same convention. Don't break it. If you add a new IPC command, the handler should set a flag, and a separate piece of code should drain the flag.

---

## 8. DS sample header encoding must match mmutil exactly

**Rule:** When serializing a sample to MAS, the `default_frequency`, loop fields, and length-in-words units must match mmutil's `Write_Sample` byte-for-byte.

**Why:** The MAS format describes sample headers in a maxmod-specific encoding that isn't fully documented anywhere except in mmutil itself. The frequency field is in a Q-format conversion of Hz that requires a specific multiply-and-shift; the loop start and length are in word units (not bytes); the length itself includes a specific +1 / -1 convention depending on whether the sample loops.

If any of these is encoded slightly wrong, the sample plays at the wrong pitch, loops at the wrong point, or doesn't loop at all. Worse, the bytes look "almost right" so you'll spend hours comparing them to a reference file before you spot the off-by-one.

**How to detect violation:** A sample plays at the wrong pitch after save-and-reload, or the loop point shifts by a few samples each loop, or playback runs off the end of the sample into garbage memory. The host test `mas_diff` will catch this immediately because it does a byte-for-byte roundtrip check, which is the main reason that test exists.

**Where it shows up:** `arm9/source/io/mas_write.c` `Write_Sample` (or whichever function emits sample headers — search for `default_frequency`). `C:\Projects\mmutil` is the canonical reference; if you're modifying sample serialization, open mmutil's source side by side. The host test `test/mas_diff.c` exercises the roundtrip; run it after any sample-encoding change.

---

## 8b. The instrument `env_flags` byte has only four valid bits

**Rule:** When serializing or deserializing an instrument, treat `env_flags` as a 4-bit field. Bits 0-2 are the EXISTS flags for volume, panning, and pitch envelopes. Bit 3 is the volume envelope's enabled flag. Bits 4-7 are unused, must be written as zero, and must be ignored on read.

**Why:** mmutil only defines four bits in `MAS_INSTR_FLAG_*` constants (`source/mas.h:85-88`), and the maxmod playback engine only checks the EXISTS bits when deciding whether to apply pan and pitch envelopes — there is no `PAN_ENV_ENABLED` or `PITCH_ENV_ENABLED` concept anywhere in the engine. The volume envelope is the only one that has a real "enabled but data still present" distinction; pan and pitch envelopes are simply applied if their EXISTS bit is set.

An earlier version of maxtracker invented two extra bits (4 = pan enabled, 5 = pitch enabled) to back a UI toggle in the instrument editor. The bits were correctly written and read, the editor preserved them in roundtrip, and `mas_diff` passed against maxtracker's own files because the read and write paths were symmetrically wrong. The trap was that the bits had no effect on playback (the engine ignored them) and the file format was no longer mmutil-compatible. The fix was to remove the invented bits, derive `env_pan.enabled` and `env_pitch.enabled` from `node_count > 0` after load, and rewire the instrument-editor toggle to actually create or destroy node sets. See `mas_load.c`'s env_flags constants and the `env_create_default` / `env_destroy` helpers in `instrument_view.c`.

**How to detect violation:** A pan or pitch envelope toggle in the instrument editor that "saves correctly" (the file bytes change) but has no audible effect on playback. Or `mas_diff` failing on a file with non-zero bits 4-7 in any instrument's env_flags. Or a future contributor adding a new flag bit to `env_flags` and having maxmod silently ignore it.

**Where it shows up:** `arm9/source/io/mas_load.c` (envelope flag constants and the parse_instrument body), `arm9/source/io/mas_write.c` (the env_flags computation block), `arm9/source/core/playback.c` (the same computation in `playback_rebuild_mas`), `arm9/source/ui/instrument_view.c` (the `env_create_default` / `env_destroy` helpers and the `param_get`/`param_set` cases for `PARAM_ENV_PAN` / `PARAM_ENV_PITCH`). Host tests `test_envelope_pan_pitch_roundtrip` and `test_envelope_pan_pitch_absent` cover the round trip.

---

## 9. Do not set MF_DVOL on note-off or note-cut rows

**Rule:** When emitting an `MF_*` flag bitmask for a pattern row, never include `MF_DVOL` if the row's note is `NOTE_OFF` (255) or `NOTE_CUT` (254).

**Why:** Maxmod's pattern decoder treats `MF_DVOL` as "set the channel back to the instrument's default volume". If you combine that with a note-off, the channel retriggers (because the volume just changed) at full volume just as it's supposed to be releasing. The result is a loud click on every note-off, and the release envelope never runs because the channel is in attack instead.

This is not an architectural choice — it's a behavior of stock maxmod's `mmReadPattern` that we have to work around in the encoder.

**How to detect violation:** Audible click on every note-off in a song that uses default-volume cells. The release tail never plays. Reproducible on both real hardware and emulators. The host test `playback_cmp` may or may not catch it depending on how the engine state diverges.

**Where it shows up:** `arm9/source/io/mas_write.c` in the pattern serialization path. The flag computation that decides which `MF_*` bits to emit must explicitly exclude `MF_DVOL` when `note >= NOTE_CUT`. There's a comment to this effect in the code; if you find yourself "simplifying" the flag logic, don't remove that exclusion.

---

## 10. The MAS prefix is 8 bytes before the header

**Rule:** Real `.mas` files have an 8-byte mmutil prefix in front of the actual MAS header. Loaders must skip past it; writers must emit it.

**Why:** mmutil prepends `MAS_PREFIX_SIZE = 8` bytes to every file it produces. This is a maxmod convention and isn't documented in the format spec. If you read a `.mas` file expecting the MAS header at offset 0, you get garbage and your offset table parsing produces invalid pointers. If you write a `.mas` file without the prefix, stock maxmod refuses to load it.

**How to detect violation:** Loading any `.mas` file fails with a parse error. Writing a `.mas` file produces something `mas_diff` can't roundtrip and stock maxmod can't play.

**Where it shows up:** `MAS_PREFIX_SIZE` is defined in `include/mt_ipc.h`. `mas_load.c` uses it as the base offset for all subsequent reads. `mas_write.c` emits 8 bytes of prefix at the start of the output file. If you're adjusting the loader or writer, make sure both sides agree on the prefix size and content.

---

## 11. Status messages need a timer or they linger forever

**Rule:** When you set `status_msg`, set `status_timer` to a frame count, and decrement the timer in the frame loop.

**Why:** This isn't a hardware quirk so much as a project convention. The status bar reads `status_msg` and displays it as long as `status_timer > 0`. If you forget to set the timer, the message stays on screen until the next thing overwrites it, which could be never. If you set the timer to a giant number, the same problem.

**How to detect violation:** A status message appears and never goes away.

**Where it shows up:** `arm9/source/core/main.c` has `status_msg` and `status_timer` as file-scope statics, and the frame loop decrements `status_timer` every iteration. The convention is `status_timer = 60;` for a one-second message, `status_timer = 120;` for two seconds, `status_timer = 180;` for three. Use those, not arbitrary frame counts.

---

## 12. Use relative file paths, not `fat:/`

**Rule:** Open files with paths like `"./data/song.mas"`, not `"fat:/data/song.mas"`.

**Why:** The `fat:` device prefix used to be required by older versions of libfat, but the current version uses POSIX paths relative to the executable's working directory. Using `fat:/` works in some configurations but not others, and the failure mode is non-obvious — `fopen` returns NULL with no useful error.

The maxtracker convention is: `fs_browse_root` is set to `"./data/"` when running from a flashcart and `"./"` when running from a NitroFS-embedded build. All file operations should use paths relative to one of those roots.

**How to detect violation:** File loading works in melonDS but fails on real hardware, or vice versa. Specific paths mysteriously fail with ENOENT despite the file being there.

**Where it shows up:** `arm9/source/core/main.c` `main()` sets `fs_browse_root` after `fatInitDefault()` succeeds. The disk screen and autosave both use this root. If you add a new file path, derive it from the root, don't hardcode `fat:/...`.

---

## 13. Compact patterns by detaching playback first

**Rule:** Before freeing or replacing any pattern that ARM7 might be reading, detach playback's cells pointer.

**Why:** ARM7 reads the cells pointer in `MT_SharedPatternState` once per row during playback. If ARM9 frees that memory underneath ARM7, the next read returns garbage and the audio breaks (notes get triggered with random instruments, the engine reads invalid effect parameters, etc.). On real hardware this is also a memory-safety issue: the freed memory may have been reallocated for something else.

The fix is the lifecycle hook system described in [architecture.md § 9](architecture.md). `song_alloc_pattern` and `song_free` both call the registered detach hook before freeing any pattern memory. `playback.c` registers itself as the hook owner at init time. The detach hook sets `shared_state.cells = NULL` and flushes the cache; ARM7 sees the NULL on its next read and skips processing until cells is repointed.

**How to detect violation:** Loading a different song while the current song is playing causes audio corruption or crashes. Using "compact patterns" (or any operation that reallocates patterns) during playback produces the same.

**Where it shows up:** `arm9/source/core/song.c` calls the lifecycle hooks. `playback.c`'s `playback_init` registers them. If you add a code path that frees pattern memory directly (bypassing `song_alloc_pattern`), call the detach hook yourself first. Better: don't bypass the song module's allocator.

---

## 14. melonDS is more forgiving than real hardware

**Rule:** A feature that works in melonDS isn't proven to work on hardware. Test on a real DS or flashcart before declaring something fixed.

**Why:** melonDS simulates the NDS faithfully enough for most things, but it diverges from hardware in a few areas that maxtracker repeatedly stumbles into:

- **Cache coherency** is more lenient. melonDS often shows ARM7's writes to ARM9 even when ARM9 is reading through the cached pointer. Real hardware does not.
- **FIFO timing** is artificially fast. Round-trips that take many milliseconds on hardware happen in microseconds in melonDS, which can hide races.
- **VRAM 8-bit writes** sometimes appear to work in melonDS depending on the version. They never work on hardware.
- **libfat / SD initialization** order matters less in melonDS because melonDS implements the SD interface in software with no resource conflicts.

**How to detect violation:** Anything in this document. The general pattern is: works fine in the emulator, glitches on hardware, you waste an afternoon, eventually realize the bug was always there.

**Where it shows up:** Everywhere. Any change to `playback.c`, `arm7/source/main.c`, `mt_shared.h`, the sample writer, or the file I/O paths should be tested on real hardware (or at least on no$gba, which is closer to hardware in some respects) before being considered done.

---

## 15. Reference projects on disk

When you're chasing a hardware bug and the in-tree code doesn't tell you the answer, three sibling projects on the same machine are the canonical references:

- **`C:\Projects\MAXMXDS`** — A working NDS player using stock maxmod. Compare against this when audio behaves wrong on hardware. If MAXMXDS plays the same `.mas` file correctly and maxtracker doesn't, the bug is in our patch or our IPC layer, not in maxmod itself.
- **`C:\Projects\mmutil`** — The canonical MAS file format authority. Open this when serialization breaks. The host test `test/mas_diff.c` already does roundtrip validation, but mmutil's source is the ground truth for any field encoding question.
- **`C:\Projects\mas_spec`** — Format specification documents. Less authoritative than mmutil's source but easier to read.

These are not part of the maxtracker repository. They live on the developer machine. If you're working on a clone, you may not have them; in that case, ask the project maintainer or fall back to mmutil's public source on the maxmod GitHub.

---

## 16. The autosave path is not a transactional save

**Rule:** Don't rely on the autosave timer to preserve data through a crash. It's a convenience, not a guarantee.

**Why:** Autosave runs in the frame loop every five minutes (`AUTOSAVE_INTERVAL`) and writes the song to a backup path. There is no journal, no atomic rename, and no fsync. If the DS loses power partway through the write, the file is left in an inconsistent state and may not be loadable.

**How to detect violation:** A power loss during autosave produces a `.mas` file that fails to load on next boot. In the worst case the user blames maxtracker for losing their song.

**Where it shows up:** The autosave block at the bottom of the frame loop in `main.c`. If you ever want to make this safer, the standard pattern is: write to `backup.mas.tmp`, fsync, rename to `backup.mas`. The DS POSIX layer supports rename but not fsync, so the guarantee is weaker than on a Unix host.

---

## 17. When in doubt: print, capture, compare

There is no symbolic debugger for the running NDS. You can run melonDS with GDB stubs, but it has the cache-coherency problem described above, so even GDB lies to you about ARM7 state.

The maxtracker debugging convention is:

1. Add `iprintf` calls inside `consoleDemoInit()` mode to print to the bottom screen.
2. For ARM7-side bugs, set values in shared memory from ARM7 and read them from ARM9 with the uncached mirror, so you have a "wire" you can sample.
3. For audio bugs, use the host test `playback_cmp` to compare engine state tick by tick against a reference.
4. For file format bugs, use `mas_diff` to byte-compare.
5. For cross-CPU race bugs, add a logging field to `MT_SharedPatternState` (a counter, a most-recent-value, etc.) and observe it from ARM9. Don't try to reason about the bug in your head.

The hardware is unforgiving but the bugs are usually one of the patterns in this document. Check those first.

---

See also: [architecture.md](architecture.md), [DEVELOPING.md](DEVELOPING.md), [audio_engine.md](audio_engine.md), [DESIGN.md](../DESIGN.md).
