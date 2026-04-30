# maxtracker -- Coding Conventions

Parent: [DESIGN.md](../DESIGN.md)

---

## 0. What this document is

The conventions the maxtracker codebase follows. Some are strong project rules; others are softer style preferences. Both are documented here so contributors don't have to reverse-engineer them from the existing code, and so they can be applied consistently in new code.

If you only have time to read one section, read section 2 (the comment policy) and section 4 (the trust-at-boundaries rule). Those are the two principles that affect the most lines of code.

---

## 1. C dialect and language features

The project is plain C, compiled with devkitARM's `arm-none-eabi-gcc` for the device builds and the host's `gcc` (or any reasonable compatible compiler) for the host tests. The dialect target is roughly C99 with a few compiler-extension uses where needed.

In practice that means:

- **Stay within C99 + GNU extensions.** No C11, C17, or C23 features unless you've verified devkitARM supports them. The compiler does support some newer features but the project hasn't relied on them.
- **Flexible array members** are used (`MT_Pattern` ends in `cells[]`). They're a C99 feature; use them when appropriate.
- **Designated initializers** are used (`{ .field = value }`). Prefer these over positional initializers when there are more than two or three fields.
- **`stdbool.h`** is included for `bool` / `true` / `false`. Use them.
- **`stdint.h`** types are not used directly. The project uses libnds's `u8`, `u16`, `u32`, `s8`, `s16`, `s32` instead, because those are what the NDS toolchain everywhere else uses. Stay consistent.
- **No GCC nested functions.** They don't work cleanly with all targets and add no value.
- **No `goto`** except for the well-understood "single cleanup point at the end of the function" pattern. Even then, prefer linear early returns when the cleanup is trivial.

The project does not use C++. Don't introduce it.

---

## 2. Comments

This is the rule that affects the largest number of lines of code in the project, so it gets the most space.

**Default to no comments.** Most code does not need comments. A well-named function with a clean signature and clean body explains itself. Adding a comment that restates the code is noise, and noise is worse than nothing because it competes with the code for the reader's attention.

**Comments explain WHY, not WHAT.** If you find yourself writing:

```c
// Increment the counter
counter++;
```

delete the comment. The code already says that. If you're writing:

```c
// We have to add 1 here because the maxmod sample length convention
// stores N - 1 to allow zero-length samples to be represented as 0xFFFF.
length = stored_value + 1;
```

that's a good comment, because it explains a non-obvious convention from an external dependency. The reader can't recover that from the code alone.

**Comments must have a long shelf life.** Don't write comments that reference the current task, the current PR, the current bug, the current developer, or the current date. Examples of what NOT to write:

- `// Added for the new sample editor`
- `// Used by the M8-style A-press handler`
- `// Quick fix for issue #42`
- `// TODO: revisit when we add stereo samples`
- `// As of 2026-04-09, this works around the libfat bug`

These rot. The code that "uses" the comment-tagged function moves around or disappears. The issue tracker reorganizes. The TODO outlives the developer who wrote it. A year later the comment is either misleading or just confusing.

The right place for that information is the commit message, the PR description, or the issue itself. Those have permanent timestamps and can be searched. Source comments cannot.

**No "what's removed" markers.** Don't leave behind comments like `// removed: old_function()` or `// no longer needed`. If something is gone, it's gone. The git history records what was deleted; the source file records what is.

**Function header comments are OK if they document the contract.** Specifically: preconditions, postconditions, ownership of pointers (who frees what), error return semantics, and any non-obvious behavior. Don't write a header comment that just restates the function name. Example of a good header:

```c
/*
 * undo_push_block -- Snapshot a rectangular block before editing it.
 * row_start..row_end and ch_start..ch_end are inclusive.
 */
```

Inclusive vs exclusive ranges are the kind of thing the function name can't convey, so the comment is earning its keep.

**Single-line clarifying comments are OK in surprising places.** If a line of code is unavoidably non-obvious (a workaround for a hardware bug, a numeric constant that came from somewhere specific, a non-standard ordering), a one-line comment is appropriate. Example:

```c
fatInitDefault();    // must precede mmInit on real hardware (see hardware_quirks.md § 2)
```

This is earning its keep because the next contributor will be tempted to reorder for stylistic reasons unless they know the constraint.

**Multi-paragraph block comments inside functions are almost always wrong.** If you need three paragraphs to explain what a function does, the function probably wants to be broken up, or the explanation belongs in a header comment, or the explanation belongs in this `doc/` directory. Inline essays are a code smell.

**No emojis in source files.** Don't add them unless explicitly asked. The user has not asked.

---

## 3. Naming

The project's naming is informal but consistent. A few conventions to follow:

**Type names are PascalCase with an `MT_` prefix.** `MT_Cell`, `MT_Pattern`, `MT_Song`, `MT_Instrument`, `MT_Sample`, `MT_SharedPatternState`, `MT_UndoEntry`, `MT_NoteClipboard`. The `MT_` prefix marks "this is a maxtracker type" and avoids collisions with NDS toolchain types.

**Constants and macros are SCREAMING_SNAKE_CASE with an `MT_` prefix.** `MT_MAX_CHANNELS`, `MT_MAX_ROWS`, `MT_UNDO_DEPTH`, `MT_PATTERN_SIZE`. Same rationale.

**Function names are snake_case.** `song_init`, `clipboard_paste`, `undo_push_block`, `playback_play`, `pattern_view_draw`. The convention is `module_verb` so it's clear which module a function belongs to.

**Static (file-local) functions follow the same convention** but don't need the `module_` prefix when their scope makes it obvious. `cursor_advance`, `cursor_cell`, `format_note` (all in `main.c` or `pattern_view.c`).

**Local variables are short snake_case.** `row`, `col`, `pat`, `cell`, `nrows`, `ch_start`. Don't be afraid of two-letter names for loop counters or hot-loop temporaries; do avoid two-letter names for things that live more than a few lines.

**Globals are snake_case without a prefix.** `song`, `cursor`, `clipboard`, `note_clipboard`. They don't need a project prefix because they're already in a small scope (the whole program).

**Boolean accessors and predicates start with a verb.** `playback_is_playing`, `clipboard_has_block`, `mt_mem_check`. Read the function name aloud; if it sounds like a question, it's correctly named.

**Acronyms in names are uppercase or all-lowercase, not mixed.** `mas_load` and `MAS_HEADER_SIZE` are both fine; `Mas_Load` is not.

When in doubt, look at five or ten existing names in the file you're editing and match them.

---

## 4. Error handling

The project uses return codes, not exceptions. There are no `setjmp`/`longjmp` paths and no global error state.

**Validate at boundaries; trust internal calls.** A "boundary" is anywhere data crosses from somewhere maxtracker doesn't control:

- User input from button presses
- File contents loaded from the SD card
- Cross-CPU messages from ARM7
- Allocations that might fail (`malloc`, `calloc`)

At a boundary, validate aggressively. Check ranges. Bounds-check offsets. Verify magic numbers. Reject malformed input. Return an error code.

Inside the trust boundary (when a function is being called by other functions in the same module that you control), don't re-validate things the caller has already verified. Pre-validating the same condition five times wastes code, wastes performance, and makes the actual logic harder to read.

For example, `clipboard_paste(MT_Pattern *pat, u8 row, u8 channel)` accepts a pattern pointer that the caller is supposed to have produced from the song model. We validate `row >= pat->nrows` defensively because that condition can occur if the cursor drifts out of bounds. We don't validate `pat != NULL` checks against `pat->ncols == 0`, because anything that produced an `MT_Pattern *` already knows it's well-formed.

**Errors propagate with negative return codes.** The convention across the project is:

- `0` means success
- A positive value means "success with a warning" (e.g. `mas_load` returning 1 to indicate "loaded but ran out of memory partway through")
- A negative value means hard failure

When you add a new function that can fail, follow this pattern. Define the specific error codes near the function (e.g. `WAV_OK = 0`, `WAV_BAD_HEADER = -1`, etc.), and document them in the header.

**Don't introduce error handling for impossible cases.** If a function is called from exactly one place that already verified the input, don't add a "what if NULL" check inside the function "just to be safe". The check costs space, costs cycles, and trains the reader to mistrust internal call paths. Save the defensive checks for the boundary.

The exception is when the function is exported (declared in a header) and could be called from a future caller you don't control. Then defensive checks earn their keep, since the trust boundary now includes "future me".

**No silent failures.** If a function fails and the caller doesn't check, that's a bug in the caller. But if a function fails *silently* (does nothing, returns success, leaves no trace), that's a bug in the function. Always return an error code or set an error flag.

**Status messages are how we tell the user about failures.** The pattern is:

```c
snprintf(status_msg, sizeof(status_msg), "Save error: %d", err);
status_timer = 180;  // 3 seconds at 60 FPS
```

Use this whenever a user-initiated action fails (load, save, paste). The message and timer are file-scope statics in `main.c`; the frame loop decrements the timer and the status bar reads `status_msg`. Don't `iprintf` to a console for user-facing errors.

---

## 5. Resource management

C doesn't have RAII or destructors. Resource cleanup is explicit at every call site. The conventions:

**`malloc` is paired with `free` in the same module.** If `clipboard.c` allocates something, `clipboard.c` frees it. If you find yourself allocating in one module and freeing in another, the ownership is unclear and you should redesign.

**Allocations check for failure.** Always:

```c
MT_Cell *data = (MT_Cell *)malloc(size);
if (!data) return -1;  // or whatever the error path is
```

The DS has 4 MB of RAM. `malloc` can and does fail on long sessions or large songs. Don't pretend otherwise.

**Free old before assigning new.** When a struct field holds a malloc'd pointer and you're about to replace it:

```c
if (smp->pcm_data) free(smp->pcm_data);
smp->pcm_data = new_data;
```

Forgetting this leaks. The leak is invisible until someone profiles and the project doesn't have profiling.

**On error paths, free what you've already allocated.** Multi-step initialization that allocates several things should free them in reverse order on the error path. Look at how `mas_load` cleans up `instr_offsets`, `samp_offsets`, `patt_offsets`, and `buf` on error paths:

```c
if (!patt_offsets) { free(samp_offsets); free(instr_offsets); free(buf); return -4; }
```

Yes, this is verbose. No, the project does not introduce a "destructor function" abstraction to hide it. Verbosity is the right answer here because it makes the cleanup contract explicit.

**Avoid `realloc`.** It's correct to use, but it complicates failure handling (you have to remember the old pointer if `realloc` fails). The project mostly avoids it. If you need to grow a buffer, free + malloc is usually fine at this scale.

---

## 6. Headers

Header files in this project follow a strict form:

```c
/*
 * filename.h -- One-line description of what this module does.
 *
 * Optional second paragraph if there are surprising invariants or
 * cross-cutting concerns the user must know.
 */

#ifndef MT_FILENAME_H
#define MT_FILENAME_H

#include <nds.h>
#include <stdbool.h>
#include "other_module.h"

/* Type definitions first */

typedef struct { ... } SomeType;

/* Constants next */

#define MT_SOMETHING 64

/* Then function prototypes, grouped by purpose */

void module_init(void);
void module_do_thing(SomeType *t);
bool module_query(const SomeType *t);

#endif /* MT_FILENAME_H */
```

A few rules to follow:

**Header guards use the `MT_` prefix and the filename in uppercase.** `MT_SONG_H`, `MT_CLIPBOARD_H`. Match the existing pattern.

**Includes go in this order**: system headers (`<nds.h>`, `<stdbool.h>`, `<stdio.h>`), then project headers in dependency order. Don't include things you don't need.

**Forward declarations are fine for function pointer types.** If a header defines a callback type that takes a pointer to a struct defined elsewhere, you can forward-declare the struct rather than including its full header. This keeps include graphs shallow.

**Headers should be self-contained.** A `.c` file that includes only one header should compile (with the system headers it pulls in transitively). Don't rely on a particular include order.

**No code in headers**, except for `static inline` functions when they're trivial (single-line accessors, packing macros). The `MT_POS_PACK` and friends in `mt_shared.h` are an example. If a function would benefit from inlining and is small, `static inline` in a header is acceptable. If the function is larger than a few lines, put it in a `.c` file.

---

## 7. File organization

A `.c` file follows this rough structure:

```c
/*
 * filename.c -- One-line description.
 */

#include "filename.h"
#include "other_module.h"
#include <stdlib.h>
#include <string.h>

/* File-scope state (statics) */

static int g_some_state = 0;

/* Static helper functions */

static void helper_thing(int x) { ... }

/* Public API functions (matching the order in the header) */

void module_init(void) { ... }
void module_do_thing(SomeType *t) { ... }
```

**Includes**: own header first, then other project headers, then system headers. This catches missing includes in the header itself (if `filename.h` forgot to include something it needs, including it first surfaces the error).

**Statics first, then helpers, then public API.** The reader can scroll top-to-bottom and see state, helpers, then the meaningful functions. Functions used before they're defined need a forward declaration.

**Group related functions together.** Don't intersperse a public API function with three helpers. Put all helpers above the public functions that use them (or below, if there are many helpers and the public API is small; pick whichever is easier to navigate).

**One module = one purpose.** If a `.c` file is doing two unrelated things, split it. The project's modules average ~300-500 lines. The big exception is `main.c`, which is 1500+ lines and known to need splitting.

---

## 8. State and globals

Maxtracker uses globals freely for things that exist in exactly one instance per program run. There is no dependency-injection container, no `Editor *` passed around, no service locator.

**The globals you'll encounter are:**

- **`song`** (`arm9/source/core/song.c`) -- The single `MT_Song` struct that holds the currently-loaded song. Every UI view reads it; the controller writes it.
- **`cursor`** (`arm9/source/ui/editor_state.c`) -- The editor cursor. Read by every view, written by the controller.
- **`clipboard`** (`arm9/source/core/clipboard.c`) -- The block clipboard.
- **`note_clipboard`** (`arm9/source/core/clipboard.c`) -- The single-slot clipboard for M8-style A-press copy/paste.
- **`shared_state`** (`arm9/source/core/playback.c`) -- The cross-CPU shared state struct. ARM9 writes most fields; ARM7 writes the position state.
- **`current_screen`** (`arm9/source/ui/screen.c`) -- Current `ScreenMode`.
- **`top_fb`, `bot_fb`** (`arm9/source/ui/screen.c`) -- Framebuffer pointers.
- **`status_msg`, `status_timer`** (`arm9/source/core/main.c`) -- User-facing status message.

Each of these has exactly one definition (`extern` declared in the header, defined in the corresponding `.c` file). Don't add a second one. If you find yourself wanting "another cursor" or "another song", you're probably solving the wrong problem.

**File-scope statics are fine for screen-local state.** A view file may have static variables that hold scroll positions, animation frame counters, or other state that doesn't need to be visible outside the file. Use them. The rule is: state is global if it must outlive the screen mode; state is file-scope-static if it only matters while that screen is active; state is automatic (stack-local) if it only matters for the duration of one function.

**Don't pass globals as parameters.** If a function operates on the song, it reads the global `song`. Don't write `do_thing(MT_Song *s, ...)` and then call it as `do_thing(&song, ...)`. The exception is functions that genuinely operate on a *different* song instance, e.g. the host test harness loading into a temporary song struct for differential testing. In that case, the parameter earns its keep.

---

## 9. Concurrency

There is no threading on ARM9. The only "concurrency" is:

- **Interrupt handlers**, which preempt the main loop. Maxtracker's IRQ handlers all follow the same convention: read a value into a static volatile, set a flag, return. The main loop drains the flag.
- **The other CPU**, which runs concurrently and has its own contract. Read [architecture.md section 8](architecture.md) and [hardware_quirks.md](hardware_quirks.md) for the rules.

This means inside the main loop you can treat all of `song`, `cursor`, `clipboard`, etc. as exclusively yours. No locks, no atomics, no `volatile` qualifiers needed. The only place `volatile` is needed is on memory shared with ARM7 or with an interrupt handler.

**Volatile rules:**

- Fields written in IRQ context and read in main loop: `volatile`.
- Fields written by ARM7 and read by ARM9 (or vice versa): `volatile`.
- Fields read and written only in main loop: not `volatile`.

`MT_SharedPatternState` correctly marks all cross-CPU fields as `volatile`. So do `pending_play`, `pending_stop`, `pending_addr_type` in `arm7/source/main.c`. Follow the same pattern.

---

## 10. The "no premature abstraction" rule

This is project policy, not just style. It comes up enough to deserve its own section.

**Three similar lines is not a pattern.** When you see three pieces of code that look similar, the temptation is to extract a helper function. Don't. Three is the minimum where a pattern *could* exist, but it's also the minimum where the differences between cases haven't fully manifested. Extracting too early commits you to an abstraction that fits the first three cases and hits friction on the fourth.

The rule is: extract the helper when there's a fourth caller, OR when the duplication has actually caused a bug (one of the three diverged from the others by accident). Until then, three near-duplicates are cheaper than a premature abstraction.

**No abstraction for hypothetical future requirements.** Don't introduce a "scene base struct" because we might add more screens. Don't introduce a "pluggable file format loader" because we might add MIDI import. The right time to add the abstraction is when the second concrete case actually exists. Building scaffolding for code that doesn't exist yet means the scaffolding gets shaped by guesses, and the actual code has to fit the guesses.

**No half-finished implementations.** Don't merge a function whose body is `{ /* TODO */ return 0; }`. Don't add a parameter that's "for future use". Don't define an enum value with no consumer. If the feature isn't done, leave it out entirely until it is.

**Resist the urge to "clean up while I'm here".** When you're fixing a bug or adding a feature, change only what's necessary for the change. The temptation to also rename variables, reformat blocks, or extract helpers in unrelated code adds noise to the diff and risks introducing regressions in code you didn't intend to touch. If the cleanup is worth doing, do it as a separate commit.

---

## 11. Testing

**Write a test when the bug bites.** Most code in this project doesn't have a unit test, and that's fine: the test plan is in [test_plan.md](test_plan.md), and the host tests cover the high-leverage areas (song model, clipboard, undo, MAS roundtrip). When a bug is found in code that wasn't tested, the right response is usually:

1. Reproduce the bug as a test case in `test/test.c` or one of the comparison tools.
2. Verify the test fails on the broken code.
3. Fix the code.
4. Verify the test passes.
5. Commit the test alongside the fix.

This means the test exists permanently to prevent regression. It also means you don't have to maintain tests for code that has no track record of breaking.

**Don't write tests "for coverage".** A test that does `MT_ASSERT_EQ(song_init(); song.initial_tempo, 125)` adds no value because nothing has ever broken `song.initial_tempo`. Tests that exist only to make the count go up are noise.

**Prefer high-level integration tests.** A test that exercises five subsystems together (load -> edit -> save -> reload -> check) catches more real bugs per line than a test that exercises one function in isolation. The host test infrastructure was built with this in mind.

**Tests should be deterministic and fast.** Aim for under 10 ms per test. Don't depend on system clocks, on file system state across runs, or on the order in which tests run. If you need a "before" and "after" comparison, build both halves inside the test.

**Stub aggressively when it helps clarity.** The host test `nds_shim.h` stubs out the entire NDS hardware layer (FIFO, DMA, VRAM, cache operations) so the song model can be tested without an emulator. When you add a host test that needs another stubbed function, add it to the shim with a no-op or sentinel implementation. Don't try to make the test "more realistic" by linking real NDS code.

See [DEVELOPING.md section 5](DEVELOPING.md) for the practical mechanics of adding a test.

---

## 12. Build hygiene

A few things that aren't enforced by the build system but are project policy:

**Builds should produce no warnings.** The host test build runs with `-Wall -Wextra`. The device build runs with whatever devkitPro's defaults are. Both should be warning-clean. If your change introduces a warning, fix the underlying cause; don't suppress it.

**Don't commit generated files.** The `release/` directory, the `arm9/build/` and `arm7/build/` directories, and any `.o` / `.elf` / `.nds` outputs are generated. The maxmod build artifacts under `lib/maxmod/` are also generated. None of them go into the repository.

**Don't commit emulator save states or test outputs.** If your debugging produces a melonDS save state or a captured tick log, leave it on your machine. They're large, they're personal, and they go stale immediately.

**The user handles the actual build commands.** Don't run `make` from agent code unless explicitly asked. The user is at the keyboard and will run the build when they're ready. (This is a project preference recorded in the repository's auto-memory and applies specifically to AI-assisted development.)

---

## 13. Commit messages

The project uses regular git commits, not squashed PRs or auto-generated changelogs. Commit messages follow standard conventions:

- **First line is a one-sentence summary.** Imperative mood ("Add foo" not "Added foo"). Under 70 characters.
- **Blank line.**
- **Body explains the why.** What problem does this commit solve? What was the user-visible behavior before vs after? What was considered and rejected? The body is where you put the context that doesn't fit in source comments (because source comments rot).
- **Reference issues by number when relevant.** "Fixes #42" at the bottom of the body is conventional. The project doesn't currently have a public issue tracker but if it ever does, this is the format.
- **One logical change per commit.** A commit that fixes a bug AND refactors AND adds a test should be three commits. The bug fix should be small and focused; the refactor should be reviewable on its own; the test should clearly tie to the bug fix.

When AI tooling generates commit messages, it should follow the same conventions and include the trailer the user has configured for `Co-Authored-By`. The user controls this; don't add or remove the trailer without being asked.

---

## 14. Things to avoid

A grab bag of patterns that have come up enough to be worth listing.

- **Don't introduce backwards-compatibility shims.** If a function changes signature, update all callers in the same commit. The project is small enough that "the old API stays around for one release" is unnecessary friction.
- **Don't add feature flags for in-development work.** Either the feature is ready and merged, or it isn't and lives in a branch. No `#ifdef EXPERIMENTAL_THING`.
- **Don't add `// removed: ...` comments.** Just delete the code. Git remembers.
- **Don't rename `_var` to `var` "for cleanliness".** If a leading underscore was load-bearing in some piece of code, removing it breaks that code. Leave it alone unless you've verified it's truly unused.
- **Don't try to make C feel like C++.** No "namespaces" in the form of typedef wrappers. No `init`/`deinit` pairs that hide their actual cost. No `Vtable` structs of function pointers unless polymorphism is genuinely needed (it isn't).
- **Don't use `int` when you mean a fixed-width type.** `u8`, `u16`, `u32` exist for a reason. The pattern is: use the smallest type that fits the data.
- **Don't use `long` or `long long`.** Their size is platform-dependent. Use `u32` or `u64`.
- **Don't `#define` a constant that's only used in one file.** Make it `static const` or just use the literal. `#define`s pollute the global namespace.
- **Don't use `assert.h`.** The NDS doesn't have a meaningful place for assertion failures to print. Use return codes and explicit error handling.
- **Don't use floating point in hot paths.** The ARM9 has no FPU. Floating point is software-emulated and slow. The project uses fixed-point or integer arithmetic everywhere it matters.

---

## 15. When in doubt

If the conventions don't cover your case, the fallback is: **match the closest existing code**. Find a file in the project that does something similar to what you're doing, read it, and follow its style. The codebase is small enough that there's almost always a precedent.

If there genuinely isn't a precedent and you're introducing a new pattern, document it in this file in the same commit. The point of conventions is consistency, not following arbitrary rules, but they have to be written down somewhere or they aren't conventions, they're just one developer's habits.

---

See also: [architecture.md](architecture.md), [hardware_quirks.md](hardware_quirks.md), [DEVELOPING.md](DEVELOPING.md), [DESIGN.md](../DESIGN.md).
