/*
 * mm_engine_shim.h — Host-native shim for compiling the maxmod engine
 * (mas.c + mas_arm.c) on x86/x64 for testing.
 *
 * This header MUST be force-included (-include mm_engine_shim.h) before any
 * other headers.  It blocks the real NDS/maxmod headers and provides all the
 * types, globals, and stubs that the engine needs.
 */

#ifndef MM_ENGINE_SHIM_H
#define MM_ENGINE_SHIM_H

/* ---- Block real platform headers ---- */
#define NDS_INCLUDE         /* blocks <nds.h> */
#define MM_TYPES_H__        /* blocks <mm_types.h> */
#define MAXMOD_H__          /* blocks <maxmod7.h> / <maxmod.h> */
#define MM_MSL_H__          /* blocks <mm_msl.h> */
/* We let ds/arm7/main_ds7.h and ds/arm7/mixer.h through so that
   mm_mix_channels[] extern is visible.  But we stub mixer_types.h
   to avoid pulling in the full DS mixer data structs. */
#define MM_DS_ARM7_MIXER_TYPES_H__  /* blocks ds/arm7/mixer_types.h */
#define MM_CORE_MIXER_H__           /* blocks core/mixer.h (function stubs below) */

/* ---- Standard headers ---- */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Pretend we are NDS ARM7 ---- */
#define __NDS__
#define MAXTRACKER_MODE

/* ---- ARM-specific attribute no-ops ---- */
/* mas_arm.c hard-#defines ARM_CODE = __attribute__((target("arm"))).
 * We can't pre-define it away because the #define in the .c file will override.
 * Instead, we use a GCC diagnostic to suppress the error, and redefine
 * __attribute__ only for the "target" case via a compiler wrapper isn't possible.
 *
 * The real fix: pass -DARM_CODE= on the command line, and patch IWRAM_CODE
 * similarly.  See the build command in the Makefile. */

/* ---- NDS integer types ---- */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;

/* ---- Maxmod types ---- */
typedef uint8_t   mm_byte;
typedef uint16_t  mm_hword;
typedef uint32_t  mm_word;
typedef int8_t    mm_sbyte;
typedef int16_t   mm_shword;
typedef uint32_t  mm_addr;
typedef int32_t   mm_bool;
typedef int32_t   mm_sword;
typedef uint32_t  mm_sfxhand;
typedef uint32_t  (*mm_callback)(uint32_t msg, uint32_t param);

/* ---- Enums that mm_types.h normally provides ---- */
typedef enum { MM_MAIN = 0, MM_JINGLE = 1 } mm_layer_type;
typedef enum { MM_PLAY_LOOP = 0, MM_PLAY_ONCE = 1 } mm_pmode;
typedef enum { MM_MODE_A = 0, MM_MODE_B = 1, MM_MODE_C = 2 } mm_mode_enum;

/* Callback messages */
#define MMCB_SONGMESSAGE   0x2A
#define MMCB_SONGFINISHED  0x2B
#define MMCB_SONGERROR     0x2C
#define MMCB_SONGTICK      0x2D

/* ---- MSL (soundbank) types ---- */
typedef struct {
    mm_hword sampleCount;
    mm_hword moduleCount;
    mm_word  reserved[2];
} msl_head_data;

typedef struct {
    msl_head_data head_data;
    mm_addr       sampleTable[];
} msl_head;

/* ---- Mixer channel ---- */
/* mm_mixer_channel, MP_SAMPFRAC, MIXER_SCALE are defined in channel_types.h
   (guarded by __NDS__).  We just provide the externs that mixer.h / main_ds7.h
   would normally supply. */

#define NUM_CHANNELS       32
#define NUM_PHYS_CHANNELS  16

/* ---- Mixer types stub (from mixer_types.h, blocked above) ---- */
typedef union { mm_byte dummy; } mm_mix_data_ds;

/* ---- Sound effect types (needed by effect.h if included) ---- */
typedef struct {
    mm_word id;
    mm_hword rate;
    mm_hword handle;
    mm_byte volume;
    mm_byte panning;
} mm_sound_effect;

/* ---- DS sample format constants ---- */
#ifndef MM_SREPEAT_FORWARD
#define MM_SREPEAT_FORWARD  1
#endif
#ifndef MM_SREPEAT_OFF
#define MM_SREPEAT_OFF      2
#endif
#ifndef MM_SFORMAT_8BIT
#define MM_SFORMAT_8BIT     0
#endif
#ifndef MM_SFORMAT_16BIT
#define MM_SFORMAT_16BIT    1
#endif

/* ---- Globals provided by engine_stubs.c ---- */
/* mm_mixer_channel is defined later by channel_types.h (after __NDS__ is set).
 * The extern for mm_mix_channels[] and the definition of the stubs are in
 * engine_stubs.c which is compiled alongside the test. */

/* mmModuleBank / mmSampleBank declared by ds/arm7/main_ds7.h (not blocked).
 * mp_solution is GBA-only; on NDS the module bank is used instead.
 * Provide it here for any references in mas.c that are ifdef __GBA__-guarded
 * (shouldn't be reached, but keeps the linker happy). */
extern msl_head *mp_solution;

/* ---- Mixer function stubs (core/mixer.h) ---- */
static inline void mmMixerSetVolume(int ch, mm_word vol)   { (void)ch; (void)vol; }
static inline void mmMixerSetPan(int ch, mm_byte pan)      { (void)ch; (void)pan; }
static inline void mmMixerSetFreq(int ch, mm_word rate)    { (void)ch; (void)rate; }
static inline void mmMixerMulFreq(int ch, mm_word factor)  { (void)ch; (void)factor; }
static inline void mmMixerStopChannel(int ch)              { (void)ch; }

/* ---- Functions that main_ds7.c or other platform code provides ---- */
static inline mm_word mmGetModuleCount(void) { return 0; }
static inline mm_word mmGetSampleCount(void) { return 0; }

/* Forward declaration — defined later in mas.c but called before definition */
void mmPlayMAS(uintptr_t address, mm_word mode, mm_word layer);

/* ---- Playback stubs (normally in playback.c, ARM9 side) ---- */
#define MT_PLAYBACK_H
static inline void playback_init(void) {}
static inline void playback_play(u8 p) { (void)p; }
static inline void playback_stop(void) {}
static inline void playback_update(void) {}
static inline int  playback_is_playing(void) { return 0; }
static inline u8   playback_get_row(void) { return 0; }
static inline u8   playback_get_order(void) { return 0; }
static inline u8   playback_get_tick(void) { return 0; }
static inline void playback_preview_note(u8 n, u8 i) { (void)n; (void)i; }
static inline void playback_stop_preview(void) {}
static inline void playback_set_mute(u8 c, int m) { (void)c; (void)m; }
static inline u32  playback_get_mute_mask(void) { return 0; }
static inline void playback_set_tempo(u8 b) { (void)b; }
static inline void playback_rebuild_mas(void) {}
static inline void playback_mark_cells_dirty(void) {}
static inline void playback_detach_pattern(void) {}
static inline void playback_reattach_pattern(void) {}
static inline void playback_refresh_shared_tables(void) {}

/* ---- Cache / DMA / HW stubs ---- */
#define DC_FlushRange(a, s)       ((void)0)
#define DC_InvalidateRange(a, s)  ((void)0)
#define swiDelay(n)               ((void)0)

/* ---- Volatile qualifier alias ---- */
typedef volatile uint16_t vu16;

/* ---- printf redirect ---- */
#define iprintf printf

/* ---- FIFO stubs ---- */
#define FIFO_MAXMOD 4

#endif /* MM_ENGINE_SHIM_H */
