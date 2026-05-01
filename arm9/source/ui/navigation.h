/*
 * navigation.h — Global SELECT-chord navigation dispatcher.
 *
 * Implements LSDJ "rooms in a house" connected navigation:
 *   Horizontal (SELECT+LEFT/RIGHT): Song <-> Pattern <-> Inside <-> Instrument <-> Sample
 *   Vertical   (SELECT+UP/DOWN):    Mixer / Project (reversible)
 *   SELECT+START:                    Song playback
 *   SELECT+B/A:                     Selection mode / clipboard paste
 *
 * Extracted from main.c — pure refactoring, zero behavioral change.
 */

#ifndef MT_NAVIGATION_H
#define MT_NAVIGATION_H

#include <nds.h>
#include <stdbool.h>

/* Configurable shift button (LSDJ-style).
 * Now backed by the keybind system — resolves via mt_keymap[]. */
#include "keybind.h"
#define MT_SHIFT_KEY MT_KEY_SHIFT

/*
 * Global SELECT-chord handler. Returns true ONLY if it actually
 * consumed the input — e.g. a navigation combo landed on a matching
 * screen case, or a pattern-view clipboard action actually ran. When
 * SELECT is held but no bound combo matches the current context, it
 * returns false so the per-view input function can claim the event
 * for its own local SELECT chord.
 */
bool navigation_handle_shift(u32 down, u32 held);

#endif /* MT_NAVIGATION_H */
