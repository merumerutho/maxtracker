/*
 * text_input.h — Modal on-screen QWERTY keyboard for maxtracker.
 *
 * Edits a caller-owned character buffer in place. While active, the
 * keyboard owns both screens: top shows the title and the current
 * buffer with a caret, bottom shows the QWERTY grid. A d-pad-navigated
 * cursor highlights one key at a time; A presses it. Special keys
 * (DEL, SPACE, OK, CANCEL) are drawn wider at the bottom of the grid.
 *
 * A call sequence looks like:
 *
 *   text_input_open(song.name, 32, "Rename Song");
 *   // each frame while the view is up:
 *   if (text_input_is_active()) {
 *       text_input_input(down, held);
 *       text_input_draw(top_fb, bot_fb);
 *       return;
 *   }
 *
 * When the user picks OK or CANCEL the widget closes and
 * text_input_is_active() returns false. On CANCEL the buffer is
 * restored from a snapshot taken at open time; on OK the buffer keeps
 * whatever the user typed (already in place since edits are live).
 *
 * The widget allocates nothing and stores state in a file-static
 * struct — only one instance can be open at a time.
 */

#ifndef MT_TEXT_INPUT_H
#define MT_TEXT_INPUT_H

#include <nds.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Open the keyboard on the given buffer.
 *
 *   buf      — caller-owned, NUL-terminated, writable for max_len+1 bytes
 *   max_len  — maximum string length (NOT including the NUL)
 *   title    — short label drawn in the header row (may be NULL)
 *
 * No-op if a keyboard is already active.
 */
void text_input_open(char *buf, size_t max_len, const char *title);

/* True while the keyboard is capturing input. */
bool text_input_is_active(void);

/* Feed one frame's worth of key input. */
void text_input_input(u32 down, u32 held);

/* Render the keyboard onto both framebuffers. No-op when inactive. */
void text_input_draw(u8 *top_fb, u8 *bot_fb);

/*
 * Draw-side companion to `is_active`. Returns true if the keyboard
 * owns the draw this frame — which is either (a) currently active, or
 * (b) was active last frame and just closed (one-shot framebuffer
 * clear so leftover key cells don't ghost into the view underneath).
 *
 * Recommended pattern in view draws:
 *
 *   if (text_input_draw_and_consume(top_fb, bot_fb))
 *       return;   // keyboard owned this frame
 *   // normal view draw
 *
 * Without the one-shot clear, views with partial-row draws (like
 * project_view's draw_bottom) leave keyboard artifacts on rows the
 * view doesn't repaint.
 */
bool text_input_draw_and_consume(u8 *top_fb, u8 *bot_fb);

#endif /* MT_TEXT_INPUT_H */
