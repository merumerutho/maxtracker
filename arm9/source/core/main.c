#include <nds.h>
#include "song.h"
#include "screen.h"
#include "font.h"
#include "editor_state.h"
#include "navigation.h"
#include "pattern_view.h"
#include "song_view.h"
#include "mixer_view.h"
#include "project_view.h"
#include "instrument_view.h"
#include "sample_view.h"
#include "disk_view.h"
#include "undo.h"
#include "playback.h"
#include "debug_view.h"

bool song_modified  = false;
bool autosave_dirty = false;

char status_msg[64] = "";
int  status_timer   = 0;

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    song_init();
    undo_init();
    playback_init();
    screen_init();
    dbg_init();
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
        case SCREEN_SONG:
            if (!navigation_handle_shift(kd, kh))
                song_view_input(kd, kh);
            if (current_screen == SCREEN_SONG)
                song_view_draw(top_fb, bot_fb);
            break;
        case SCREEN_MIXER:
            if (!navigation_handle_shift(kd, kh))
                mixer_view_input(kd, kh);
            if (current_screen == SCREEN_MIXER)
                mixer_view_draw(top_fb, bot_fb);
            break;
        case SCREEN_PROJECT:
            if (!navigation_handle_shift(kd, kh))
                project_view_input(kd, kh);
            if (current_screen == SCREEN_PROJECT)
                project_view_draw(top_fb, bot_fb);
            break;
        case SCREEN_INSTRUMENT:
            if (!navigation_handle_shift(kd, kh))
                instrument_view_input(kd, kh);
            if (current_screen == SCREEN_INSTRUMENT)
                instrument_view_draw(top_fb, bot_fb);
            break;
        case SCREEN_SAMPLE:
            if (!navigation_handle_shift(kd, kh))
                sample_view_input(kd, kh);
            if (current_screen == SCREEN_SAMPLE)
                sample_view_draw(top_fb, bot_fb);
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

        playback_update();
        if (cursor.playing) {
            cursor.play_row   = playback_get_row();
            cursor.play_order = playback_get_order();

            if (cursor.follow) {
                cursor.row       = cursor.play_row;
                cursor.order_pos = cursor.play_order;
            }

            if (!playback_is_playing())
                cursor.playing = false;
        }

        dbg_frame_tick();
        dbg_draw_overlay(top_fb);

        screen_flush();
    }

    return 0;
}
