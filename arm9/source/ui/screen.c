/*
 * screen.c — Dual-screen 8bpp bitmap setup and palette for maxtracker.
 *
 * Double-buffered rendering:
 *
 *   Drawing code writes to SHADOW framebuffers in main RAM via top_fb /
 *   bot_fb. Once per frame, during VBlank, screen_flush() DMA-copies the
 *   shadow buffers into the real VRAM bitmaps (top_vram / bot_vram) that
 *   the 2D engines scan out. Writing to VRAM only while the LCD is not
 *   reading it eliminates scanout races entirely — no tearing, no half-
 *   drawn frames, no "obscuring band" caused by draws overrunning VBlank.
 *
 *   The cost is 48 KB × 2 = 96 KB of main RAM (out of 4 MB EWRAM) and a
 *   pair of DMA copies per frame (~120 µs each, well inside the 4.4 ms
 *   VBlank window).
 *
 *   Callers see no API change: top_fb / bot_fb are still u8* pointers,
 *   still accept 8bpp palette bytes. They just point at RAM now, so byte
 *   writes are also safe (NDS VRAM rejects 8-bit writes — RAM does not —
 *   but font.c still uses vu16 halfword RMW, which is correct in both
 *   places and harmless here).
 */

#include "screen.h"
#include "font.h"
#include "pattern_view.h"
#include <string.h>

/* Shadow framebuffers: the buffers drawing code targets. 32-byte aligned
 * so DC_FlushRange operates cleanly on whole cache lines. */
static u8 top_shadow[256 * 192] __attribute__((aligned(32)));
static u8 bot_shadow[256 * 192] __attribute__((aligned(32)));

/* Real VRAM bitmaps — destinations for screen_flush() DMA. Not written
 * directly by any draw code. */
static u8 *top_vram = NULL;
static u8 *bot_vram = NULL;

/* Public pointers. Alias the shadow buffers so every existing font_*
 * call in the codebase keeps working without modification. */
u8 *top_fb = NULL;
u8 *bot_fb = NULL;

ScreenMode current_screen = SCREEN_PATTERN;

static int top_bg2 = -1;
static int bot_bg2 = -1;

static void setup_palette(vu16 *pal)
{
    pal[PAL_BG]         = RGB15( 0,  0,  1);   /* near-black */
    pal[PAL_ROW_EVEN]   = RGB15( 3,  3,  6);   /* dark blue tint */
    pal[PAL_ROW_BEAT]   = RGB15( 4,  5,  9);   /* beat row: brighter blue */
    pal[PAL_ROW_CURSOR] = RGB15( 4,  7, 16);   /* cursor row: vivid blue */
    pal[PAL_DIM]        = RGB15(10, 10, 10);   /* dim gray */
    pal[PAL_GRAY]       = RGB15(18, 18, 18);   /* gray */
    pal[PAL_TEXT]        = RGB15(25, 25, 25);   /* light gray text */
    pal[PAL_WHITE]      = RGB15(31, 31, 31);   /* white */
    pal[PAL_NOTE]       = RGB15( 0, 31, 22);   /* cyan - notes */
    pal[PAL_INST]       = RGB15(31, 28,  6);   /* yellow - instruments */
    pal[PAL_EFFECT]     = RGB15(14, 20, 31);   /* blue - effects */
    pal[PAL_PARAM]      = RGB15(28, 16, 31);   /* magenta - params */
    pal[PAL_PLAY]       = RGB15( 8, 31,  8);   /* green - playing */
    pal[PAL_RED]        = RGB15(31,  6,  6);   /* red - mute/warning */
    pal[PAL_ORANGE]     = RGB15(31, 20,  0);   /* orange - selection */
    pal[PAL_PLAY_BG]    = RGB15( 4, 10,  4);   /* dark green - playing row bg */
    pal[PAL_HEADER_BG]  = RGB15( 6,  4,  8);   /* dark purple - header bar */
    pal[PAL_SEL_BG]     = RGB15(10,  8,  2);   /* dark amber - selection bg */
}

const char *screen_name(ScreenMode mode)
{
    switch (mode) {
#define X(name, label) case SCREEN_##name: return label;
    SCREEN_MODES(X)
#undef X
    default: return "?";
    }
}

void screen_init(void)
{
    /* Power on both screens */
    powerOn(POWER_ALL_2D);

    /* Top screen = main engine */
    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG);

    /* Bottom screen = sub engine */
    videoSetModeSub(MODE_5_2D | DISPLAY_BG2_ACTIVE);
    vramSetBankC(VRAM_C_SUB_BG);

    /* Top screen: 8bpp bitmap on BG2
     * mapBase=4 places bitmap at 4*16KB = 64KB offset into VRAM_A */
    top_bg2 = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
    bgSetPriority(top_bg2, 0);
    top_vram = (u8 *)bgGetGfxPtr(top_bg2);

    /* Bottom screen: 8bpp bitmap on BG2
     * mapBase=4 places bitmap at 64KB offset into VRAM_C */
    bot_bg2 = bgInitSub(2, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
    bgSetPriority(bot_bg2, 0);
    bot_vram = (u8 *)bgGetGfxPtr(bot_bg2);

    /* Point public framebuffer pointers at the shadow buffers in main
     * RAM. All draw code will now write here instead of into VRAM.
     * screen_flush() below DMAs these into top_vram / bot_vram. */
    top_fb = top_shadow;
    bot_fb = bot_shadow;

    /* Set palettes */
    setup_palette(BG_PALETTE);
    setup_palette(BG_PALETTE_SUB);

    /* Clear shadows to black (the draw target). */
    font_init();
    font_clear(top_fb, PAL_BG);
    font_clear(bot_fb, PAL_BG);

    /* Also clear the real VRAM bitmaps directly so the very first
     * displayed frame (before main loop runs screen_flush) is clean
     * instead of showing post-reset garbage. */
    dmaFillWords(0, top_vram, 256 * 192);
    dmaFillWords(0, bot_vram, 256 * 192);

    current_screen = SCREEN_PATTERN;
}

void screen_set_mode(ScreenMode mode)
{
    if (mode >= SCREEN_COUNT) return;
    current_screen = mode;
    /* Clear bottom shadow on mode switch — top is always pattern grid. */
    font_clear(bot_fb, PAL_BG);
    pattern_view_invalidate_bottom();
}

void screen_flush(void)
{
    /* Flush the shadow buffers out of the ARM9 data cache so the DMA
     * engine reads the most recent draw results from main RAM. Without
     * this, recent CPU writes may still be sitting in dcache and the
     * DMA would copy stale data to VRAM — showing the previous frame.
     *
     * We flush only the ranges we're about to copy. ARM9 dcache is
     * small (4 KB per way × 4 ways), so flushing 96 KB amounts to a
     * full cache walk but is still cheap (~microseconds). */
    DC_FlushRange(top_shadow, sizeof(top_shadow));
    DC_FlushRange(bot_shadow, sizeof(bot_shadow));

    /* Copy shadow → VRAM via DMA channel 3 (blocking). 48 KB per screen,
     * 12288 words, ~120 µs each on ARM9 — well inside the 4.4 ms VBlank
     * window, which is why this must be called from within VBlank. */
    dmaCopyWords(3, top_shadow, top_vram, sizeof(top_shadow));
    dmaCopyWords(3, bot_shadow, bot_vram, sizeof(bot_shadow));
}
