/*
 * screen.h — Dual-screen bitmap management for maxtracker.
 */

#ifndef MT_SCREEN_H
#define MT_SCREEN_H

#include <nds.h>

/* ---- Screen mode ----
 *
 * The on-screen order here IS the navigation order: LSDJ-style
 * SELECT+LEFT/RIGHT walks left/right through this list, SELECT+UP/DOWN
 * walks up/down through the current "row" of screens (PATTERN-SONG etc.).
 * The first column (PATTERN, INSTRUMENT, SAMPLE, LFE, LFE_FX) is the
 * sound-editing corridor; SONG/MIXER/DISK/PROJECT are the management
 * corridor. See navigation.c for the actual adjacency map.
 *
 * X-list: columns = (NAME, short_label)
 *   - The enum is generated from it; SCREEN_COUNT stays in sync
 *     automatically.
 *   - The short label is used by screen_name() for status banners,
 *     debug overlays, and crash logs — keep it <= 6 chars.
 */
#define SCREEN_MODES(X) \
    X(PATTERN,    "PAT")    \
    X(INSTRUMENT, "INST")   \
    X(SAMPLE,     "SAMP")   \
    X(LFE,        "LFE")    \
    X(LFE_FX,     "LFEFX")  \
    X(SONG,       "SONG")   \
    X(MIXER,      "MIX")    \
    X(DISK,       "DISK")   \
    X(PROJECT,    "PROJ")

typedef enum {
#define X(name, label) SCREEN_##name,
    SCREEN_MODES(X)
#undef X
    SCREEN_COUNT
} ScreenMode;

/* Short label for a screen mode. Returns "?" for out-of-range values. */
const char *screen_name(ScreenMode mode);

/* Palette indices */
#define PAL_BG          0
#define PAL_ROW_EVEN    1
#define PAL_ROW_BEAT    2
#define PAL_ROW_CURSOR  3
#define PAL_DIM         4
#define PAL_GRAY        5
#define PAL_TEXT         6
#define PAL_WHITE       7
#define PAL_NOTE        8
#define PAL_INST        9
#define PAL_EFFECT      10
#define PAL_PARAM       11
#define PAL_PLAY        12
#define PAL_RED         13
#define PAL_ORANGE      14
#define PAL_PLAY_BG     15
#define PAL_HEADER_BG   16
#define PAL_SEL_BG      17      /* selection highlight background */

/* Framebuffer pointers (set by screen_init) */
extern u8 *top_fb;
extern u8 *bot_fb;

/* Current screen mode */
extern ScreenMode current_screen;

/* Initialize both screens: MODE_5_2D, 8bpp bitmaps, palette */
void screen_init(void);

/* Set the current screen mode (triggers redraw) */
void screen_set_mode(ScreenMode mode);

/* Double-buffering flush: copy the shadow framebuffers (which drawing code
 * writes to via top_fb / bot_fb) into the real VRAM bitmaps the 2D engines
 * scan out. Must be called once per frame during VBlank so the DMA copy
 * lands while the LCD is not reading VRAM — otherwise you get tearing. */
void screen_flush(void);

#endif /* MT_SCREEN_H */
