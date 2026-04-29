/*
 * song_view.h — Song arrangement view (order table editor).
 *
 * Renders the song arrangement grid and handles order-table editing:
 * pattern assignment, cloning, deletion, and navigation.
 */

#ifndef MT_SONG_VIEW_H
#define MT_SONG_VIEW_H

#include <nds.h>

void song_view_draw(u8 *top_fb, u8 *bot_fb);
void song_view_input(u32 down, u32 held);

/* Paste the order clipboard at cursor (insert mode — shifts existing
 * entries down). Called from navigation.c on SELECT+A in SCREEN_SONG.
 * Returns true if paste happened. */
bool song_view_paste_orders(void);

/* True if the order clipboard has content. */
bool song_view_has_clip(void);

#endif /* MT_SONG_VIEW_H */
