#ifndef MT_SCREEN_H
#define MT_SCREEN_H

#include <nds.h>

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
#define PAL_SEL_BG      17

/* Framebuffer pointers (set by screen_init) */
extern u8 *top_fb;
extern u8 *bot_fb;

/* Current screen mode */
extern ScreenMode current_screen;

void screen_init(void);
void screen_set_mode(ScreenMode mode);
void screen_flush(void);

#endif /* MT_SCREEN_H */
