/*
 * mixer_view.h — Channel mixer display and fader controls for maxtracker.
 *
 * Shows all 32 channels simultaneously on the bottom screen with
 * compact 2-column-per-channel vertical faders. The top screen shows
 * a detailed view of the currently selected channel.
 *
 * Volume range is 0-64 (matching the MAS/IT convention where 64 is
 * full volume and maxmod's internal cvolume cap).
 */

#ifndef MT_MIXER_VIEW_H
#define MT_MIXER_VIEW_H

#include <nds.h>
#include <stdbool.h>

/* Maximum volume for channel faders (matches maxmod's MAS convention). */
#define MIXER_VOL_MAX  64

/* Mixer state */
typedef struct {
    u8   selected_channel;           /* 0-31 absolute channel index */
    bool muted[32];                  /* per-channel mute state */
} MixerState;

extern MixerState mixer_state;

/* Draw both screens for mixer mode. */
void mixer_view_draw(u8 *top_fb, u8 *bot_fb);

/* Handle button input for the mixer screen. */
void mixer_view_input(u32 down, u32 held);

#endif /* MT_MIXER_VIEW_H */
