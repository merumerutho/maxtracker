/*
 * disk_view.c — Disk-screen flow.
 *
 * WAV/MAS load, sample save with overwrite confirmation, save-as for the
 * current module, and routing back to the opener view when done. State
 * declared in disk_view.h is defined here; main.c used to own it.
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
#include "playback.h"
#include "editor_state.h"
#include "memtrack.h"
#include "pattern_view.h"
#include "sample_view.h"
#include "undo.h"
#include "mas_load.h"
#include "mas_write.h"
#include "wav_load.h"
#include "wav_save.h"
#ifdef MAXTRACKER_LFE
#include "waveform_view.h"
#endif

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

/* ---- Local state ---- */
static const char *fs_save_path = "./data/song.mas";
static char save_overwrite_path[128] = "";
static bool save_overwrite_pending   = false;

#define SAVE_ERR_NO_DATA   (-99)
#define SAVE_ERR_ALLOC     (-98)

static int mt_save_sample_wav(const char *path, u8 slot_idx)
{
    MT_Sample *s = &song.samples[slot_idx];
    if (!s->active || !s->pcm_data || s->length == 0)
        return SAVE_ERR_NO_DATA;

    u32 rate = s->base_freq ? s->base_freq : 22050;

    if (s->bits == 16) {
        return wav_save_mono16(path, (const s16 *)s->pcm_data,
                               s->length, rate);
    }

    s16 *tmp = (s16 *)malloc(s->length * sizeof(s16));
    if (!tmp) return SAVE_ERR_ALLOC;
    const s8 *src = (const s8 *)s->pcm_data;
    for (u32 i = 0; i < s->length; i++)
        tmp[i] = (s16)(src[i] << 8);
    int err = wav_save_mono16(path, tmp, s->length, rate);
    free(tmp);
    return err;
}

static int mt_compose_sample_save_path(char *out, size_t out_sz,
                                       const char *dir, u8 slot_idx)
{
    MT_Sample *s = &song.samples[slot_idx];
    const char *name_src;
    char fallback[16];

    if (s->name[0]) {
        name_src = s->name;
    } else {
        snprintf(fallback, sizeof(fallback), "sample_%02X",
                 (unsigned)(slot_idx + 1));
        name_src = fallback;
    }

    int dlen = (int)strlen(dir);
    const char *sep = (dlen > 0 && dir[dlen - 1] == '/') ? "" : "/";
    int n = snprintf(out, out_sz, "%s%s%s.wav", dir, sep, name_src);
    return (n > 0 && (size_t)n < out_sz) ? n : 0;
}

static bool mt_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

void disk_view_cleanup(void)
{
    sample_load_target     = 0;
    sample_save_target     = 0;
    save_overwrite_pending = false;
    save_overwrite_path[0] = '\0';
}

void disk_view_input(u32 kd, u32 kh)
{
    if (navigation_handle_shift(kd, kh)) return;

    /* ---- Overwrite-confirm modal ---- */
    if (save_overwrite_pending) {
        if (kd & KEY_A) {
            int err = mt_save_sample_wav(save_overwrite_path,
                                         sample_save_target - 1);
            if (err == WAV_SAVE_OK) {
                snprintf(status_msg, sizeof(status_msg),
                         "Saved: %s", save_overwrite_path);
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Save error: %d", err);
            }
            status_timer = 180;
            save_overwrite_pending = false;
            save_overwrite_path[0] = '\0';
            sample_save_target = 0;
            screen_set_mode(disk_return_screen);
            font_clear(top_fb, PAL_BG);
            font_clear(bot_fb, PAL_BG);
            return;
        }
        if (kd & KEY_B) {
            save_overwrite_pending = false;
            save_overwrite_path[0] = '\0';
            snprintf(status_msg, sizeof(status_msg),
                     "Save cancelled");
            status_timer = 120;
        }
        /* Any other input is ignored while the modal is up. */
        return;
    }

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
        if (len > 4 && (strcmp(sel + len - 4, ".wav") == 0 ||
                        strcmp(sel + len - 4, ".WAV") == 0)) {
            u8 *out_data = NULL;
            u32 out_len = 0;
            u32 out_rate = 0;
            u8  out_bits = 0;
            u8 smp_idx = (sample_load_target > 0)
                         ? (sample_load_target - 1)
                         : (cursor.instrument > 0
                            ? cursor.instrument - 1 : 0);
            MT_Sample *smp = &song.samples[smp_idx];

            u32 max_bytes = mt_mem_free_budget();
            if (smp->active && smp->pcm_data) {
                u32 bps = (smp->format == 1) ? 2 : 1;
                max_bytes += smp->length * bps;
            }

            int err = wav_load_ex(sel, max_bytes,
                                  &out_data, &out_len,
                                  &out_rate, &out_bits);
            if (err == WAV_OK && out_data) {
                if (playback_is_playing()) {
                    playback_stop();
                    cursor.playing = false;
                    play_mode = PLAY_STOPPED;
                }
#ifdef MAXTRACKER_LFE
                if (waveform_view_is_active())
                    waveform_view_close();
#endif

                if (smp->pcm_data) free(smp->pcm_data);

                smp->active         = true;
                smp->pcm_data       = out_data;
                smp->length         = out_len / (out_bits == 16 ? 2 : 1);
                smp->bits           = out_bits;
                smp->format         = (out_bits == 16) ? 1 : 0;
                smp->base_freq      = out_rate;
                smp->default_volume = 64;
                smp->panning        = 128;
                smp->global_volume  = 64;
                smp->loop_start     = 0;
                smp->loop_length    = 0;
                smp->loop_type      = 0;

                const char *fname = sel;
                for (const char *p = sel; *p; p++) {
                    if (*p == '/' || *p == '\\') fname = p + 1;
                }
                strncpy(smp->name, fname, sizeof(smp->name) - 1);
                smp->name[sizeof(smp->name) - 1] = '\0';
                char *dot = strrchr(smp->name, '.');
                if (dot) *dot = '\0';

                if (smp_idx + 1 > song.samp_count)
                    song.samp_count = smp_idx + 1;

                snprintf(status_msg, sizeof(status_msg),
                         "Loaded: %s", fname);
                mt_mark_song_modified();
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "WAV error: %s", wav_strerror(err));
            }
            status_timer = 180;
            if (sample_load_target > 0) {
                sample_view_set_selected(sample_load_target - 1);
                sample_load_target = 0;
                screen_set_mode(SCREEN_SAMPLE);
                font_clear(top_fb, PAL_BG);
                font_clear(bot_fb, PAL_BG);
            }
        } else if (len > 4 && strcmp(sel + len - 4, ".mas") == 0) {
            if (playback_is_playing()) {
                playback_stop();
                cursor.playing = false;
                play_mode = PLAY_STOPPED;
            }
            playback_detach_pattern();
            swiWaitForVBlank();
#ifdef MAXTRACKER_LFE
            if (waveform_view_is_active())
                waveform_view_close();
#endif
            int err = mas_load(sel, &song);
            if (err == 0 || err == 1) {
                if (err == 1)
                    snprintf(status_msg, sizeof(status_msg),
                             "Partial load (RAM exhausted, some patterns missing): %s",
                             sel);
                else
                    snprintf(status_msg, sizeof(status_msg),
                             "Loaded: %s", sel);
                cursor.order_pos = 0;
                cursor.row = 0;
                song_modified = false;
                autosave_dirty = false;
                status_timer = 180;
                undo_init();
                screen_set_mode(SCREEN_PATTERN);
                font_clear(top_fb, PAL_BG);
            } else if (err == MAS_LOAD_RESTORED) {
                snprintf(status_msg, sizeof(status_msg),
                         "Load failed, previous song kept");
                status_timer = 240;
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
                undo_init();
                cursor.order_pos = 0;
                cursor.row = 0;
                song_modified  = false;
                autosave_dirty = false;
                status_timer = 240;
            }
        }
    }
    if ((kh & KEY_Y) && (kd & KEY_X)) {
        /* Y+X = Save-As: find next available numbered filename */
        char sa_path[128];
        int sa_num = 1;
        for (; sa_num <= 99; sa_num++) {
            snprintf(sa_path, sizeof(sa_path),
                     "./data/song_%02d.mas", sa_num);
            FILE *f = fopen(sa_path, "r");
            if (!f) break;
            fclose(f);
        }
        if (sa_num <= 99) {
            int err = mas_write(sa_path, &song);
            if (err == 0) {
                snprintf(status_msg, sizeof(status_msg),
                         "Saved: song_%02d.mas", sa_num);
                song_modified = false;
                autosave_dirty = false;
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Save error: %d", err);
            }
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "Save-As: no free slot (01-99)");
        }
        status_timer = 180;
    } else if (kd & KEY_X) {
        int err = mas_write(fs_save_path, &song);
        if (err == 0) {
            snprintf(status_msg, sizeof(status_msg), "Saved OK");
            song_modified = false;
            autosave_dirty = false;
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "Save error: %d", err);
        }
        status_timer = 180;
    }
    /* ---- Save-request: user pressed START in SAVE mode ---- */
    if (sample_save_target > 0 &&
        filebrowser_take_save_request(&disk_browser)) {
        char target[FB_PATH_LEN];
        int n = mt_compose_sample_save_path(target, sizeof(target),
                                            disk_browser.path,
                                            sample_save_target - 1);
        if (n == 0) {
            snprintf(status_msg, sizeof(status_msg),
                     "Save: path too long");
            status_timer = 180;
        } else if (mt_file_exists(target)) {
            strncpy(save_overwrite_path, target,
                    sizeof(save_overwrite_path) - 1);
            save_overwrite_path[sizeof(save_overwrite_path)-1] = '\0';
            save_overwrite_pending = true;
        } else {
            int err = mt_save_sample_wav(target,
                                         sample_save_target - 1);
            if (err == WAV_SAVE_OK) {
                snprintf(status_msg, sizeof(status_msg),
                         "Saved: %s", target);
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Save error: %d", err);
            }
            status_timer = 180;
            sample_save_target = 0;
            screen_set_mode(disk_return_screen);
            font_clear(top_fb, PAL_BG);
            font_clear(bot_fb, PAL_BG);
            return;
        }
    }
}

void disk_view_draw(u8 *top, u8 *bot)
{
    if (disk_browser_inited)
        filebrowser_draw(&disk_browser, top);
    font_clear(bot, PAL_BG);
    font_fill_row(bot, 0, 0, FONT_COLS, PAL_HEADER_BG);
    if (save_overwrite_pending) {
        font_puts(bot, 0, 0, "OVERWRITE?", PAL_ORANGE);
        font_puts(bot, 1, 2, save_overwrite_path, PAL_WHITE);
        font_puts(bot, 1, 4,
                  "File exists. A = overwrite, B = cancel",
                  PAL_TEXT);
    } else if (disk_browser.mode == FB_MODE_SAVE) {
        font_puts(bot, 0, 0, "DISK — SAVE SAMPLE", PAL_ORANGE);
        font_puts(bot, 1, 2,
                  "A: enter folder   B: back/exit",
                  PAL_DIM);
        font_puts(bot, 1, 3,
                  "START: save here",
                  PAL_WHITE);
    } else {
        font_puts(bot, 0, 0, "DISK", PAL_TEXT);
        font_puts(bot, 6, 0, "A:load X:save Y+X:saveAs",
                  PAL_DIM);
    }
    if (status_timer > 0) {
        int row = save_overwrite_pending ? 6 : 2;
        font_puts(bot, 0, row, status_msg, PAL_WHITE);
    }
}
