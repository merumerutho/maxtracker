#include <nds.h>
#include "song.h"
#include "screen.h"
#include "font.h"
#include "editor_state.h"
#include "navigation.h"
#include "pattern_view.h"
#include "disk_view.h"

bool song_modified  = false;
bool autosave_dirty = false;

char status_msg[64] = "";
int  status_timer   = 0;

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    song_init();
    screen_init();
    ui_apply_key_repeat();

    /* Place a few demo notes so the screen isn't empty */
    {
        MT_Pattern *pat = song.patterns[0];
        if (pat) {
            MT_CELL(pat, 0, 0)->note = 48; /* C-4 */
            MT_CELL(pat, 0, 0)->inst = 1;
            MT_CELL(pat, 2, 0)->note = 52; /* E-4 */
            MT_CELL(pat, 2, 0)->inst = 1;
            MT_CELL(pat, 4, 0)->note = 55; /* G-4 */
            MT_CELL(pat, 4, 0)->inst = 1;
            MT_CELL(pat, 6, 0)->note = 48; /* C-4 */
            MT_CELL(pat, 6, 0)->inst = 1;
        }
    }

    /* Initial draw */
    pattern_view_draw(top_fb);
    pattern_view_draw_bottom(bot_fb);

    while (1) {
        swiWaitForVBlank();

        scanKeys();
        u32 kd = keysDown();
        u32 kh = keysHeld();

        switch (current_screen) {
        case SCREEN_PATTERN:
            pattern_view_input(kd, kh);
            if (current_screen == SCREEN_PATTERN) {
                pattern_view_draw(top_fb);
                pattern_view_draw_bottom(bot_fb);
            }
            break;
        case SCREEN_DISK:
            disk_view_input(kd, kh);
            if (current_screen == SCREEN_DISK)
                disk_view_draw(top_fb, bot_fb);
            break;
        default:
            navigation_handle_shift(kd, kh);
            break;
        }

        if (status_timer > 0) status_timer--;

        screen_flush();
    }

    return 0;
}
