#ifndef MT_FONT_H
#define MT_FONT_H

#include <nds.h>

typedef enum {
    FONT_MODE_SMALL = 0,
    FONT_MODE_BIG   = 1,
} FontMode;

#define FONT_COLS_MAX 64

extern int FONT_W;
extern int FONT_H;
extern int FONT_COLS;
extern int FONT_ROWS;

void font_init(void);
void font_set_mode(FontMode mode);
FontMode font_get_mode(void);

int font_face_count(FontMode mode);
const char *font_face_name(FontMode mode, int index);
void font_set_face(FontMode mode, int index);
int font_get_face(FontMode mode);

void font_putc(u8 *fb, int col, int row, char ch, u8 color);
int font_puts(u8 *fb, int col, int row, const char *str, u8 color);
void font_puts_colored(u8 *fb, int col, int row,
                       const char *str, const u8 *colors, int len);
void font_fill_row(u8 *fb, int row, int col_start, int col_end, u8 bg);
void font_clear(u8 *fb, u8 bg);
int font_printf(u8 *fb, int col, int row, u8 color, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static inline int font_scale_col(int small_col) {
    return small_col * FONT_COLS / 64;
}

static inline int font_scale_row(int small_row) {
    return small_row * FONT_ROWS / 32;
}

#endif /* MT_FONT_H */
