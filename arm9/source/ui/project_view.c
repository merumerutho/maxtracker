/*
 * project_view.c -- Project/Settings screen for maxtracker.
 *
 * Top screen: editable song-level parameter list (LGPT ProjectView style).
 * Bottom screen: song statistics summary.
 *
 * Layout (64 cols x 32 rows at 4x6 font):
 *   Row 0:      Header bar "PROJECT SETTINGS"
 *   Row 1:      Separator
 *   Rows 2+:    Parameter rows (label left, value right)
 *   Row 30:     Help text
 *   Row 31:     Transport bar
 *
 * Initial version: Save/Save-As stubbed (needs mas_write), debug
 * overlay stubbed (needs debug_view), song name is read-only (needs
 * text_input for editing). All other settings fully functional.
 */

#include "project_view.h"
#include "memtrack.h"
#include "screen.h"
#include "font.h"
#include "song.h"
#include "editor_state.h"
#include "pattern_view.h"
#include "filebrowser.h"
#include "scroll_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern FileBrowser disk_browser;
extern bool        disk_browser_inited;
extern char        status_msg[64];
extern int         status_timer;
extern bool        song_modified;
extern bool        autosave_dirty;
extern ScreenMode  disk_return_screen;

/* ------------------------------------------------------------------ */
/* Row definitions                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    PK_EDIT = 0,        /* editable value */
    PK_RO,              /* read-only counter */
    PK_SEP,             /* separator (NULL label, cursor skips) */
    PK_ACT,             /* action, A-press runs immediately */
    PK_ACT_CONFIRM      /* action, A-press stages a confirm dialog */
} ProjKind;

#define PROJ_ROWS(X) \
    X(NAME,             "Song Name",         PK_EDIT)        \
    X(TEMPO,            "Tempo (BPM)",       PK_EDIT)        \
    X(SPEED,            "Speed (ticks/row)", PK_EDIT)        \
    X(MASTER_VOL,       "Master Volume",     PK_EDIT)        \
    X(CHANNELS,         "Channels",          PK_EDIT)        \
    X(REPEAT_POS,       "Repeat Position",   PK_EDIT)        \
    X(FOLLOW_MODE,      "Follow Mode",       PK_EDIT)        \
    X(FONT_SIZE,        "Font Size",         PK_EDIT)        \
    X(DEBUG_OVERLAY,    "Debug Overlay",     PK_EDIT)        \
    X(KEY_REPEAT_DELAY, "Key Repeat Delay",  PK_EDIT)        \
    X(KEY_REPEAT_RATE,  "Key Repeat Rate",   PK_EDIT)        \
    X(SEP1,             NULL,                PK_SEP)         \
    X(INST_COUNT,       "Instruments",       PK_RO)          \
    X(SAMP_COUNT,       "Samples",           PK_RO)          \
    X(PATT_COUNT,       "Patterns",          PK_RO)          \
    X(SEP2,             NULL,                PK_SEP)         \
    X(NEW_SONG,         ">> New",            PK_ACT_CONFIRM) \
    X(LOAD,             ">> Load",           PK_ACT_CONFIRM) \
    X(SAVE,             ">> Save",           PK_ACT)         \
    X(SAVE_AS,          ">> Save As",        PK_ACT)         \
    X(COMPACT_PAT,      ">> Compact Patterns",    PK_ACT_CONFIRM) \
    X(COMPACT_INST,     ">> Compact Instruments", PK_ACT_CONFIRM)

typedef enum {
#define X(name, label, kind) PROW_##name,
    PROJ_ROWS(X)
#undef X
    PROW_COUNT
} ProjRow;

static const char *row_labels[PROW_COUNT] = {
#define X(name, label, kind) label,
    PROJ_ROWS(X)
#undef X
};

static const u8 row_kinds[PROW_COUNT] = {
#define X(name, label, kind) (u8)(kind),
    PROJ_ROWS(X)
#undef X
};

/* Valid channel count values for cycling */
static const u8 chan_values[] = { 4, 8, 16, 24, 32 };
#define CHAN_VALUE_COUNT 5

/* ------------------------------------------------------------------ */
/* Editor state                                                        */
/* ------------------------------------------------------------------ */

static struct {
    u8   cursor_row;        /* 0..PROW_COUNT-1                        */
    bool confirm_pending;   /* true = waiting for A confirm on action  */
    u8   name_edit_pos;     /* cursor position within name string      */
} pv_state;

static ScrollView pv_sv = {
    .row_y       = 3,
    .row_height  = 0,
    .total       = 0,
    .cursor      = 0,
    .scroll      = 0,
    .margin      = 2,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline ProjKind row_kind(ProjRow r)
{
    return (r < PROW_COUNT) ? (ProjKind)row_kinds[r] : PK_SEP;
}

static bool is_separator(ProjRow r) { return row_kind(r) == PK_SEP; }
static bool is_readonly (ProjRow r) { return row_kind(r) == PK_RO;  }

static bool is_action(ProjRow r)
{
    ProjKind k = row_kind(r);
    return k == PK_ACT || k == PK_ACT_CONFIRM;
}

static bool needs_confirm(ProjRow r)
{
    return row_kind(r) == PK_ACT_CONFIRM;
}

static void pv_open_load_browser(void)
{
    filebrowser_init(&disk_browser, fs_browse_root);
    disk_browser_inited = true;
    disk_return_screen = SCREEN_PROJECT;
    screen_set_mode(SCREEN_DISK);
    font_clear(top_fb, PAL_BG);
    font_clear(bot_fb, PAL_BG);
}

static int chan_index(u8 val)
{
    for (int i = 0; i < CHAN_VALUE_COUNT; i++) {
        if (chan_values[i] == val) return i;
    }
    return 0;
}

static void move_cursor(int dir)
{
    int r = (int)pv_state.cursor_row + dir;
    while (r >= 0 && r < PROW_COUNT && is_separator((ProjRow)r))
        r += dir;
    if (r < 0) r = 0;
    if (r >= PROW_COUNT) r = PROW_COUNT - 1;
    if (is_separator((ProjRow)r)) return;
    pv_state.cursor_row = (u8)r;
    pv_state.confirm_pending = false;
}

static int count_used_patterns(void)
{
    int count = 0;
    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i]) count++;
    }
    return count;
}

static void compact_patterns(void)
{
    bool used[MT_MAX_PATTERNS];
    memset(used, 0, sizeof(used));

    for (int i = 0; i < song.order_count; i++) {
        if (song.orders[i] < MT_MAX_PATTERNS)
            used[song.orders[i]] = true;
    }

    int freed = 0;
    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (!used[i] && song.patterns[i]) {
            free(song.patterns[i]);
            song.patterns[i] = NULL;
            freed++;
        }
    }

    u8 max_pat = 0;
    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i] && i >= max_pat)
            max_pat = i + 1;
    }
    song.patt_count = max_pat;

    snprintf(status_msg, sizeof(status_msg),
             "Freed %d pattern(s)", freed);
    status_timer = 120;
    if (freed > 0) mt_mark_song_modified();
}

static void compact_instruments(void)
{
    bool inst_used[MT_MAX_INSTRUMENTS];
    memset(inst_used, 0, sizeof(inst_used));

    for (int p = 0; p < MT_MAX_PATTERNS; p++) {
        MT_Pattern *pat = song.patterns[p];
        if (!pat) continue;
        int total = pat->nrows * pat->ncols;
        for (int c = 0; c < total; c++) {
            u8 inst = pat->cells[c].inst;
            if (inst > 0 && inst <= MT_MAX_INSTRUMENTS)
                inst_used[inst - 1] = true;
        }
    }

    int freed_inst = 0;
    int freed_samp = 0;

    for (int i = 0; i < MT_MAX_INSTRUMENTS; i++) {
        if (!inst_used[i] && song.instruments[i].sample > 0) {
            u8 smp_idx = song.instruments[i].sample - 1;
            memset(&song.instruments[i], 0, sizeof(MT_Instrument));

            bool samp_still_used = false;
            for (int j = 0; j < MT_MAX_INSTRUMENTS; j++) {
                if (j != i && song.instruments[j].sample == smp_idx + 1)
                    samp_still_used = true;
            }
            if (!samp_still_used && song.samples[smp_idx].active) {
                if (song.samples[smp_idx].pcm_data)
                    free(song.samples[smp_idx].pcm_data);
                memset(&song.samples[smp_idx], 0, sizeof(MT_Sample));
                freed_samp++;
            }
            freed_inst++;
        } else if (!inst_used[i]) {
            memset(&song.instruments[i], 0, sizeof(MT_Instrument));
        }
    }

    u8 max_inst = 0;
    for (int i = 0; i < MT_MAX_INSTRUMENTS; i++) {
        if (song.instruments[i].sample > 0 && i + 1 > max_inst)
            max_inst = i + 1;
    }
    song.inst_count = max_inst;

    u8 max_samp = 0;
    for (int i = 0; i < MT_MAX_SAMPLES; i++) {
        if (song.samples[i].active && i + 1 > max_samp)
            max_samp = i + 1;
    }
    song.samp_count = max_samp;

    snprintf(status_msg, sizeof(status_msg),
             "Freed %d inst, %d samp", freed_inst, freed_samp);
    status_timer = 120;
    if (freed_inst > 0 || freed_samp > 0) mt_mark_song_modified();
}

/* ------------------------------------------------------------------ */
/* Value adjustment                                                    */
/* ------------------------------------------------------------------ */

static void adjust_value(ProjRow row, int delta)
{
    switch (row) {
    case PROW_TEMPO: {
        int v = (int)song.initial_tempo + delta;
        if (v < 32) v = 32;
        if (v > 255) v = 255;
        song.initial_tempo = (u8)v;
        break;
    }
    case PROW_SPEED: {
        int v = (int)song.initial_speed + delta;
        if (v < 1) v = 1;
        if (v > 31) v = 31;
        song.initial_speed = (u8)v;
        break;
    }
    case PROW_MASTER_VOL: {
        int v = (int)song.global_volume + delta;
        if (v < 0) v = 0;
        if (v > 128) v = 128;
        song.global_volume = (u8)v;
        break;
    }
    case PROW_CHANNELS: {
        int idx = chan_index(song.channel_count);
        if (delta > 0) idx++;
        else if (delta < 0) idx--;
        if (idx < 0) idx = 0;
        if (idx >= CHAN_VALUE_COUNT) idx = CHAN_VALUE_COUNT - 1;
        song.channel_count = chan_values[idx];
        break;
    }
    case PROW_REPEAT_POS: {
        int v = (int)song.repeat_position + delta;
        if (v < 0) v = 0;
        if (v > 199) v = 199;
        song.repeat_position = (u8)v;
        break;
    }
    case PROW_FOLLOW_MODE:
        cursor.follow = !cursor.follow;
        break;
    case PROW_FONT_SIZE:
        font_set_mode(font_get_mode() == FONT_MODE_BIG
                      ? FONT_MODE_SMALL : FONT_MODE_BIG);
        pattern_view_invalidate_bottom();
        break;
    case PROW_DEBUG_OVERLAY:
        break;  /* debug_view not wired yet */
    case PROW_KEY_REPEAT_DELAY: {
        int v = (int)ui_repeat_delay + delta;
        if (v < 4)  v = 4;
        if (v > 60) v = 60;
        ui_repeat_delay = (u8)v;
        ui_apply_key_repeat();
        break;
    }
    case PROW_KEY_REPEAT_RATE: {
        int v = (int)ui_repeat_rate + delta;
        if (v < 1)  v = 1;
        if (v > 30) v = 30;
        ui_repeat_rate = (u8)v;
        ui_apply_key_repeat();
        break;
    }
    case PROW_NAME: {
        int pos = (int)pv_state.name_edit_pos + delta;
        int len = (int)strlen(song.name);
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        if (pos > 19) pos = 19;
        pv_state.name_edit_pos = (u8)pos;
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Top screen draw                                                     */
/* ------------------------------------------------------------------ */

#define DATA_ROW_START 3
#define LABEL_COL      1
#define VALUE_COL_SMALL 40
#define SEP_STR "-----------------------------------------------" \
                "-----------------"

static void draw_top(u8 *fb)
{
    /* Row 0 -- header */
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 1, 0, "PROJECT SETTINGS", PAL_WHITE);

    /* Row 1 -- blank */
    font_fill_row(fb, 1, 0, FONT_COLS, PAL_BG);

    /* Row 2 -- separator */
    font_fill_row(fb, 2, 0, FONT_COLS, PAL_BG);
    font_puts(fb, 0, 2, SEP_STR, PAL_DIM);

    int help_row_end = font_scale_row(30);
    pv_sv.row_height = help_row_end - DATA_ROW_START;
    pv_sv.total      = PROW_COUNT;
    pv_sv.cursor     = (int)pv_state.cursor_row;
    scroll_view_follow(&pv_sv);

    int first, last;
    scroll_view_visible(&pv_sv, &first, &last);

    char vbuf[40];
    for (int i = first; i < last; i++) {
        int row = pv_sv.row_y + (i - pv_sv.scroll);

        bool selected = ((int)pv_state.cursor_row == i);

        if (is_separator((ProjRow)i)) {
            font_fill_row(fb, row, 0, FONT_COLS, PAL_BG);
            font_puts(fb, 0, row, SEP_STR, PAL_DIM);
            continue;
        }

        u8 bg = selected ? PAL_ROW_CURSOR : PAL_BG;
        font_fill_row(fb, row, 0, FONT_COLS, bg);

        u8 label_color = selected ? PAL_WHITE : PAL_TEXT;
        if (is_action((ProjRow)i))
            label_color = selected ? PAL_ORANGE : PAL_INST;

        font_puts(fb, LABEL_COL, row, row_labels[i], label_color);

        /* Format value */
        vbuf[0] = '\0';
        u8 val_color = selected ? PAL_WHITE : PAL_GRAY;

        switch ((ProjRow)i) {
        case PROW_NAME:
            snprintf(vbuf, sizeof(vbuf), "%.32s", song.name);
            val_color = PAL_NOTE;
            break;
        case PROW_TEMPO:
            snprintf(vbuf, sizeof(vbuf), "%3d", song.initial_tempo);
            break;
        case PROW_SPEED:
            snprintf(vbuf, sizeof(vbuf), "%2d", song.initial_speed);
            break;
        case PROW_MASTER_VOL:
            snprintf(vbuf, sizeof(vbuf), "%3d", song.global_volume);
            break;
        case PROW_CHANNELS:
            snprintf(vbuf, sizeof(vbuf), "%2d", song.channel_count);
            break;
        case PROW_REPEAT_POS:
            snprintf(vbuf, sizeof(vbuf), "%3d", song.repeat_position);
            break;
        case PROW_FOLLOW_MODE:
            snprintf(vbuf, sizeof(vbuf), "%s",
                     cursor.follow ? "On" : "Off");
            val_color = cursor.follow ? PAL_PLAY : PAL_RED;
            break;
        case PROW_FONT_SIZE:
            snprintf(vbuf, sizeof(vbuf), "%s",
                     font_get_mode() == FONT_MODE_BIG ? "BIG" : "SMALL");
            val_color = PAL_NOTE;
            break;
        case PROW_DEBUG_OVERLAY:
            snprintf(vbuf, sizeof(vbuf), "N/A");
            val_color = PAL_DIM;
            break;
        case PROW_KEY_REPEAT_DELAY:
            snprintf(vbuf, sizeof(vbuf), "%2d fr (%dms)",
                     ui_repeat_delay, (int)ui_repeat_delay * 1000 / 60);
            break;
        case PROW_KEY_REPEAT_RATE:
            snprintf(vbuf, sizeof(vbuf), "%2d fr (%dms)",
                     ui_repeat_rate, (int)ui_repeat_rate * 1000 / 60);
            break;
        case PROW_INST_COUNT:
            snprintf(vbuf, sizeof(vbuf), "%d", song.inst_count);
            val_color = PAL_DIM;
            break;
        case PROW_SAMP_COUNT:
            snprintf(vbuf, sizeof(vbuf), "%d", song.samp_count);
            val_color = PAL_DIM;
            break;
        case PROW_PATT_COUNT:
            snprintf(vbuf, sizeof(vbuf), "%d", song.patt_count);
            val_color = PAL_DIM;
            break;
        case PROW_NEW_SONG:
        case PROW_COMPACT_PAT:
        case PROW_COMPACT_INST:
        case PROW_LOAD:
            if (pv_state.confirm_pending && (int)pv_state.cursor_row == i)
                snprintf(vbuf, sizeof(vbuf),
                         song_modified ? "[discard? A=yes B=no]"
                                       : "[A=confirm B=cancel]");
            break;
        case PROW_SAVE:
        case PROW_SAVE_AS:
            if (pv_state.confirm_pending && (int)pv_state.cursor_row == i)
                snprintf(vbuf, sizeof(vbuf), "[not available yet]");
            break;
        default:
            break;
        }

        if (vbuf[0])
            font_puts(fb, font_scale_col(VALUE_COL_SMALL), row,
                      vbuf, val_color);
    }

    int help_row = help_row_end;
    int tport_row = font_scale_row(31);
    int drawn = last - first;
    for (int r = DATA_ROW_START + drawn; r < help_row; r++)
        font_fill_row(fb, r, 0, FONT_COLS, PAL_BG);

    scroll_view_draw_scrollbar(&pv_sv, fb, FONT_COLS - 1);

    font_fill_row(fb, help_row, 0, FONT_COLS, PAL_BG);
    font_puts(fb, 0, help_row,
              "A+L/R:edit A+U/D:x10 A:action SEL+D:back",
              PAL_DIM);

    font_fill_row(fb, tport_row, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 1, tport_row, "PROJECT", PAL_GRAY);
}

/* ------------------------------------------------------------------ */
/* Bottom screen draw                                                  */
/* ------------------------------------------------------------------ */

static void draw_bottom(u8 *fb)
{
    font_fill_row(fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 0, "SONG INFO", PAL_TEXT);

    font_fill_row(fb, 2, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 2, PAL_TEXT, "Name: %.32s", song.name);

    font_fill_row(fb, 3, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 3, PAL_TEXT, "Orders: %d", song.order_count);

    font_fill_row(fb, 4, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 4, PAL_TEXT, "Patterns: %d allocated, %d in use",
                (int)song.patt_count, count_used_patterns());

    font_fill_row(fb, 5, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 5, PAL_TEXT, "Instruments: %d", song.inst_count);

    font_fill_row(fb, 6, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 6, PAL_TEXT, "Samples: %d", song.samp_count);

    font_fill_row(fb, 7, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 7, PAL_TEXT, "Channels: %d", song.channel_count);

    font_fill_row(fb, 8, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 8, PAL_TEXT, "Tempo: %d  Speed: %d",
                song.initial_tempo, song.initial_speed);

    /* Memory usage breakdown */
    MT_MemUsage mem;
    mt_mem_usage(&mem);
    u32 pct = (mem.total * 100) / mem.available;

    font_fill_row(fb, 10, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(fb, 0, 10, "MEMORY", PAL_TEXT);

    u8 mc = (pct > 90) ? PAL_RED : (pct > 70) ? PAL_ORANGE : PAL_TEXT;

    font_fill_row(fb, 11, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 11, mc, "Total: %lu KB / %lu KB (%lu%%)",
                (unsigned long)(mem.total / 1024),
                (unsigned long)(mem.available / 1024),
                (unsigned long)pct);

    font_fill_row(fb, 12, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 12, PAL_TEXT, "  Patterns: %lu KB (%d allocated)",
                (unsigned long)(mem.patterns / 1024),
                mem.patt_count);

    font_fill_row(fb, 13, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 13, PAL_TEXT, "  Samples:  %lu KB (%d active)",
                (unsigned long)(mem.samples / 1024),
                mem.samp_count);

    font_fill_row(fb, 14, 0, FONT_COLS, PAL_BG);
    font_printf(fb, 1, 14, PAL_TEXT, "  Song struct: %lu KB",
                (unsigned long)(mem.song_struct / 1024));

    /* Visual memory bar */
    int bar_max = FONT_COLS - 4;
    font_fill_row(fb, 16, 0, FONT_COLS, PAL_BG);
    font_puts(fb, 1, 16, "[", PAL_DIM);
    int bar_len = (int)(pct * bar_max / 100);
    if (bar_len > bar_max) bar_len = bar_max;
    for (int x = 0; x < bar_max; x++) {
        u8 c = (x < bar_len) ? mc : PAL_DIM;
        font_putc(fb, 2 + x, 16, (x < bar_len) ? '=' : '.', c);
    }
    font_puts(fb, 2 + bar_max, 16, "]", PAL_DIM);

    if (pct > 90) {
        font_fill_row(fb, 18, 0, FONT_COLS, PAL_BG);
        font_puts(fb, 1, 18, "WARNING: Low memory!", PAL_RED);
        font_puts(fb, 1, 19, "Reduce patterns or samples.", PAL_RED);
    }

    /* Clear rest */
    for (int r = 20; r < FONT_ROWS; r++)
        font_fill_row(fb, r, 0, FONT_COLS, PAL_BG);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void project_view_draw(u8 *top_fb, u8 *bot_fb)
{
    draw_top(top_fb);
    draw_bottom(bot_fb);
}

void project_view_input(u32 down, u32 held)
{
    /* --- Navigation --- */
    if (!(held & KEY_A)) {
        u32 rep = keysDownRepeat();
        if (rep & KEY_UP)   move_cursor(-1);
        if (rep & KEY_DOWN) move_cursor(1);
    }

    /* --- B: cancel confirmation --- */
    if (down & KEY_B) {
        if (pv_state.confirm_pending) {
            pv_state.confirm_pending = false;
        }
        return;
    }

    ProjRow row = (ProjRow)pv_state.cursor_row;

    /* --- A pressed on action rows: trigger / confirm --- */
    if ((down & KEY_A) && is_action(row)) {
        if (needs_confirm(row)) {
            if (pv_state.confirm_pending) {
                if (row == PROW_NEW_SONG) {
                    song_free();
                    song_init();
                    cursor.order_pos = 0;
                    cursor.row = 0;
                    song_modified = false;
                    autosave_dirty = false;
                    snprintf(status_msg, sizeof(status_msg), "New song");
                    status_timer = 120;
                } else if (row == PROW_COMPACT_PAT) {
                    compact_patterns();
                } else if (row == PROW_COMPACT_INST) {
                    compact_instruments();
                } else if (row == PROW_LOAD) {
                    pv_open_load_browser();
                }
                pv_state.confirm_pending = false;
            } else {
                pv_state.confirm_pending = true;
            }
        } else {
            if (row == PROW_SAVE || row == PROW_SAVE_AS) {
                snprintf(status_msg, sizeof(status_msg),
                         "Save not available yet");
                status_timer = 120;
            }
        }
        return;
    }

    /* --- A held + directions: edit values --- */
    if (held & KEY_A) {
        if (is_readonly(row) || is_separator(row) || is_action(row))
            return;
        if (row == PROW_NAME)
            return;  /* name editing needs text_input */

        u32 rep = keysDownRepeat();

        if (rep & KEY_LEFT)  adjust_value(row, -1);
        if (rep & KEY_RIGHT) adjust_value(row, 1);

        if (rep & KEY_UP) {
            switch (row) {
            case PROW_TEMPO:            adjust_value(row, 10);  break;
            case PROW_MASTER_VOL:       adjust_value(row, 10);  break;
            case PROW_SPEED:            adjust_value(row, 5);   break;
            case PROW_REPEAT_POS:       adjust_value(row, 10);  break;
            case PROW_KEY_REPEAT_DELAY: adjust_value(row, 5);   break;
            case PROW_KEY_REPEAT_RATE:  adjust_value(row, 5);   break;
            default: break;
            }
        }
        if (rep & KEY_DOWN) {
            switch (row) {
            case PROW_TEMPO:            adjust_value(row, -10); break;
            case PROW_MASTER_VOL:       adjust_value(row, -10); break;
            case PROW_SPEED:            adjust_value(row, -5);  break;
            case PROW_REPEAT_POS:       adjust_value(row, -10); break;
            case PROW_KEY_REPEAT_DELAY: adjust_value(row, -5);  break;
            case PROW_KEY_REPEAT_RATE:  adjust_value(row, -5);  break;
            default: break;
            }
        }
        return;
    }
}
