/*
 * text_input.c — Modal on-screen QWERTY keyboard.
 *
 * Layout (5 rows of keys, 10 cells wide):
 *
 *   Row 0:  1 2 3 4 5 6 7 8 9 0
 *   Row 1:  Q W E R T Y U I O P
 *   Row 2:  A S D F G H J K L -
 *   Row 3:  Z X C V B N M . _ /
 *   Row 4:  [DEL ][SPACE ][CANCL][ OK ]        (each special key = ~2-3 cells)
 *
 * Uppercase only; SHIFT is intentionally omitted because maxtracker's
 * name fields are style-guide uppercase and keeping the layout simple
 * avoids a second character table. Lowercase / punctuation can be
 * added later if the project needs it.
 *
 * Navigation:
 *   D-pad    — move cursor (wraps at edges)
 *   A        — press highlighted key
 *   B        — shortcut for DEL (backspace) while any regular key is
 *              focused; on the special-row it cancels (same as CANCEL)
 *   START    — shortcut for OK
 *   SELECT   — shortcut for CANCEL
 *
 * The caller owns the buffer. Edits land directly; CANCEL restores
 * from a file-static snapshot captured at open time.
 */

#include "text_input.h"
#include "font.h"
#include "screen.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Layout constants                                                    */
/* ------------------------------------------------------------------ */

#define KB_ROWS         5               /* 4 char rows + 1 special row */
#define KB_COLS         10              /* regular-key columns         */
#define KB_KEY_W        4               /* text cells per key          */
#define KB_KEY_H        2               /* text rows per key           */
#define KB_X_OFFSET     12              /* left margin on bot screen   */
#define KB_Y_OFFSET     10              /* top margin on bot screen    */

#define NAME_MAX        64              /* widget-internal snapshot cap */

/* Regular rows: 4 × 10 chars (the first 4 rows of the keyboard). */
static const char kb_chars[4][KB_COLS] = {
    { '1','2','3','4','5','6','7','8','9','0' },
    { 'Q','W','E','R','T','Y','U','I','O','P' },
    { 'A','S','D','F','G','H','J','K','L','-' },
    { 'Z','X','C','V','B','N','M','.','_','/' },
};

/* Special-row keys: each has a label and a column span. Columns sum
 * to KB_COLS so the row fills the same width as a regular row. */
typedef struct {
    const char *label;
    u8          span;
    u8          action;     /* see KB_ACT_* below */
} SpecialKey;

enum {
    KB_ACT_DEL = 0,
    KB_ACT_SPACE,
    KB_ACT_CANCEL,
    KB_ACT_OK,
};

static const SpecialKey kb_special[] = {
    { "DEL",    2, KB_ACT_DEL    },
    { "SPACE",  3, KB_ACT_SPACE  },
    { "CANCEL", 3, KB_ACT_CANCEL },
    { "OK",     2, KB_ACT_OK     },
};
#define KB_SPECIAL_COUNT ((int)(sizeof(kb_special) / sizeof(kb_special[0])))

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

static struct {
    bool     active;
    bool     close_clear_pending;   /* one-shot: clear fbs after close */
    char    *buf;                   /* caller-owned destination       */
    size_t   max_len;               /* excluding NUL                  */
    char     title[24];
    char     snapshot[NAME_MAX + 1]; /* restored on CANCEL            */
    u8       row;                   /* 0..KB_ROWS-1                   */
    u8       col;                   /* 0..KB_COLS-1 for regular rows  */
    u8       special_idx;           /* 0..KB_SPECIAL_COUNT-1          */
} ti;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool is_special_row(void)
{
    return ti.row == 4;
}

/* Append a character to the buffer if there's room. Updates the NUL
 * terminator; silently drops the insert if the buffer is full. */
static void buf_append(char c)
{
    size_t n = strlen(ti.buf);
    if (n + 1 > ti.max_len) return;
    ti.buf[n]     = c;
    ti.buf[n + 1] = '\0';
}

/* Delete the last character if any. */
static void buf_backspace(void)
{
    size_t n = strlen(ti.buf);
    if (n == 0) return;
    ti.buf[n - 1] = '\0';
}

static void close_commit(void)
{
    ti.active = false;
    ti.buf = NULL;
    ti.close_clear_pending = true;
}

static void close_cancel(void)
{
    /* Restore snapshot before releasing the pointer. */
    if (ti.buf) {
        strncpy(ti.buf, ti.snapshot, ti.max_len);
        ti.buf[ti.max_len] = '\0';
    }
    ti.active = false;
    ti.buf = NULL;
    ti.close_clear_pending = true;
}

/* Translate the current cursor position to a character press. For
 * special keys, dispatch the action and possibly close the widget. */
static void press_current(void)
{
    if (is_special_row()) {
        switch (kb_special[ti.special_idx].action) {
        case KB_ACT_DEL:    buf_backspace();       break;
        case KB_ACT_SPACE:  buf_append(' ');       break;
        case KB_ACT_CANCEL: close_cancel();        break;
        case KB_ACT_OK:     close_commit();        break;
        }
        return;
    }

    buf_append(kb_chars[ti.row][ti.col]);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void text_input_open(char *buf, size_t max_len, const char *title)
{
    if (ti.active || !buf || max_len == 0) return;

    ti.active      = true;
    ti.buf         = buf;
    ti.max_len     = max_len > NAME_MAX ? NAME_MAX : max_len;
    ti.row         = 1;   /* start on Q row — ergonomic default */
    ti.col         = 0;
    ti.special_idx = KB_ACT_OK;

    if (title) {
        strncpy(ti.title, title, sizeof(ti.title) - 1);
        ti.title[sizeof(ti.title) - 1] = '\0';
    } else {
        ti.title[0] = '\0';
    }

    /* Snapshot for CANCEL. If the caller's buffer exceeds our
     * internal cap we still snapshot up to NAME_MAX — the widget's
     * edit range is capped to max_len, so any extra tail bytes in the
     * caller buffer remain untouched. */
    size_t n = strlen(buf);
    if (n > NAME_MAX) n = NAME_MAX;
    memcpy(ti.snapshot, buf, n);
    ti.snapshot[n] = '\0';
}

bool text_input_is_active(void)
{
    return ti.active;
}

void text_input_cancel(void)
{
    if (ti.active) close_cancel();
}

void text_input_input(u32 down, u32 held)
{
    (void)held;
    if (!ti.active) return;

    /* Shortcut keys. */
    if (down & KEY_START)  { close_commit(); return; }
    if (down & KEY_SELECT) { close_cancel(); return; }

    if (down & KEY_UP) {
        if (ti.row > 0) ti.row--;
    }
    if (down & KEY_DOWN) {
        if (ti.row + 1 < KB_ROWS) ti.row++;
    }

    if (is_special_row()) {
        if (down & KEY_LEFT) {
            if (ti.special_idx > 0) ti.special_idx--;
        }
        if (down & KEY_RIGHT) {
            if (ti.special_idx + 1 < KB_SPECIAL_COUNT) ti.special_idx++;
        }
    } else {
        if (down & KEY_LEFT) {
            if (ti.col > 0) ti.col--;
        }
        if (down & KEY_RIGHT) {
            if (ti.col + 1 < KB_COLS) ti.col++;
        }
    }

    if (down & KEY_A) press_current();
    if (down & KEY_B) buf_backspace();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_key_cell(u8 *fb, int col, int row, int w_cells, int h_cells,
                          const char *text, bool highlighted)
{
    u8 bg = highlighted ? PAL_ROW_CURSOR : PAL_HEADER_BG;
    u8 fg = highlighted ? PAL_WHITE      : PAL_TEXT;

    /* Fill the key cell. */
    for (int r = 0; r < h_cells; r++) {
        font_fill_row(fb, row + r, col, col + w_cells, bg);
    }

    /* Center the label horizontally, vertically on the first row. */
    int label_len = (int)strlen(text);
    int label_col = col + (w_cells - label_len) / 2;
    if (label_col < col) label_col = col;
    font_puts(fb, label_col, row, text, fg);
}

static void draw_bottom_keyboard(u8 *bot_fb)
{
    font_clear(bot_fb, PAL_BG);

    /* Header. */
    font_fill_row(bot_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(bot_fb, 1, 0, "KEYBOARD", PAL_WHITE);

    /* Regular rows (row 0..3). */
    for (int r = 0; r < 4; r++) {
        int row_y = KB_Y_OFFSET + r * KB_KEY_H;
        for (int c = 0; c < KB_COLS; c++) {
            int col_x = KB_X_OFFSET + c * KB_KEY_W;
            char label[2] = { kb_chars[r][c], '\0' };
            bool here = ((int)ti.row == r && (int)ti.col == c);
            draw_key_cell(bot_fb, col_x, row_y, KB_KEY_W, KB_KEY_H,
                          label, here);
        }
    }

    /* Special row (row 4) — spans distributed across KB_COLS. */
    int row_y = KB_Y_OFFSET + 4 * KB_KEY_H;
    int col_x = KB_X_OFFSET;
    for (int i = 0; i < KB_SPECIAL_COUNT; i++) {
        int w = kb_special[i].span * KB_KEY_W;
        bool here = (is_special_row() && (int)ti.special_idx == i);
        draw_key_cell(bot_fb, col_x, row_y, w, KB_KEY_H,
                      kb_special[i].label, here);
        col_x += w;
    }

    /* Footer hints. */
    int hint_row = FONT_ROWS - 2;
    font_fill_row(bot_fb, hint_row, 0, FONT_COLS, PAL_BG);
    font_puts(bot_fb, 1, hint_row,
              "A:press  B:del  START:ok  SELECT:cancel",
              PAL_DIM);
}

static void draw_top_buffer(u8 *top_fb)
{
    font_clear(top_fb, PAL_BG);

    font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 1, 0, ti.title[0] ? ti.title : "TEXT INPUT", PAL_WHITE);

    /* Buffer with caret appended. The caret is always after the last
     * character because the widget only supports append/backspace. */
    int row = 4;
    font_fill_row(top_fb, row, 0, FONT_COLS, PAL_BG);
    font_printf(top_fb, 2, row, PAL_TEXT, "> %s", ti.buf ? ti.buf : "");

    size_t n = ti.buf ? strlen(ti.buf) : 0;
    int caret_col = 2 + 2 + (int)n;   /* "> " offset + text length */
    if (caret_col < FONT_COLS)
        font_putc(top_fb, caret_col, row, '_', PAL_ORANGE);

    /* Length indicator. */
    font_printf(top_fb, 2, row + 2, PAL_DIM,
                "%u / %u chars", (unsigned)n, (unsigned)ti.max_len);
}

void text_input_draw(u8 *top_fb, u8 *bot_fb)
{
    if (!ti.active) return;
    if (top_fb) draw_top_buffer(top_fb);
    if (bot_fb) draw_bottom_keyboard(bot_fb);
}

bool text_input_draw_and_consume(u8 *top_fb, u8 *bot_fb)
{
    if (ti.active) {
        text_input_draw(top_fb, bot_fb);
        return true;
    }
    if (ti.close_clear_pending) {
        /* Wipe both screens once so the underlying view starts from a
         * known-clean state on its next draw. We return true so the
         * caller skips its own draw for this single frame — next frame
         * the view draws normally on top of the cleared fbs. */
        if (top_fb) font_clear(top_fb, PAL_BG);
        if (bot_fb) font_clear(bot_fb, PAL_BG);
        ti.close_clear_pending = false;
        return true;
    }
    return false;
}
