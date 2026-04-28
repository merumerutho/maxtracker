#include "screen.h"
#include <string.h>

static u8 top_shadow[256 * 192] __attribute__((aligned(32)));
static u8 bot_shadow[256 * 192] __attribute__((aligned(32)));

static u8 *top_vram = NULL;
static u8 *bot_vram = NULL;

u8 *top_fb = NULL;
u8 *bot_fb = NULL;

ScreenMode current_screen = SCREEN_PATTERN;

static int top_bg2 = -1;
static int bot_bg2 = -1;

static void setup_palette(vu16 *pal)
{
    pal[PAL_BG]         = RGB15( 0,  0,  1);
    pal[PAL_ROW_EVEN]   = RGB15( 3,  3,  6);
    pal[PAL_ROW_BEAT]   = RGB15( 4,  5,  9);
    pal[PAL_ROW_CURSOR] = RGB15( 4,  7, 16);
    pal[PAL_DIM]        = RGB15(10, 10, 10);
    pal[PAL_GRAY]       = RGB15(18, 18, 18);
    pal[PAL_TEXT]        = RGB15(25, 25, 25);
    pal[PAL_WHITE]      = RGB15(31, 31, 31);
    pal[PAL_NOTE]       = RGB15( 0, 31, 22);
    pal[PAL_INST]       = RGB15(31, 28,  6);
    pal[PAL_EFFECT]     = RGB15(14, 20, 31);
    pal[PAL_PARAM]      = RGB15(28, 16, 31);
    pal[PAL_PLAY]       = RGB15( 8, 31,  8);
    pal[PAL_RED]        = RGB15(31,  6,  6);
    pal[PAL_ORANGE]     = RGB15(31, 20,  0);
    pal[PAL_PLAY_BG]    = RGB15( 4, 10,  4);
    pal[PAL_HEADER_BG]  = RGB15( 6,  4,  8);
    pal[PAL_SEL_BG]     = RGB15(10,  8,  2);
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
    powerOn(POWER_ALL_2D);

    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG);

    videoSetModeSub(MODE_5_2D | DISPLAY_BG2_ACTIVE);
    vramSetBankC(VRAM_C_SUB_BG);

    top_bg2 = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
    bgSetPriority(top_bg2, 0);
    top_vram = (u8 *)bgGetGfxPtr(top_bg2);

    bot_bg2 = bgInitSub(2, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
    bgSetPriority(bot_bg2, 0);
    bot_vram = (u8 *)bgGetGfxPtr(bot_bg2);

    top_fb = top_shadow;
    bot_fb = bot_shadow;

    setup_palette(BG_PALETTE);
    setup_palette(BG_PALETTE_SUB);

    memset(top_shadow, PAL_BG, sizeof(top_shadow));
    memset(bot_shadow, PAL_BG, sizeof(bot_shadow));

    dmaFillWords(0, top_vram, 256 * 192);
    dmaFillWords(0, bot_vram, 256 * 192);

    current_screen = SCREEN_PATTERN;
}

void screen_set_mode(ScreenMode mode)
{
    if (mode >= SCREEN_COUNT) return;
    current_screen = mode;
    memset(bot_shadow, PAL_BG, sizeof(bot_shadow));
}

void screen_flush(void)
{
    DC_FlushRange(top_shadow, sizeof(top_shadow));
    DC_FlushRange(bot_shadow, sizeof(bot_shadow));

    dmaCopyWords(3, top_shadow, top_vram, sizeof(top_shadow));
    dmaCopyWords(3, bot_shadow, bot_vram, sizeof(bot_shadow));
}
