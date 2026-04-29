/*
 * lfe_fx_view.h — LFE Effects room.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Separate room to the right of the LFE synth view. Applies
 * destructive effects (distortion, filter, delay, envelope) to the
 * shared wv_draft buffer over a user-selected sample range.
 */

#ifndef MT_LFE_FX_VIEW_H
#define MT_LFE_FX_VIEW_H

#ifdef MAXTRACKER_LFE

#include <nds.h>
#include <stdbool.h>

void lfe_fx_view_open(void);
void lfe_fx_view_close(void);
bool lfe_fx_view_is_active(void);
void lfe_fx_view_input(u32 down, u32 held);
void lfe_fx_view_draw(u8 *top_fb, u8 *bot_fb);

#endif /* MAXTRACKER_LFE */

#endif /* MT_LFE_FX_VIEW_H */
