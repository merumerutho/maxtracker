/*
 * navigation.c — Global SELECT-chord navigation dispatcher.
 *
 * Extracted from main.c — pure refactoring, zero behavioral change.
 * See navigation.h for the public API and design notes.
 */

#include "navigation.h"

#include <nds.h>
#include <stdbool.h>

#include "song.h"
#include "screen.h"
#include "font.h"
#include "editor_state.h"
#include "clipboard.h"
#include "disk_view.h"
#include "sample_view.h"
#include "song_view.h"

#include <stdio.h>
#ifdef MAXTRACKER_LFE
#include "waveform_view.h"
#include "lfe_fx_view.h"
#endif
#include "clipboard.h"
#include "undo.h"
#include "playback.h"

/* --- Externs from main.c --- */
extern char        status_msg[64];
extern int         status_timer;
extern ScreenMode  disk_return_screen;
extern u8          sample_load_target;

/* Pattern helpers now in editor_state.c — accessed via editor_state.h */

/* Navigation: track the screen we came from for UP/DOWN reversibility */
static ScreenMode nav_vertical_return = SCREEN_PATTERN;

/*
 * Get the instrument number at the cursor row, scanning upward if the
 * current row has no instrument set (inst == 0). Returns 1-based
 * instrument number, defaulting to 1 if none found.
 */
static u8 get_contextual_instrument(void)
{
    int nrows;
    MT_Pattern *pat = editor_get_current_pattern(&nrows);
    if (!pat) return cursor.instrument ? cursor.instrument : 1;

    for (int r = cursor.row; r >= 0; r--) {
        u8 inst = MT_CELL(pat, r, cursor.channel)->inst;
        if (inst > 0) return inst;
    }
    return cursor.instrument ? cursor.instrument : 1;
}

/*
 * SHIFT (SELECT) handler — LSDJ "rooms in a house" connected navigation.
 *
 * Horizontal chain (SELECT+LEFT/RIGHT):
 *   Song <-> Pattern Overview <-> Inside Column <-> Instrument <-> Sample
 *
 * Each RIGHT step goes deeper with context:
 *   Song       -> Pattern at cursor order position
 *   Overview   -> Inside on highlighted channel
 *   Inside     -> Instrument of the note at cursor (scans up for last inst)
 *   Instrument -> Sample of that instrument's assigned sample
 *
 * Each LEFT step reverses:
 *   Sample     -> Instrument
 *   Instrument -> Inside (returns to pattern)
 *   Inside     -> Overview
 *   Overview   -> Song
 *
 * Vertical (SELECT+UP/DOWN): reversible.
 *   UP = Mixer (from any screen, remembers return screen)
 *   DOWN = return to where UP came from
 *
 * SELECT+START = Song playback (all screens except Disk)
 * SELECT+A/B   = Copy/Paste
 */
/*
 * Global SELECT-chord handler. Returns true ONLY if it actually
 * consumed the input — e.g. a navigation combo landed on a matching
 * screen case, or a pattern-view clipboard action actually ran. When
 * SELECT is held but no bound combo matches the current context, it
 * returns false so the per-view input function can claim the event
 * for its own local SELECT chord (e.g. SELECT+B "restore original"
 * inside the waveform editor).
 *
 * Design rule: each `if (down & MT_KEY_*)` block must set `handled` only
 * in branches that actually perform the action. Adding a no-op branch
 * (like "default: break;" in a nav switch) must NOT mark handled.
 */
bool navigation_handle_shift(u32 down, u32 held)
{
    if (!((held | down) & MT_SHIFT_KEY)) return false;

    bool handled = false;

    /* --- Horizontal: LEFT = back, RIGHT = deeper --- */
    if (down & KEY_LEFT) {
        switch (current_screen) {
        case SCREEN_PATTERN:
            cursor.selecting = false;
            if (cursor.inside) {
                cursor.inside = false;
            } else {
                screen_set_mode(SCREEN_SONG);
            }
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_SAMPLE:
            /* Sample -> Instrument */
            screen_set_mode(SCREEN_INSTRUMENT);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#ifdef MAXTRACKER_LFE
        case SCREEN_LFE:
            /* LFE -> Sample. waveform_view_close respects the dirty-
             * draft confirmation: if the draft has uncommitted work,
             * the first call sets exit_pending and returns without
             * actually closing. We only navigate to SAMPLE if close
             * actually deactivated the editor. */
            waveform_view_close();
            if (!waveform_view_is_active()) {
                screen_set_mode(SCREEN_SAMPLE);
                font_clear(top_fb, PAL_BG);
            }
            handled = true;
            break;
        case SCREEN_LFE_FX:
            /* LFE FX -> LFE synth */
            lfe_fx_view_close();
            screen_set_mode(SCREEN_LFE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#endif
        case SCREEN_INSTRUMENT:
            /* Instrument -> back to Inside column */
            screen_set_mode(SCREEN_PATTERN);
            cursor.inside = true;
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_PROJECT:
            /* Project -> back to Song */
            screen_set_mode(SCREEN_SONG);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_MIXER:
            screen_set_mode(nav_vertical_return);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_DISK:
            disk_view_cleanup();
            screen_set_mode(disk_return_screen);
            font_clear(top_fb, PAL_BG);
            font_clear(bot_fb, PAL_BG);
            handled = true;
            break;
        default:
            break;
        }
    }
    if (down & KEY_RIGHT) {
        switch (current_screen) {
        case SCREEN_SONG:
            /* Song -> Pattern overview */
            cursor.selecting = false;
            screen_set_mode(SCREEN_PATTERN);
            cursor.inside = false;
            {
                MT_Pattern *p = song.patterns[song.orders[cursor.order_pos]];
                int nr = p ? p->nrows : 64;
                if (cursor.row >= nr) cursor.row = nr - 1;
            }
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_PATTERN:
            cursor.selecting = false;
            if (!cursor.inside) {
                /* Overview -> Inside column */
                cursor.inside = true;
                cursor.column = 0;
            } else {
                /* Inside -> Instrument (contextual) */
                cursor.instrument = get_contextual_instrument();
                screen_set_mode(SCREEN_INSTRUMENT);
            }
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_INSTRUMENT: {
            /* Instrument -> Sample (of instrument's sample) */
            u8 idx = cursor.instrument > 0 ? cursor.instrument - 1 : 0;
            u8 smp = song.instruments[idx].sample;
            sample_view_set_selected(smp > 0 ? smp - 1 : 0);
            screen_set_mode(SCREEN_SAMPLE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        }
#ifdef MAXTRACKER_LFE
        case SCREEN_SAMPLE:
            /* Sample -> LFE (waveform editor for current sample) */
            waveform_view_open(sample_view_get_selected());
            screen_set_mode(SCREEN_LFE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_LFE:
            /* LFE synth -> LFE FX */
            lfe_fx_view_open();
            screen_set_mode(SCREEN_LFE_FX);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#endif
        default:
            break;
        }
    }

    /* --- Vertical: contextual UP/DOWN --- */
    /*   Song level:    UP/DOWN = Project <-> Song
     *   Pattern level: UP/DOWN = Mixer   <-> Pattern
     *   Disk/other:    DOWN    = return to previous screen
     */
    if (down & KEY_UP) {
        if (current_screen == SCREEN_SONG) {
            /* Song -> Project */
            screen_set_mode(SCREEN_PROJECT);
            font_clear(top_fb, PAL_BG);
            handled = true;
        } else if (current_screen != SCREEN_MIXER &&
                   current_screen != SCREEN_PROJECT) {
            /* Any other (Pattern, Instrument, Sample) -> Mixer */
            nav_vertical_return = current_screen;
            screen_set_mode(SCREEN_MIXER);
            font_clear(top_fb, PAL_BG);
            handled = true;
        }
    }
    if (down & KEY_DOWN) {
        if (current_screen == SCREEN_PROJECT) {
            /* Project -> Song */
            screen_set_mode(SCREEN_SONG);
            font_clear(top_fb, PAL_BG);
            handled = true;
        } else if (current_screen == SCREEN_MIXER) {
            /* Mixer -> return to previous screen */
            screen_set_mode(nav_vertical_return);
            font_clear(top_fb, PAL_BG);
            handled = true;
        } else if (current_screen == SCREEN_DISK) {
            disk_view_cleanup();
            screen_set_mode(disk_return_screen);
            font_clear(top_fb, PAL_BG);
            font_clear(bot_fb, PAL_BG);
            handled = true;
        }
    }

    /* --- SELECT+START --- */
    if (down & MT_KEY_START) {
        if (current_screen != SCREEN_DISK) {
            /* Song playback (full arrangement
             * from current order position). If already playing, stop
             * and immediately restart — user's spec for SELECT+START
             * while playing is "interrupt and restart", not toggle. */
            if (cursor.playing) {
                playback_stop();
                cursor.playing = false;
                play_mode = PLAY_STOPPED;
            }
            /* Restore mixer mutes and start song mode. Can't call
             * the centralized helpers directly (they're static in
             * main.c), so we inline the essential logic here. */
            for (int ch = 0; ch < MT_MAX_CHANNELS; ch++)
                playback_set_mute(ch, false);  /* clear solo overrides */
            playback_play(cursor.order_pos);
            play_mode       = PLAY_SONG;
            cursor.playing  = true;
            cursor.play_order = cursor.order_pos;
            cursor.play_row   = 0;
            handled = true;
        }
    }

    /*
     * M8-style clipboard (SHIFT=SELECT, OPTION=B, EDIT=A):
     *
     * SELECT+B (SHIFT+OPTION): Enter selection mode (pattern view only)
     * SELECT+A (SHIFT+EDIT):   Paste clipboard at cursor (pattern view)
     *
     * In selection mode (handled in pattern input, not here):
     *   B alone (OPTION):      Copy selection, exit selection mode
     *   A+B (EDIT+OPTION):     Cut selection (copy + clear), exit
     */

    /* SELECT+B = enter selection mode (pattern or song view). Only claims
     * the event when not already selecting — otherwise passes through to
     * the active view (e.g. LFE binds SELECT+B as "restore original"). */
    if (down & MT_KEY_BACK) {
        if (current_screen == SCREEN_PATTERN && !cursor.selecting) {
            cursor.selecting = true;
            cursor.sel_start_row = cursor.row;
            cursor.sel_start_col = cursor.channel;
            handled = true;
        } else if (current_screen == SCREEN_SONG && !cursor.selecting) {
            cursor.selecting = true;
            cursor.sel_start_row = cursor.order_pos;
            handled = true;
        } else if (current_screen == SCREEN_INSTRUMENT) {
            u8 idx = cursor.instrument > 0 ? cursor.instrument - 1 : 0;
            clipboard_copy_instrument(idx);
            snprintf(status_msg, sizeof(status_msg),
                     "Copied instrument %02X", idx + 1);
            status_timer = 90;
            handled = true;
        }
    }

    /* SELECT+A = paste clipboard at cursor (pattern block or order entries).
     * Only claims the event when a paste actually happened. */
    if (down & MT_KEY_CONFIRM) {
        if (current_screen == SCREEN_PATTERN) {
            int nrows;
            MT_Pattern *pat = editor_get_current_pattern(&nrows);
            if (pat && clipboard_has_block()) {
                u8 pi = editor_get_current_pattern_idx();
                u16 paste_rows = clipboard.cell_rows;
                u16 paste_ch   = clipboard.cell_channels;
                u16 end_row = cursor.row + paste_rows - 1;
                u16 end_ch  = cursor.channel + paste_ch - 1;
                if (end_row >= pat->nrows) end_row = pat->nrows - 1;
                if (end_ch >= pat->ncols) end_ch = pat->ncols - 1;
                undo_push_block(pi, cursor.row, end_row, cursor.channel, end_ch);
                clipboard_paste(pat, cursor.row, cursor.channel);
                mt_mark_song_modified();
                handled = true;
            }
            cursor.selecting = false;
        } else if (current_screen == SCREEN_SONG) {
            if (song_view_paste_orders()) handled = true;
            cursor.selecting = false;
        } else if (current_screen == SCREEN_INSTRUMENT) {
            u8 idx = cursor.instrument > 0 ? cursor.instrument - 1 : 0;
            if (clipboard_paste_instrument(idx)) {
                mt_mark_song_modified();
                snprintf(status_msg, sizeof(status_msg),
                         "Pasted to instrument %02X", idx + 1);
                status_timer = 90;
                handled = true;
            }
        }
    }

    return handled;
}
