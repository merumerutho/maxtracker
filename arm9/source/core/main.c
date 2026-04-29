/*
 * main.c — maxtracker ARM9 entry point.
 *
 * Entry point with pattern grid display, cursor navigation, input handling,
 * and real audio playback via maxmod/ARM7.
 */

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fat.h>
#include <maxmod9.h>
#include <sys/stat.h>

#include "mt_ipc.h"
#include "song.h"
#include "memtrack.h"
#include "screen.h"
#include "font.h"
#include "debug_view.h"
#include "pattern_view.h"
#include "instrument_view.h"
#include "mixer_view.h"
#include "sample_view.h"
#ifdef MAXTRACKER_LFE
#include "waveform_view.h"
#include "lfe_fx_view.h"
#endif
#include "song_view.h"

#ifdef MAXTRACKER_LFE
#include "lfe.h"
#endif
#include "clipboard.h"
#include "undo.h"
#include "navigation.h"
#include "disk_view.h"
#include "mas_write.h"
#include "playback.h"
#include "project_view.h"
#include "test.h"

/* Forward declarations for NitroFS (avoids header shadow with our own names) */
bool nitroFSInit(char **basepath);

/* Disk-screen state (disk_browser, sample_load/save_target,
 * disk_return_screen) lives in disk_view.c. */

/* Status message for bottom screen (e.g. "Saved: mysong.mas") */
char status_msg[64] = "";
int  status_timer = 0;

/* Filesystem: true if using NitroFS (emulator), false if FAT (flashcart) */
static bool using_nitrofs = false;

/* Root paths — relative to the .nds location (same as MAXMXDS). */
/* fs_browse_root is defined in filebrowser.c with its default;
 * we update it below after probing FAT vs NitroFS.
 * fs_save_path (./data/song.mas) lives in disk_view.c now. */
static const char *fs_backup_dir   = "./data/backup";
static const char *fs_backup_path  = "./data/backup/autosave.mas";

/* Song-modified flag (cleared only on manual save) */
bool song_modified = false;

/* Separate flag for auto-save (cleared on auto-save, set whenever song_modified is set) */
bool autosave_dirty = false;

/* Auto-save timer (counts frames; 18000 = 5 min at 60fps) */
#define AUTOSAVE_INTERVAL (5 * 60 * 60)
static int autosave_timer = 0;


/* Solo playback: when started from inside mode, only current channel plays */
bool solo_playback = false;

/* (A-held bitmask state removed — now using LGPT modifier-priority model) */

/* (Simulated playback removed — real playback via playback.h) */

/* get_current_pattern / get_current_pattern_idx → editor_state.c
 * note_slot_a_press / inst_slot_a_press → clipboard.c
 * get_contextual_instrument, handle_shift → navigation.c
 * pattern_view_input, cursor_advance, cursor_cell → pattern_view.c */

#include "mixer_view.h"

/* ================================================================== */
/* Centralized playback helpers                                        */
/*                                                                     */
/* LSDJ/M8-style playback: START = pattern loop (overview=all,        */
/* inside=solo), SELECT+START = song playback (full arrangement).     */
/* These helpers are called from per-view START handlers and from     */
/* navigation_handle_shift's SELECT+START case.                        */
/* ================================================================== */

void stop_playback_all(void)
{
    playback_stop();
    cursor.playing = false;
    play_mode = PLAY_STOPPED;

    /* Restore mixer mute states (solo may have overridden them). */
    for (int ch = 0; ch < MT_MAX_CHANNELS; ch++)
        playback_set_mute(ch, mixer_state.muted[ch]);
    solo_playback = false;
}

void start_pattern_loop(void)
{
    u8 pat_idx = editor_get_current_pattern_idx();

    if (cursor.inside) {
        /* Solo: mute all except cursor.channel */
        solo_playback = true;
        for (int ch = 0; ch < MT_MAX_CHANNELS; ch++)
            playback_set_mute(ch, ch != cursor.channel);
        play_mode = PLAY_PATTERN_SOLO;
    } else {
        /* All channels: respect mixer mutes */
        solo_playback = false;
        for (int ch = 0; ch < MT_MAX_CHANNELS; ch++)
            playback_set_mute(ch, mixer_state.muted[ch]);
        play_mode = PLAY_PATTERN_ALL;
    }

    playback_play_pattern_at(pat_idx, cursor.row);
    cursor.playing     = true;
    cursor.play_order  = cursor.order_pos;
    cursor.play_row    = cursor.row;
}

void start_song_playback(void)
{
    solo_playback = false;
    for (int ch = 0; ch < MT_MAX_CHANNELS; ch++)
        playback_set_mute(ch, mixer_state.muted[ch]);

    playback_play_at(cursor.order_pos, cursor.row);
    play_mode          = PLAY_SONG;
    cursor.playing     = true;
    cursor.play_order  = cursor.order_pos;
    cursor.play_row    = cursor.row;
}


int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Initialize screens */
    screen_init();

    /* Initialize the debug overlay early so dbg_log() works from any
     * subsystem init that runs below this point. */
    dbg_init();
    dbg_log("boot");

#ifdef MAXTRACKER_LFE
    /* The lfe library builds its wavetables and internal state here.
     * Every generator function (test tone, drawn, drum, synth) and
     * every FX entry point checks `lfe_is_initialized()` first and
     * returns LFE_ERR_NOT_INIT (-4) if this call was skipped — which
     * was the cause of the "generate returns rc=-4" failure before
     * this line was added. Safe to call multiple times (idempotent). */
    {
        lfe_status lfe_rc = lfe_init();
        dbg_log("lfe_init rc=%d ver=%s", (int)lfe_rc, lfe_version());
        if (lfe_rc != LFE_OK) {
            dbg_set_last_error("lfe_init failed rc=%d", (int)lfe_rc);
        }
    }
#endif

    /* Turn on master sound */
    fifoSendValue32(FIFO_SOUND, SOUND_MASTER_ENABLE);

    /* Init filesystem BEFORE maxmod — FAT/NitroFS must be set up before
     * mmInit() touches FIFO, as the FAT driver also uses FIFO/DMA and
     * initializing in the wrong order can break SD access on real hardware.
     *
     * Try FAT first (the common case on real hardware / flashcarts), then
     * fall back to NitroFS (emulators with embedded data). */
    if (fatInitDefault()) {
        using_nitrofs = false;
        fs_browse_root = "./data/";
    } else if (nitroFSInit(NULL)) {
        using_nitrofs = true;
        fs_browse_root = "./";
    }

    /* Init maxmod (no soundbank, we'll use mmPlayMAS later) */
    static mm_word mm_bank[1];
    mm_ds_system sys = {
        .mod_count    = 0,
        .samp_count   = 0,
        .mem_bank     = mm_bank,
        .fifo_channel = FIFO_MAXMOD,
    };
    mmInit(&sys);
    mmSelectMode(MM_MODE_C);

    /* Init song model */
    song_init();

    /* Init subsystems */
    undo_init();
    playback_init();

#ifdef UNIT_TESTING
    /* On-device tests: only hardware-specific tests that can't run on host.
     * Pure logic tests (song, clipboard, MAS roundtrip, etc.) are in host-test.
     * TODO: add VRAM rendering tests, playback/FIFO tests here. */
    consoleDemoInit();
    iprintf("On-device tests: none yet.\n");
    iprintf("Use 'make host-test' for logic tests.\n");
    while(1) swiWaitForVBlank();
#endif

    /* Place a few demo notes so the screen isn't empty */
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

        MT_CELL(pat, 0, 1)->note = 60; /* C-5 */
        MT_CELL(pat, 0, 1)->inst = 2;
        MT_CELL(pat, 4, 1)->note = 64; /* E-5 */
        MT_CELL(pat, 4, 1)->inst = 2;

        MT_CELL(pat, 0, 2)->note = NOTE_OFF;
        MT_CELL(pat, 2, 2)->note = 36; /* C-3 */
        MT_CELL(pat, 2, 2)->inst = 3;
        MT_CELL(pat, 2, 2)->fx   = 6;  /* F = porta up */
        MT_CELL(pat, 2, 2)->param = 0x20;
    }

    /* Initial draw */
    pattern_view_draw(top_fb);
    pattern_view_draw_bottom(bot_fb);

    /* Key repeat setup */
    ui_apply_key_repeat();   /* honors ui_repeat_delay / ui_repeat_rate */

    /* Main loop — double-buffered at 60 Hz.
     *
     * Drawing code writes to shadow framebuffers in main RAM (top_fb /
     * bot_fb now alias shadow_top / shadow_bot). screen_flush() DMA-
     * copies the shadows to the real VRAM bitmaps, and we call it right
     * after swiWaitForVBlank() so the DMA runs during VBlank while the
     * LCD is not reading VRAM — no scanout races, no tearing.
     *
     * CPU draws during the visible scanout period are harmless because
     * they land in RAM, not VRAM. */
    while (1) {
        scanKeys();

        /* Dispatch input and rendering by current screen */
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
            if (!navigation_handle_shift(kd, kh)) {
                /* START in song view = song playback (not pattern loop).
                 * This is handled here because song_view_input's own
                 * START handler will be removed (centralized below). */
                if (kd & KEY_START) {
                    if (cursor.playing)
                        stop_playback_all();
                    else
                        start_song_playback();
                } else {
                    song_view_input(kd, kh);
                    /* Order edits in song view (insert / clone / pattern-
                     * number nudge / B+A delete) all mutate song.orders[]
                     * or song.patt_count without touching the pattern-
                     * lifecycle hook. Refresh shared tables once per
                     * frame here — the call is a no-op when not playing,
                     * cheap (~1 KB copy + cache flush) when playing. */
                    playback_refresh_shared_tables();
                }
            }
            /* Draw only if navigation_handle_shift didn't transition us
             * elsewhere — otherwise we'd render the old view on top of
             * the already-cleared framebuffer for one frame, leaving
             * stale pixels outside the new view's fill region. */
            if (current_screen == SCREEN_SONG)
                song_view_draw(top_fb, bot_fb);
            break;

        case SCREEN_INSTRUMENT:
            if (!navigation_handle_shift(kd, kh)) {
                /* START = pattern loop (inherits inside/overview context) */
                if (kd & KEY_START) {
                    if (cursor.playing) stop_playback_all();
                    else                start_pattern_loop();
                } else {
                    instrument_view_input(kd, kh);
                }
            }
            if (current_screen == SCREEN_INSTRUMENT)
                instrument_view_draw(top_fb, bot_fb);
            break;

        case SCREEN_MIXER:
            if (!navigation_handle_shift(kd, kh)) {
                /* START in mixer = song playback from current position */
                if (kd & KEY_START) {
                    if (cursor.playing) stop_playback_all();
                    else                start_song_playback();
                } else {
                    mixer_view_input(kd, kh);
                }
            }
            if (current_screen == SCREEN_MIXER)
                mixer_view_draw(top_fb, bot_fb);
            break;

        case SCREEN_DISK:
            disk_view_input(kd, kh);
            if (current_screen == SCREEN_DISK)
                disk_view_draw(top_fb, bot_fb);
            break;

        case SCREEN_SAMPLE:
            if (!navigation_handle_shift(kd, kh)) {
                /* START = pattern loop (inherits inside/overview context) */
                if (kd & KEY_START) {
                    if (cursor.playing) stop_playback_all();
                    else                start_pattern_loop();
                } else {
                    sample_view_input(kd, kh);
                }
            }
            if (current_screen == SCREEN_SAMPLE)
                sample_view_draw(top_fb, bot_fb);
            break;

#ifdef MAXTRACKER_LFE
        case SCREEN_LFE:
            /* LFE handles its own SELECT chords (SEL+A commit, SEL+B
             * restore, SEL+X save-as-WAV) but navigation_handle_shift
             * still runs first so SELECT+LEFT navigates back and
             * SELECT+START triggers song playback. */
            if (!navigation_handle_shift(kd, kh)) {
                /* START = pattern loop from LFE too (X generates now) */
                if (kd & KEY_START) {
                    if (cursor.playing) stop_playback_all();
                    else                start_pattern_loop();
                } else {
                    waveform_view_input(kd, kh);
                }
            }
            /* If LFE was closed (B exit or navigation_handle_shift's SELECT+LEFT),
             * switch back to SCREEN_SAMPLE and draw it immediately so
             * there's no one-frame blank. */
            if (!waveform_view_is_active()) {
                screen_set_mode(SCREEN_SAMPLE);
                font_clear(top_fb, PAL_BG);
                sample_view_draw(top_fb, bot_fb);
            } else {
                waveform_view_draw(top_fb, bot_fb);
            }
            break;

        case SCREEN_LFE_FX:
            if (!navigation_handle_shift(kd, kh)) {
                if (kd & KEY_START) {
                    if (cursor.playing) stop_playback_all();
                    else                start_pattern_loop();
                } else {
                    lfe_fx_view_input(kd, kh);
                }
            }
            if (!lfe_fx_view_is_active()) {
                screen_set_mode(SCREEN_LFE);
                font_clear(top_fb, PAL_BG);
                waveform_view_draw(top_fb, bot_fb);
            } else {
                lfe_fx_view_draw(top_fb, bot_fb);
            }
            break;
#endif

        case SCREEN_PROJECT:
            if (!navigation_handle_shift(kd, kh))
                project_view_input(kd, kh);
            if (current_screen == SCREEN_PROJECT)
                project_view_draw(top_fb, bot_fb);
            break;

        default:
            navigation_handle_shift(kd, kh);
            /* (B no longer navigates — use SELECT+direction) */
            break;
        }

        /* Decrement status timer globally */
        if (status_timer > 0) status_timer--;

        /* Update playback state from ARM7 each frame */
        playback_update();
        if (cursor.playing) {
            cursor.play_row   = playback_get_row();
            cursor.play_order = playback_get_order();

            /* Follow mode: cursor tracks playback position */
            if (cursor.follow) {
                cursor.row       = cursor.play_row;
                cursor.order_pos = cursor.play_order;
            }

            /* Detect when ARM7 has stopped (song end) */
            if (!playback_is_playing()) {
                cursor.playing = false;
            }
        }

        /* Auto-save: every 5 minutes if song has been modified */
        autosave_timer++;
        if (autosave_timer >= AUTOSAVE_INTERVAL && autosave_dirty) {
            mkdir(fs_backup_dir, 0777);
            int as_err = mas_write(fs_backup_path,
                                   &song);
            if (as_err == 0) {
                snprintf(status_msg, sizeof(status_msg), "Auto-saved");
                autosave_dirty = false;
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Auto-save error: %d", as_err);
            }
            status_timer = 120; /* 2 sec at 60fps */
            autosave_timer = 0;
        } else if (autosave_timer >= AUTOSAVE_INTERVAL) {
            autosave_timer = 0;
        }

        /* Debug overlay: drawn last so it overlays whatever view ran
         * above. Frame counter ticks here so the overlay shows a fresh
         * number each frame. */
        dbg_frame_tick();
        dbg_draw_overlay(top_fb);

        /* Sync to VBlank, then DMA the shadow framebuffers into real
         * VRAM. Done inside VBlank so the LCD isn't reading VRAM while
         * DMA writes to it — this is what actually eliminates tearing. */
        swiWaitForVBlank();
        screen_flush();
    }

    return 0;
}
