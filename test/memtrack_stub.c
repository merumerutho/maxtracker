#include <stdint.h>
typedef uint8_t u8; typedef uint32_t u32;

/*
 * Stub for host tests: always report "no memory needed".
 *
 * mt_mem_check() in memtrack.h compares the returned value with `<=` against
 * MT_RAM_AVAILABLE, so returning 0 makes the soft memory warning never fire.
 * (Returning 0xFFFFFFFF would mean "you need 4GB", which fails the check
 * and makes mas_load return its mem_warning code rather than 0.)
 */
u32 mt_mem_estimate_mas(u8 a, u32 b) { (void)a; (void)b; return 0; }

/*
 * Host-side stubs for globals that live in `main.c` on-device but
 * aren't linked into the host test. `editor_state.c` is compiled
 * into the host build, so `cursor` / `play_mode` are real (not
 * stubbed). Everything else here is just a symbol the linker needs
 * to resolve — the clipboard / autosave-helper tests don't read
 * these values.
 */
#include <stdbool.h>

char status_msg[64];
int  status_timer;
bool song_modified;
bool autosave_dirty;
