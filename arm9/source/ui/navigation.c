/*
 * navigation.c — Global SELECT-chord navigation dispatcher.
 *
 * Extracted from main.c — pure refactoring, zero behavioral change.
 * See navigation.h for the public API and design notes.
 *
 * This is the initial version with screen-switching only.
 * Clipboard (SELECT+A/B), playback (SELECT+START), and undo
 * integration are added in subsequent commits as those subsystems
 * land.
 */

#include "navigation.h"

#include <nds.h>
#include <stdbool.h>

#include "song.h"
#include "screen.h"
#include "font.h"
#include "editor_state.h"
#include "disk_view.h"
#include "sample_view.h"
#ifdef MAXTRACKER_LFE
#include "waveform_view.h"
#include "lfe_fx_view.h"
#endif

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
 *   Song <-> Pattern Overview <-> Inside Column <-> Instrument <-> Sample [<-> LFE <-> LFE FX]
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
            screen_set_mode(SCREEN_INSTRUMENT);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#ifdef MAXTRACKER_LFE
        case SCREEN_LFE:
            waveform_view_close();
            if (!waveform_view_is_active()) {
                screen_set_mode(SCREEN_SAMPLE);
                font_clear(top_fb, PAL_BG);
            }
            handled = true;
            break;
        case SCREEN_LFE_FX:
            lfe_fx_view_close();
            screen_set_mode(SCREEN_LFE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#endif
        case SCREEN_INSTRUMENT:
            screen_set_mode(SCREEN_PATTERN);
            cursor.inside = true;
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_PROJECT:
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
                cursor.inside = true;
                cursor.column = 0;
            } else {
                cursor.instrument = get_contextual_instrument();
                screen_set_mode(SCREEN_INSTRUMENT);
            }
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_INSTRUMENT:
            screen_set_mode(SCREEN_SAMPLE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
#ifdef MAXTRACKER_LFE
        case SCREEN_SAMPLE:
            waveform_view_open(sample_view_get_selected());
            screen_set_mode(SCREEN_LFE);
            font_clear(top_fb, PAL_BG);
            handled = true;
            break;
        case SCREEN_LFE:
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
    if (down & KEY_UP) {
        if (current_screen == SCREEN_SONG) {
            screen_set_mode(SCREEN_PROJECT);
            font_clear(top_fb, PAL_BG);
            handled = true;
        } else if (current_screen != SCREEN_MIXER &&
                   current_screen != SCREEN_PROJECT) {
            nav_vertical_return = current_screen;
            screen_set_mode(SCREEN_MIXER);
            font_clear(top_fb, PAL_BG);
            handled = true;
        }
    }
    if (down & KEY_DOWN) {
        if (current_screen == SCREEN_PROJECT) {
            screen_set_mode(SCREEN_SONG);
            font_clear(top_fb, PAL_BG);
            handled = true;
        } else if (current_screen == SCREEN_MIXER) {
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

    return handled;
}
