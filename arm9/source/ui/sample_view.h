/*
 * sample_view.h — Sample editor screen for maxtracker.
 *
 * Top screen:  Sample info + waveform display.
 * Bottom screen: Waveform drawing canvas (touchscreen) or load info.
 */

#ifndef MT_SAMPLE_VIEW_H
#define MT_SAMPLE_VIEW_H

#include <nds.h>
#include <stdbool.h>
#include "song.h"

/* Set the currently displayed sample (0-based index) */
void sample_view_set_selected(u8 index);

/* Get the currently displayed sample (0-based index) */
u8 sample_view_get_selected(void);

/* Draw both screens for sample mode */
void sample_view_draw(u8 *top_fb, u8 *bot_fb);

/* Process button input */
void sample_view_input(u32 down, u32 held);

#endif /* MT_SAMPLE_VIEW_H */
