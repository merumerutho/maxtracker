/*
 * nds_shim.h — Minimal NDS type shim for host-native unit tests.
 *
 * Provides the typedefs and macros that NDS code expects, so that
 * pure-logic modules (song, clipboard, undo, MAS I/O, scale) can
 * compile and run on x86/x64 with a standard C compiler.
 */

#ifndef NDS_SHIM_H
#define NDS_SHIM_H

/* If the engine shim is active, skip everything — it provides a superset. */
#ifdef MM_ENGINE_SHIM_H
/* nothing */
#else

/* Prevent the real <nds.h> and <mm_types.h> from being included */
#define NDS_INCLUDE
#define MM_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* NDS integer types */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;

/* Maxmod types (we block mm_types.h via MM_TYPES_H__) */
typedef uint8_t   mm_byte;
typedef uint16_t  mm_hword;
typedef uint32_t  mm_word;
typedef int8_t    mm_sbyte;
typedef uint32_t  mm_addr;
typedef int32_t   mm_bool;
typedef int16_t   mm_shword;

/* Volatile qualifier used in shared state (no-op on host) */
typedef volatile uint16_t vu16;

/* DC_FlushRange / DC_InvalidateRange — no-op on host */
#define DC_FlushRange(addr, size)      ((void)0)
#define DC_InvalidateRange(addr, size) ((void)0)

/* iprintf → printf on host */
#include <stdio.h>
#define iprintf printf

/* FIFO stubs — no-op on host */
#define FIFO_MT 15
#define fifoSendValue32(ch, val)   ((void)0)
#define fifoSendAddress(ch, addr)  ((void)0)

/* Block playback.h and provide stubs (playback.c not compiled for host tests) */
#define MT_PLAYBACK_H
static inline void playback_init(void) {}
static inline void playback_play(u8 p) { (void)p; }
static inline void playback_stop(void) {}
static inline void playback_update(void) {}
static inline bool playback_is_playing(void) { return false; }
static inline u8   playback_get_row(void) { return 0; }
static inline u8   playback_get_order(void) { return 0; }
static inline u8   playback_get_tick(void) { return 0; }
static inline void playback_preview_note(u8 n, u8 i) { (void)n; (void)i; }
static inline void playback_stop_preview(void) {}
static inline void playback_set_mute(u8 c, bool m) { (void)c; (void)m; }
static inline u32  playback_get_mute_mask(void) { return 0; }
static inline void playback_set_tempo(u8 b) { (void)b; }
static inline void playback_rebuild_mas(void) {}
static inline void playback_mark_cells_dirty(void) {}
static inline void playback_detach_pattern(void) {}
static inline void playback_reattach_pattern(void) {}
static inline void playback_refresh_shared_tables(void) {}

/* Maxmod enums/defines that mm_mas.h or our code references */
typedef u32 (*mm_callback)(u32 msg, u32 param);
typedef u32 mm_sfxhand;
#define mmSetModuleTempo(t) ((void)0)

/* MAS format constants needed by mas_write/mas_load */
#ifndef MM_SREPEAT_FORWARD
#define MM_SREPEAT_FORWARD 1
#endif

/* libnds key bitmasks — mirror the values in <nds/arm9/input.h>. Keep in
 * sync if libnds ever renumbers (unlikely; the values are ABI-stable). */
#ifndef KEY_A
#define KEY_A       (1 <<  0)
#define KEY_B       (1 <<  1)
#define KEY_SELECT  (1 <<  2)
#define KEY_START   (1 <<  3)
#define KEY_RIGHT   (1 <<  4)
#define KEY_LEFT    (1 <<  5)
#define KEY_UP      (1 <<  6)
#define KEY_DOWN    (1 <<  7)
#define KEY_R       (1 <<  8)
#define KEY_L       (1 <<  9)
#define KEY_X       (1 << 10)
#define KEY_Y       (1 << 11)
#define KEY_TOUCH   (1 << 12)
#define KEY_LID     (1 << 13)
#endif

/* libnds key-repeat knob — no-op on host (no input loop to drive). */
static inline void keysSetRepeat(unsigned delay, unsigned rate)
{
    (void)delay; (void)rate;
}

#endif /* !MM_ENGINE_SHIM_H */
#endif /* NDS_SHIM_H */
