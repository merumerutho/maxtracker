/*
 * instrument_view.c — Instrument parameter editor.
 *
 * Top screen: editable parameter list for the current instrument.
 * Bottom screen: envelope visualization with tab selection.
 *
 * Layout (64 cols x 32 rows at 4x6 font):
 *   Row 0:      Header bar (instrument number, name, count)
 *   Row 1:      Separator
 *   Rows 2-12:  Parameter rows (one per editable field)
 *   Row 13:     Separator
 *   Rows 14-29: (spare / future instrument info)
 *   Row 30:     Footer help
 *   Row 31:     Transport bar
 */

#include "instrument_view.h"
#include "screen.h"
#include "font.h"
#include "draw_util.h"
#include "editor_state.h"   /* EditorCursor (cursor.instrument) */
#include "song.h"
#include "playback.h"       /* playback_rebuild_mas for live edits */
#include "scroll_view.h"
#include <stdio.h>
#include <string.h>
#include "keybind.h"

/* dirty flags: use mt_mark_song_modified() from editor_state.h */

/* ------------------------------------------------------------------ */
/* Parameter field definitions                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    PARAM_GLOBAL_VOL = 0,
    PARAM_FADEOUT,
    PARAM_PANNING,
    PARAM_NNA,
    PARAM_ENV_VOL,
    PARAM_ENV_PAN,
    PARAM_ENV_PITCH,
    PARAM_SAMPLE,
    PARAM_COUNT
} ParamField;

static const char *param_labels[PARAM_COUNT] = {
    "Global Volume",
    "Fadeout",
    "Panning",
    "New Note Action",
    "Envelope: Volume",
    "Envelope: Panning",
    "Envelope: Pitch",
    "Sample",
};

static const char *nna_names[4] = { "Cut", "Continue", "Off", "Fade" };

/* ------------------------------------------------------------------ */
/* Editor state (file-static)                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    ENV_TAB_VOL = 0,
    ENV_TAB_PAN,
    ENV_TAB_PITCH,
    ENV_TAB_COUNT
} EnvTab;

static struct {
    u8       param_row;     /* highlighted parameter (0..PARAM_COUNT-1)   */
    EnvTab   env_tab;       /* which envelope tab is shown on bottom      */

    /* Touch envelope editing state */
    s8       drag_node;     /* index of node being dragged, -1 = none    */
    u8       last_tap_node; /* index of last tapped node (double-tap)    */
    u8       tap_timer;     /* frames since last tap (0 = inactive)      */

    /* B+A delete confirm: first chord arms this, second confirms. */
    bool     delete_confirm_pending;
} iv_state = {
    .drag_node = -1
};

void instrument_view_reset_transient(void)
{
    iv_state.delete_confirm_pending = false;
    iv_state.drag_node = -1;
    iv_state.tap_timer = 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Clamp-and-return instrument index (0-based) from cursor.instrument
 * (1-based). Returns 0 if cursor.instrument is 0. */
static inline int cur_inst_idx(void)
{
    int idx = (int)cursor.instrument - 1;
    if (idx < 0) idx = 0;
    if (idx >= MT_MAX_INSTRUMENTS) idx = MT_MAX_INSTRUMENTS - 1;
    return idx;
}

static inline MT_Instrument *cur_inst(void)
{
    return &song.instruments[cur_inst_idx()];
}

/*
 * Pan and pitch envelopes don't have a separate "enabled" flag in the
 * MAS file format — they play whenever they exist. The instrument view's
 * toggle for these envelopes therefore creates or destroys a starter
 * node set instead of flipping a flag. Volume envelope keeps its real
 * mmutil-defined enabled bit.
 *
 * The starter envelope is two nodes at value 32 (the neutral value for
 * both pan and pitch in maxmod's 0-64 envelope range). The user is
 * expected to edit it from there.
 */
static void env_create_default(MT_Envelope *env)
{
    env->node_count = 2;
    env->nodes[0].x = 0;
    env->nodes[0].y = 32;
    env->nodes[1].x = 50;
    env->nodes[1].y = 32;
    env->loop_start = 255;
    env->loop_end   = 255;
    env->sus_start  = 255;
    env->sus_end    = 255;
    env->enabled    = true;
}

static void env_destroy(MT_Envelope *env)
{
    env->node_count = 0;
    env->enabled    = false;
}

/* Read the value of the currently highlighted parameter. */
static int param_get(MT_Instrument *ins, ParamField p)
{
    switch (p) {
    case PARAM_GLOBAL_VOL: return ins->global_volume;
    case PARAM_FADEOUT:    return ins->fadeout;
    case PARAM_PANNING:    return ins->panning;
    case PARAM_NNA:        return ins->nna;
    case PARAM_ENV_VOL:    return ins->env_vol.enabled ? 1 : 0;
    /* Pan and pitch presence is "do nodes exist". The legacy enabled
     * field is kept in sync but is not the source of truth. */
    case PARAM_ENV_PAN:    return ins->env_pan.node_count > 0 ? 1 : 0;
    case PARAM_ENV_PITCH:  return ins->env_pitch.node_count > 0 ? 1 : 0;
    case PARAM_SAMPLE:     return ins->sample;
    default:               return 0;
    }
}

/* Write value with clamping. */
static void param_set(MT_Instrument *ins, ParamField p, int v)
{
    switch (p) {
    case PARAM_GLOBAL_VOL:
        if (v < 0) v = 0;
        if (v > 128) v = 128;
        ins->global_volume = (u8)v;
        break;
    case PARAM_FADEOUT:
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        ins->fadeout = (u8)v;
        break;
    case PARAM_PANNING:
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        ins->panning = (u8)v;
        break;
    case PARAM_NNA:
        if (v < 0) v = 3;
        if (v > 3) v = 0; /* wrap */
        ins->nna = (u8)v;
        break;
    case PARAM_ENV_VOL:
        ins->env_vol.enabled = v ? true : false;
        break;
    /* Pan/pitch toggles are destructive: turning them on creates a
     * starter node set, turning them off discards all nodes. The
     * instrument editor has no other delete-envelope action and no
     * undo, so the user accepts node loss the same way they do for
     * other instrument-editor edits. */
    case PARAM_ENV_PAN:
        if (v) {
            if (ins->env_pan.node_count == 0)
                env_create_default(&ins->env_pan);
        } else {
            env_destroy(&ins->env_pan);
        }
        break;
    case PARAM_ENV_PITCH:
        if (v) {
            if (ins->env_pitch.node_count == 0)
                env_create_default(&ins->env_pitch);
        } else {
            env_destroy(&ins->env_pitch);
        }
        break;
    case PARAM_SAMPLE:
        if (v < 0) v = 0;
        if (v > MT_MAX_SAMPLES) v = MT_MAX_SAMPLES;
        ins->sample = (u8)v;
        break;
    default:
        break;
    }

    /* If the song is playing right now, rebuild the MAS buffer so the
     * audio engine picks up the change immediately. The rebuild is
     * heavyweight (stops + restarts playback under the hood), so it's
     * gated on actual playback to avoid wasted work when stopped — the
     * next playback_play() will rebuild from scratch anyway. */
    if (playback_is_playing())
        playback_rebuild_mas();

    mt_mark_song_modified();
}

/* Step size for LEFT/RIGHT value changes. */
static int param_step(ParamField p)
{
    switch (p) {
    case PARAM_ENV_VOL:
    case PARAM_ENV_PAN:
    case PARAM_ENV_PITCH:
    case PARAM_NNA:
        return 1;  /* toggle / cycle */
    default:
        return 1;
    }
}

/* Format the value string for a parameter into buf (max 20 chars). */
static void param_format(char *buf, int buflen, MT_Instrument *ins,
                         ParamField p)
{
    int v = param_get(ins, p);
    switch (p) {
    case PARAM_GLOBAL_VOL:
        snprintf(buf, buflen, "%3d", v);
        break;
    case PARAM_FADEOUT:
        snprintf(buf, buflen, "%3d", v);
        break;
    case PARAM_PANNING:
        if (v == 128)
            snprintf(buf, buflen, "%3d (C)", v);
        else if (v < 128)
            snprintf(buf, buflen, "%3d (L)", v);
        else
            snprintf(buf, buflen, "%3d (R)", v);
        break;
    case PARAM_NNA:
        snprintf(buf, buflen, "%s", nna_names[v & 3]);
        break;
    case PARAM_ENV_VOL:
    case PARAM_ENV_PAN:
    case PARAM_ENV_PITCH:
        snprintf(buf, buflen, "%s", v ? "ON" : "OFF");
        break;
    case PARAM_SAMPLE:
        snprintf(buf, buflen, "%02X", v);
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Top screen: parameter list                                          */
/* ------------------------------------------------------------------ */

#define PARAM_ROW_START 3   /* first data row on screen */
#define LABEL_COL         1
#define VALUE_COL_SMALL  40
#define SEP_STR  "-----------------------------------------------" \
                 "-----------------"

static void draw_top(u8 *fb)
{
    MT_Instrument *ins = cur_inst();

    /* Row 0 — header */
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    if (ins->name[0])
        font_printf(fb, 0, 0, PAL_WHITE, "Instrument %02X \"%s\"",
                    cursor.instrument, ins->name);
    else
        font_printf(fb, 0, 0, PAL_WHITE, "Instrument %02X",
                    cursor.instrument);
    font_printf(fb, font_scale_col(50), 0, PAL_GRAY, "[%02X/%02X]",
                cursor.instrument, (unsigned)song.inst_count);

    /* Row 1 — blank */
    font_fill_row(fb, 1, 0, FONT_COLS, PAL_BG);

    /* Row 2 — separator */
    font_fill_row(fb, 2, 0, FONT_COLS, PAL_BG);
    font_puts(fb, 0, 2, SEP_STR, PAL_DIM);

    /* Parameter list in a scrolling viewport. Height is recomputed so
     * BIG mode shrinks it correctly; the list stays inside the trailing
     * separator row (no spill into the footer). */
    static ScrollView iv_sv = { .row_y = PARAM_ROW_START, .margin = 2 };
    int sep_row = PARAM_ROW_START + PARAM_COUNT;
    if (sep_row > font_scale_row(30) - 1)
        sep_row = font_scale_row(30) - 1;
    iv_sv.row_height = sep_row - PARAM_ROW_START;
    iv_sv.total      = PARAM_COUNT;
    iv_sv.cursor     = (int)iv_state.param_row;
    scroll_view_follow(&iv_sv);

    int iv_first, iv_last;
    scroll_view_visible(&iv_sv, &iv_first, &iv_last);

    char vbuf[24];
    for (int i = iv_first; i < iv_last; i++) {
        int row = iv_sv.row_y + (i - iv_sv.scroll);
        bool selected = ((int)iv_state.param_row == i);
        u8 bg = selected ? PAL_ROW_CURSOR : PAL_BG;

        font_fill_row(fb, row, 0, FONT_COLS, bg);

        /* Label */
        u8 label_color = selected ? PAL_WHITE : PAL_TEXT;
        font_puts(fb, LABEL_COL, row, param_labels[i], label_color);

        /* Dot leader between label and value */
        int value_col = font_scale_col(VALUE_COL_SMALL);
        int label_len = (int)strlen(param_labels[i]);
        for (int d = LABEL_COL + label_len; d < value_col - 1; d++)
            font_putc(fb, d, row, '.', PAL_DIM);

        /* Value */
        param_format(vbuf, sizeof(vbuf), ins, (ParamField)i);
        u8 val_color = selected ? PAL_WHITE : PAL_INST;
        font_puts(fb, value_col, row, vbuf, val_color);
    }

    /* Scrollbar on the rightmost grid column spans only the viewport. */
    scroll_view_draw_scrollbar(&iv_sv, fb, FONT_COLS - 1);

    /* Clear any viewport rows the content didn't fill. */
    int iv_drawn = iv_last - iv_first;
    for (int r = PARAM_ROW_START + iv_drawn; r < sep_row; r++)
        font_fill_row(fb, r, 0, FONT_COLS, PAL_BG);

    /* Row after params — separator */
    font_fill_row(fb, sep_row, 0, FONT_COLS, PAL_BG);
    font_puts(fb, 0, sep_row, SEP_STR, PAL_DIM);

    /* Clear remaining rows up to (but not including) the footer row.
     * Scaled so BIG mode (24 rows) doesn't draw past the framebuffer. */
    for (int r = sep_row + 1; r < font_scale_row(30); r++)
        font_fill_row(fb, r, 0, FONT_COLS, PAL_BG);

    /* Row 30 — footer help / delete-confirm prompt */
    font_fill_row(fb, font_scale_row(30), 0, FONT_COLS, PAL_BG);
    if (iv_state.delete_confirm_pending) {
        font_puts(fb, 0, font_scale_row(30),
                  "Confirm removing this instrument? B+A:yes  B:no",
                  PAL_ORANGE);
    } else {
        font_puts(fb, 0, font_scale_row(30),
                  "A+L/R:value  B+A:reset  L/R:Inst  SEL+L:back",
                  PAL_GRAY);
    }

    /* Row 31 — transport (reuse pattern view style) */
    int trow = font_scale_row(31);
    font_fill_row(fb, trow, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, trow, "INST", PAL_INST);
    font_printf(fb, 6, trow, PAL_GRAY, "Ins:%02X  Oct:%d  Smp:%02X",
                cursor.instrument, cursor.octave,
                ins->sample);
    font_puts(fb, font_scale_col(44), trow, "[SELECT+L:back]", PAL_DIM);
}

/* ------------------------------------------------------------------ */
/* Bottom screen: envelope visualization                               */
/* ------------------------------------------------------------------ */

/* Draw area bounds (pixel coordinates on 256x192 bottom screen) */
#define ENV_DRAW_X0  8
#define ENV_DRAW_X1  247
#define ENV_DRAW_Y0  12
#define ENV_DRAW_Y1  168
#define ENV_DRAW_W   (ENV_DRAW_X1 - ENV_DRAW_X0)  /* 240 */
#define ENV_DRAW_H   (ENV_DRAW_Y1 - ENV_DRAW_Y0)  /* 156 */

/* Bottom screen width in pixels */
#define BOT_WIDTH  256

static const char *env_tab_names[ENV_TAB_COUNT] = {
    "Volume", "Panning", "Pitch"
};

/* plot_pixel is now provided by draw_util.h */

/* Draw a filled rectangle using plot_pixel. */
static void fill_rect(u8 *fb, int x0, int y0, int w, int h, u8 color)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            plot_pixel(fb, x, y, color);
}

/* Draw a line between two points using Bresenham's algorithm. */
static void draw_line(u8 *fb, int x0, int y0, int x1, int y1, u8 color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;

    for (;;) {
        plot_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Draw a vertical dashed line within the draw area. */
static void draw_vdash(u8 *fb, int px, u8 color)
{
    if (px < ENV_DRAW_X0 || px > ENV_DRAW_X1) return;
    for (int y = ENV_DRAW_Y0; y <= ENV_DRAW_Y1; y += 3)
        plot_pixel(fb, px, y, color);
}

/* Get the currently selected envelope. */
static MT_Envelope *get_cur_envelope(MT_Instrument *ins, EnvTab tab)
{
    switch (tab) {
    case ENV_TAB_VOL:   return &ins->env_vol;
    case ENV_TAB_PAN:   return &ins->env_pan;
    case ENV_TAB_PITCH: return &ins->env_pitch;
    default:            return &ins->env_vol;
    }
}

/* ------------------------------------------------------------------ */
/* Coordinate mapping helpers for envelope touch editing               */
/* ------------------------------------------------------------------ */

/* Compute max_tick from envelope nodes (for X scaling). */
static u16 env_max_tick(const MT_Envelope *env)
{
    u16 mt = 0;
    for (int i = 0; i < env->node_count; i++) {
        if (env->nodes[i].x > mt)
            mt = env->nodes[i].x;
    }
    return (mt > 0) ? mt : 512;
}

/* Pixel -> envelope coordinate */
static u16 px_to_env_x(int px, u16 max_tick)
{
    int rel = px - ENV_DRAW_X0;
    if (rel < 0) rel = 0;
    if (rel > ENV_DRAW_W) rel = ENV_DRAW_W;
    return (u16)(((u32)rel * max_tick) / ENV_DRAW_W);
}

static u8 px_to_env_y(int py)
{
    int rel = py - ENV_DRAW_Y0;
    if (rel < 0) rel = 0;
    if (rel > ENV_DRAW_H) rel = ENV_DRAW_H;
    int val = 64 - (rel * 64) / ENV_DRAW_H;
    if (val < 0) val = 0;
    if (val > 64) val = 64;
    return (u8)val;
}

/* Envelope -> pixel coordinate */
static int env_x_to_px(u16 ex, u16 max_tick)
{
    if (max_tick == 0) return ENV_DRAW_X0;
    return ENV_DRAW_X0 + (int)(((u32)ex * ENV_DRAW_W) / max_tick);
}

static int env_y_to_px(u8 ey)
{
    return ENV_DRAW_Y1 - (int)(((u32)ey * ENV_DRAW_H) / 64);
}

/* Find the nearest node within a given pixel radius. Returns -1 if none. */
static int find_nearest_node(const MT_Envelope *env, int px, int py,
                             u16 max_tick, int radius)
{
    int best = -1;
    int best_dist2 = radius * radius + 1;
    for (int i = 0; i < env->node_count; i++) {
        int nx = env_x_to_px(env->nodes[i].x, max_tick);
        int ny = env_y_to_px(env->nodes[i].y);
        int dx = px - nx;
        int dy = py - ny;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best = i;
        }
    }
    return best;
}

/* Insert a new node at the correct sorted position by x. Returns the
 * index where it was inserted, or -1 if the array is full. */
static int env_insert_node(MT_Envelope *env, u16 x, u8 y)
{
    if (env->node_count >= MT_MAX_ENV_NODES) return -1;

    /* Find insertion point (keep sorted by x) */
    int pos = 0;
    while (pos < env->node_count && env->nodes[pos].x < x)
        pos++;

    /* Shift nodes right */
    for (int i = env->node_count; i > pos; i--)
        env->nodes[i] = env->nodes[i - 1];

    env->nodes[pos].x = x;
    env->nodes[pos].y = y;
    env->node_count++;

    /* Adjust loop/sustain indices that shifted */
    if (env->loop_start != 255 && env->loop_start >= pos)
        env->loop_start++;
    if (env->loop_end != 255 && env->loop_end >= pos)
        env->loop_end++;
    if (env->sus_start != 255 && env->sus_start >= pos)
        env->sus_start++;
    if (env->sus_end != 255 && env->sus_end >= pos)
        env->sus_end++;

    return pos;
}

/* Delete a node at index. Shifts remaining nodes left.
 * Does nothing if node_count <= 1. */
static void env_delete_node(MT_Envelope *env, int idx)
{
    if (env->node_count <= 1) return;
    if (idx < 0 || idx >= env->node_count) return;

    /* Shift nodes left */
    for (int i = idx; i < env->node_count - 1; i++)
        env->nodes[i] = env->nodes[i + 1];
    env->node_count--;

    /* Adjust loop/sustain indices */
    if (env->loop_start != 255) {
        if (env->loop_start == idx)
            env->loop_start = 255; /* deleted the loop point */
        else if (env->loop_start > idx)
            env->loop_start--;
    }
    if (env->loop_end != 255) {
        if (env->loop_end == idx)
            env->loop_end = 255;
        else if (env->loop_end > idx)
            env->loop_end--;
    }
    if (env->sus_start != 255) {
        if (env->sus_start == idx)
            env->sus_start = 255;
        else if (env->sus_start > idx)
            env->sus_start--;
    }
    if (env->sus_end != 255) {
        if (env->sus_end == idx)
            env->sus_end = 255;
        else if (env->sus_end > idx)
            env->sus_end--;
    }
}

/* Handle touchscreen input for envelope editing. Called each frame. */
static void env_touch_input(u32 down, u32 held)
{
    MT_Instrument *ins = cur_inst();
    MT_Envelope *env = get_cur_envelope(ins, iv_state.env_tab);

    /* Decrement tap timer each frame */
    if (iv_state.tap_timer > 0)
        iv_state.tap_timer--;

    /* Check touch state */
    bool touch_down = (down & KEY_TOUCH) != 0;
    bool touch_held = (held & KEY_TOUCH) != 0;
    bool touch_active = touch_down || touch_held;

    if (!touch_active) {
        /* Stylus released: end drag */
        iv_state.drag_node = -1;
        return;
    }

    /* Read via the maxtracker touch helper which does its own raw→pixel
     * linear calibration; libnds's touch.px/py are zero under no$gba
     * without readUserSettings, while the raw ADC values are fine. */
    int tx, ty;
    touch_read_pixel(&tx, &ty);

    /* ---- Check for tab label taps (row 0, y < ENV_DRAW_Y0 - 1) ---- */
    if (touch_down && ty < ENV_DRAW_Y0 - 1) {
        /* Tab labels are at font row 0. Each char is FONT_W (4) pixels.
         * Layout: col 1: "Vol", col 5: " | ", col 8: "Pan", col 12: " | ",
         *         col 15: "Pitch"
         * Convert pixel x to font column. */
        int col = tx / FONT_W;
        if (col >= 1 && col < 4)
            iv_state.env_tab = ENV_TAB_VOL;
        else if (col >= 8 && col < 11)
            iv_state.env_tab = ENV_TAB_PAN;
        else if (col >= 15 && col < 20)
            iv_state.env_tab = ENV_TAB_PITCH;
        return;
    }

    /* ---- Only handle touches within the draw area ---- */
    if (tx < ENV_DRAW_X0 || tx > ENV_DRAW_X1 ||
        ty < ENV_DRAW_Y0 || ty > ENV_DRAW_Y1)
        return;

    u16 max_tick = env_max_tick(env);

    /* ---- New touch: select node or add node ---- */
    if (touch_down) {
        int nearest = find_nearest_node(env, tx, ty, max_tick, 8);

        if (nearest >= 0) {
            /* Check for double-tap on same node */
            if (iv_state.tap_timer > 0 &&
                iv_state.last_tap_node == (u8)nearest) {
                /* Double-tap: delete node */
                env_delete_node(env, nearest);
                iv_state.tap_timer = 0;
                iv_state.drag_node = -1;
                mt_mark_song_modified();
                return;
            }

            /* Single tap on node: start drag, record for double-tap */
            iv_state.drag_node = (s8)nearest;
            iv_state.last_tap_node = (u8)nearest;
            iv_state.tap_timer = 15;
        } else {
            /* Tap on empty area: add a new node */
            u16 new_x = px_to_env_x(tx, max_tick);
            u8  new_y = px_to_env_y(ty);
            int idx = env_insert_node(env, new_x, new_y);
            if (idx >= 0) {
                iv_state.drag_node = (s8)idx;
                iv_state.last_tap_node = (u8)idx;
                iv_state.tap_timer = 15;
                mt_mark_song_modified();
            }
        }
        return;
    }

    /* ---- Dragging: update node position ---- */
    if (iv_state.drag_node >= 0 && iv_state.drag_node < env->node_count) {
        int di = iv_state.drag_node;

        /* Compute new envelope coordinates */
        u16 new_x = px_to_env_x(tx, max_tick);
        u8  new_y = px_to_env_y(ty);

        /* Constrain X to maintain node ordering */
        u16 min_x = 0;
        u16 max_x = 65535;
        if (di > 0)
            min_x = env->nodes[di - 1].x + 1;
        if (di < env->node_count - 1)
            max_x = env->nodes[di + 1].x - 1;
        if (new_x < min_x) new_x = min_x;
        if (new_x > max_x) new_x = max_x;

        /* Constrain Y to 0..64 (already done in px_to_env_y) */
        if (new_y > 64) new_y = 64;

        if (env->nodes[di].x != new_x || env->nodes[di].y != new_y) {
            env->nodes[di].x = new_x;
            env->nodes[di].y = new_y;
            mt_mark_song_modified();
        }
    }
}

static void draw_bottom(u8 *fb)
{
    MT_Instrument *ins = cur_inst();
    MT_Envelope *env = get_cur_envelope(ins, iv_state.env_tab);

    /* Clear entire bottom screen */
    font_clear(fb, PAL_BG);

    /* ---- Tab labels at top: "Vol | Pan | Pitch" ---- */
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);

    static const char *tab_short[ENV_TAB_COUNT] = { "Vol", "Pan", "Pitch" };
    int col = 1;
    for (int t = 0; t < ENV_TAB_COUNT; t++) {
        u8 c = ((int)iv_state.env_tab == t) ? PAL_WHITE : PAL_DIM;
        font_puts(fb, col, 0, tab_short[t], c);
        col += (int)strlen(tab_short[t]);
        if (t < ENV_TAB_COUNT - 1) {
            font_puts(fb, col, 0, " | ", PAL_GRAY);
            col += 3;
        }
    }

    /* Envelope info text */
    font_printf(fb, 22, 0, PAL_GRAY, " %s  Nodes:%d",
                env->enabled ? "ON" : "OFF", env->node_count);

    /* ---- Draw area border (single-pixel outline) ---- */
    for (int x = ENV_DRAW_X0 - 1; x <= ENV_DRAW_X1 + 1; x++) {
        plot_pixel(fb, x, ENV_DRAW_Y0 - 1, PAL_DIM);
        plot_pixel(fb, x, ENV_DRAW_Y1 + 1, PAL_DIM);
    }
    for (int y = ENV_DRAW_Y0 - 1; y <= ENV_DRAW_Y1 + 1; y++) {
        plot_pixel(fb, ENV_DRAW_X0 - 1, y, PAL_DIM);
        plot_pixel(fb, ENV_DRAW_X1 + 1, y, PAL_DIM);
    }

    /* ---- Envelope visualization ---- */
    if (env->node_count < 2) {
        /* Not enough nodes to draw */
        font_puts(fb, 10, 14, "No envelope data", PAL_GRAY);
    } else {
        /* Find max tick for X scaling */
        u16 max_tick = 1;
        for (int i = 0; i < env->node_count; i++) {
            if (env->nodes[i].x > max_tick)
                max_tick = env->nodes[i].x;
        }

        /* Map envelope node to pixel coordinates.
         * node.x: 0..max_tick  -> ENV_DRAW_X0..ENV_DRAW_X1
         * node.y: 0..64        -> ENV_DRAW_Y1..ENV_DRAW_Y0 (inverted: 0=bottom, 64=top)
         */
        #define NODE_PX(n) (ENV_DRAW_X0 + (int)((u32)(n).x * ENV_DRAW_W / max_tick))
        #define NODE_PY(n) (ENV_DRAW_Y1 - (int)((u32)(n).y * ENV_DRAW_H / 64))

        /* Draw loop markers (vertical dashed lines in orange) */
        if (env->loop_start != 255 && env->loop_start < env->node_count) {
            int lx = NODE_PX(env->nodes[env->loop_start]);
            draw_vdash(fb, lx, PAL_ORANGE);
        }
        if (env->loop_end != 255 && env->loop_end < env->node_count) {
            int lx = NODE_PX(env->nodes[env->loop_end]);
            draw_vdash(fb, lx, PAL_ORANGE);
        }

        /* Draw sustain markers (vertical dashed lines in green/play color) */
        if (env->sus_start != 255 && env->sus_start < env->node_count) {
            int sx = NODE_PX(env->nodes[env->sus_start]);
            draw_vdash(fb, sx, PAL_PLAY);
        }
        if (env->sus_end != 255 && env->sus_end < env->node_count) {
            int sx = NODE_PX(env->nodes[env->sus_end]);
            draw_vdash(fb, sx, PAL_PLAY);
        }

        /* Draw lines between consecutive nodes */
        for (int i = 0; i < env->node_count - 1; i++) {
            int x0 = NODE_PX(env->nodes[i]);
            int y0 = NODE_PY(env->nodes[i]);
            int x1 = NODE_PX(env->nodes[i + 1]);
            int y1 = NODE_PY(env->nodes[i + 1]);
            draw_line(fb, x0, y0, x1, y1, PAL_WHITE);
        }

        /* Draw 3x3 squares at each node position.
         * The dragged node is drawn larger (5x5) in orange. */
        for (int i = 0; i < env->node_count; i++) {
            int px = NODE_PX(env->nodes[i]);
            int py = NODE_PY(env->nodes[i]);
            if (iv_state.drag_node == i) {
                fill_rect(fb, px - 2, py - 2, 5, 5, PAL_ORANGE);
            } else {
                fill_rect(fb, px - 1, py - 1, 3, 3, PAL_NOTE);
            }
        }

        #undef NODE_PX
        #undef NODE_PY
    }

    /* ---- Legend / status text below draw area ---- */
    int info_row = font_scale_row(29); /* font row near bottom */
    if (env->loop_start != 255)
        font_printf(fb, 1, info_row, PAL_ORANGE, "Loop:%d-%d",
                    env->loop_start, env->loop_end);
    else
        font_puts(fb, 1, info_row, "Loop:--", PAL_DIM);

    if (env->sus_start != 255)
        font_printf(fb, 16, info_row, PAL_PLAY, "Sus:%d-%d",
                    env->sus_start, env->sus_end);
    else
        font_puts(fb, 16, info_row, "Sus:--", PAL_DIM);

    /* Bottom help rows */
    font_fill_row(fb, font_scale_row(30), 0, FONT_COLS, PAL_BG);
    font_puts(fb, 0, font_scale_row(30),
              "Tap:add  Drag:move  DblTap:del  X:tab",
              PAL_DIM);
    font_fill_row(fb, font_scale_row(31), 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, font_scale_row(31), "L/R:prev/next instrument", PAL_GRAY);
}

/* ------------------------------------------------------------------ */
/* Input handler                                                       */
/* ------------------------------------------------------------------ */

void instrument_view_input(u32 down, u32 held)
{
    MT_Instrument *ins = cur_inst();

    /* --- Touchscreen envelope editing (always active) --- */
    env_touch_input(down, held);

    /* --- B+A: reset instrument to defaults (two-step confirm) ---
     * First chord arms a prompt (rendered by instrument_view_draw);
     * second B+A while the prompt is up performs the reset. Any other
     * input clears the pending prompt. */
    if ((held & MT_KEY_BACK) && (down & MT_KEY_CONFIRM)) {
        if (!iv_state.delete_confirm_pending) {
            iv_state.delete_confirm_pending = true;
            return;
        }
        iv_state.delete_confirm_pending = false;
        ins->active = false;
        ins->global_volume = 128;
        ins->fadeout = 0;
        ins->panning = 128;
        ins->random_volume = 0;
        ins->nna = 0;
        ins->dct = 0;
        ins->dca = 0;
        ins->env_vol.enabled = false;
        ins->env_vol.node_count = 0;
        ins->env_pan.enabled = false;
        ins->env_pan.node_count = 0;
        ins->env_pitch.enabled = false;
        ins->env_pitch.node_count = 0;
        ins->has_full_notemap = false;
        ins->sample = 0;
        memset(ins->name, 0, sizeof(ins->name));
        if (playback_is_playing())
            playback_rebuild_mas();
        mt_mark_song_modified();
        return;
    }

    /* B alone cancels the pending prompt. */
    if (iv_state.delete_confirm_pending &&
        (down & (MT_KEY_BACK | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                 MT_KEY_MOD_PRIMARY | MT_KEY_MOD_SECONDARY | MT_KEY_SHOULDER_L | MT_KEY_SHOULDER_R | MT_KEY_START | MT_KEY_SHIFT))) {
        iv_state.delete_confirm_pending = false;
        if (down & MT_KEY_BACK) return;
    }

    /* --- X button: cycle envelope tab (Vol->Pan->Pitch->Vol) --- */
    if (down & MT_KEY_MOD_PRIMARY) {
        iv_state.env_tab = (EnvTab)(((int)iv_state.env_tab + 1) % ENV_TAB_COUNT);
    }

    /* --- LSDJ A-held editing ---
     * Plain UP/DOWN navigates the parameter list.
     * A held + LEFT/RIGHT: small edit (±1, or toggle/cycle for booleans).
     * A held + UP/DOWN: large edit (±10 for numeric fields; same
     *   toggle/cycle for booleans since ±10 is meaningless on a toggle).
     * Repeat-aware via keysDownRepeat so holding a direction keeps
     * nudging the value. */
    if (!(held & MT_KEY_CONFIRM)) {
        if (down & KEY_UP) {
            if (iv_state.param_row > 0)
                iv_state.param_row--;
            else
                iv_state.param_row = PARAM_COUNT - 1;
        }
        if (down & KEY_DOWN) {
            if (iv_state.param_row < PARAM_COUNT - 1)
                iv_state.param_row++;
            else
                iv_state.param_row = 0;
        }
    } else {
        u32 rep = keysDownRepeat();
        ParamField p = (ParamField)iv_state.param_row;
        int val = param_get(ins, p);
        bool is_toggle = (p == PARAM_ENV_VOL  || p == PARAM_ENV_PAN ||
                          p == PARAM_ENV_PITCH);
        bool is_cycle  = (p == PARAM_NNA);

        int delta = 0;
        if (rep & KEY_RIGHT) delta = +1;
        if (rep & KEY_LEFT)  delta = -1;
        if (rep & KEY_UP)    delta = +10;
        if (rep & KEY_DOWN)  delta = -10;

        if (delta != 0) {
            if (is_cycle)
                param_set(ins, p, (val + (delta > 0 ? 1 : 3)) % 4);
            else if (is_toggle)
                param_set(ins, p, val ? 0 : 1);
            else
                param_set(ins, p, val + delta);
        }
    }

    /* (B no longer navigates — use SELECT+LEFT to return to pattern) */

    /* L: previous instrument */
    if (down & MT_KEY_SHOULDER_L) {
        if (cursor.instrument > 1)
            cursor.instrument--;
        else
            cursor.instrument = (u8)song.inst_count;
    }

    /* R: next instrument */
    if (down & MT_KEY_SHOULDER_R) {
        if (cursor.instrument < song.inst_count)
            cursor.instrument++;
        else
            cursor.instrument = 1;
    }

    /* Update envelope tab to match highlighted parameter (auto-switch) */
    if (iv_state.param_row == PARAM_ENV_VOL)
        iv_state.env_tab = ENV_TAB_VOL;
    else if (iv_state.param_row == PARAM_ENV_PAN)
        iv_state.env_tab = ENV_TAB_PAN;
    else if (iv_state.param_row == PARAM_ENV_PITCH)
        iv_state.env_tab = ENV_TAB_PITCH;
}

/* ------------------------------------------------------------------ */
/* Public draw API                                                     */
/* ------------------------------------------------------------------ */

void instrument_view_draw(u8 *top_fb, u8 *bot_fb)
{
    draw_top(top_fb);
    draw_bottom(bot_fb);
}
