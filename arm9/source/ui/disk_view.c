/*
 * disk_view.c — Disk-screen flow.
 *
 * MAS load with error reporting. WAV load/save, MAS write, playback
 * stop, and undo reset are added in subsequent commits as those
 * subsystems land.
 */

#include "disk_view.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screen.h"
#include "font.h"
#include "filebrowser.h"
#include "navigation.h"
#include "song.h"
#include "editor_state.h"
#include "memtrack.h"
#include "mas_load.h"

/* ---- Shared globals ---- */
FileBrowser disk_browser;
bool        disk_browser_inited = false;
u8          sample_load_target  = 0;
u8          sample_save_target  = 0;
ScreenMode  disk_return_screen  = SCREEN_PROJECT;

/* ---- From main.c ---- */
extern char status_msg[64];
extern int  status_timer;
extern bool song_modified;
extern bool autosave_dirty;

void disk_view_cleanup(void)
{
    sample_load_target     = 0;
    sample_save_target     = 0;
}

void disk_view_input(u32 kd, u32 kh)
{
    if (navigation_handle_shift(kd, kh)) return;

    /* B at root exits the browser back to the opener. */
    if ((kd & KEY_B) && filebrowser_at_root(&disk_browser)) {
        disk_view_cleanup();
        screen_set_mode(disk_return_screen);
        font_clear(top_fb, PAL_BG);
        font_clear(bot_fb, PAL_BG);
        return;
    }

    if (filebrowser_input(&disk_browser, kd)) {
        const char *sel = filebrowser_selected_path(&disk_browser);
        int len = strlen(sel);

        if (len > 4 && strcmp(sel + len - 4, ".mas") == 0) {
            song_free();
            int err = mas_load(sel, &song);
            if (err == 0 || err == 1) {
                if (err == 1)
                    snprintf(status_msg, sizeof(status_msg),
                             "Partial load (RAM exhausted): %s",
                             sel);
                else
                    snprintf(status_msg, sizeof(status_msg),
                             "Loaded: %s", sel);
                cursor.order_pos = 0;
                cursor.row = 0;
                song_modified = false;
                autosave_dirty = false;
                status_timer = 180;
                screen_set_mode(SCREEN_PATTERN);
                font_clear(top_fb, PAL_BG);
            } else {
                const char *errstr = "unknown";
                switch (err) {
                case -1: errstr = "file not found"; break;
                case -2: errstr = "read error"; break;
                case -3: errstr = "not a MAS song"; break;
                case -4: errstr = "out of memory"; break;
                case -5: errstr = "parse error"; break;
                case -6: errstr = "MAS too big (won't fit in RAM)"; break;
                }
                snprintf(status_msg, sizeof(status_msg),
                         "Load failed: %s — started new module",
                         errstr);
                song_free();
                song_init();
                cursor.order_pos = 0;
                cursor.row = 0;
                song_modified  = false;
                autosave_dirty = false;
                status_timer = 240;
            }
        }
    }
}

void disk_view_draw(u8 *top, u8 *bot)
{
    if (disk_browser_inited)
        filebrowser_draw(&disk_browser, top);
    font_clear(bot, PAL_BG);
    font_fill_row(bot, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot, 0, 0, "DISK", PAL_TEXT);
    font_puts(bot, 6, 0, "A:load",
              PAL_DIM);
    if (status_timer > 0) {
        font_puts(bot, 0, 2, status_msg, PAL_WHITE);
    }
}
