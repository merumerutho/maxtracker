/*
 * instrument_view.h — Instrument parameter editor (top screen)
 *                     and envelope visualization (bottom screen).
 */

#ifndef MT_INSTRUMENT_VIEW_H
#define MT_INSTRUMENT_VIEW_H

#include <nds.h>
#include <stdbool.h>
#include "song.h"

/* ------------------------------------------------------------------ */
/* View API                                                            */
/* ------------------------------------------------------------------ */

/* Draw both screens for instrument mode */
void instrument_view_draw(u8 *top_fb, u8 *bot_fb);

/* Process button input (called each frame while in instrument mode) */
void instrument_view_input(u32 down, u32 held);

#endif /* MT_INSTRUMENT_VIEW_H */
