/*
 * sample_view.c — Sample viewer for maxtracker.
 *
 * Top screen: waveform display + sample info parameters.
 * Bottom screen: sample metadata (length, rate, loop points, volume).
 *
 * This file is read-only with respect to sample data: it visualizes
 * an existing sample but does not allow editing or generation. The
 * editor — drawing, synthesis, FX — lives in the Waveform Editor
 * (lib/lfe + arm9/source/ui/waveform_view.c) and is opened from this
 * view via SELECT+RIGHT when MAXTRACKER_LFE is defined.
 *
 * The free-drawing code that used to live here was lifted into the
 * lfe library in Phase 2. The split is: maxtracker visualizes,
 * waveform_view edits.
 */

#include "sample_view.h"
#include "screen.h"
#include "font.h"
#include "draw_util.h"
#include "waveform_render.h"
#include "editor_state.h"  /* cursor.instrument for current sample */
#include "song.h"
#include "playback.h"      /* playback_rebuild_mas for live edits */
#include "filebrowser.h"
#include "text_input.h"
#ifdef MAXTRACKER_LFE
#include "waveform_view.h" /* Waveform Editor menu (Phase 0+) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keybind.h"

extern FileBrowser disk_browser;
extern bool        disk_browser_inited;
extern u8          sample_load_target;
extern u8          sample_save_target;
extern char        status_msg[64];
extern int         status_timer;
extern ScreenMode  disk_return_screen;
/* dirty flags: use mt_mark_song_modified() from editor_state.h */

/* Internal state. Drawing fields were removed in Phase 2 — they live
 * in waveform_view.c now. */
static struct {
    u8   selected;      /* current sample index (0-based, display as 1-based) */
    u8   action_row;    /* 0 = Load, 1 = Save, 2 = Rename */
    bool confirm_pending;     /* action-row SAVE/LOAD confirm */
    bool delete_confirm_pending; /* B+A two-tap confirm to free PCM */
    int  zoom;          /* waveform zoom level: 0=fit, 1=2x, 2=4x, etc. */
    int  scroll;        /* horizontal scroll offset in samples */
} sv;

#define SV_ACTION_COUNT 3
#define SV_ACTION_LOAD    0
#define SV_ACTION_SAVE    1
#define SV_ACTION_RENAME  2

void sample_view_reset_transient(void)
{
    sv.confirm_pending = false;
    sv.delete_confirm_pending = false;
}

void sample_view_set_selected(u8 index)
{
    if (index < MT_MAX_SAMPLES) {
        sv.selected = index;
        sv.scroll = 0;
        sv.zoom = 0;
    }
}

u8 sample_view_get_selected(void)
{
    return sv.selected;
}

/* ------------------------------------------------------------------ */
/* LOAD / SAVE actions                                                 */
/* ------------------------------------------------------------------ */

static void sv_open_load_browser(void)
{
    sample_load_target = sv.selected + 1;   /* 1-based flag */
    filebrowser_init(&disk_browser, fs_browse_root);
    disk_browser_inited = true;
    disk_return_screen = SCREEN_SAMPLE;
    screen_set_mode(SCREEN_DISK);
    font_clear(top_fb, PAL_BG);
    font_clear(bot_fb, PAL_BG);
}

/* Open the disk browser in SAVE mode so the user can pick a folder.
 * The actual write is performed in main.c's SCREEN_DISK handler when
 * START fires (and after any overwrite-confirm dialog). The filename
 * is composed from the sample's name (sanitized) or "sample_XX" as
 * a fallback. */
static void sv_open_save_browser(void)
{
    MT_Sample *s = &song.samples[sv.selected];
    if (!s->active || !s->pcm_data || s->length == 0) {
        snprintf(status_msg, sizeof(status_msg), "No sample to save");
        status_timer = 180;
        return;
    }

    sample_save_target = sv.selected + 1;   /* 1-based */
    sample_load_target = 0;                 /* mutually exclusive with load */
    filebrowser_init(&disk_browser, fs_browse_root);
    filebrowser_set_mode(&disk_browser, FB_MODE_SAVE);
    disk_browser_inited = true;
    disk_return_screen = SCREEN_SAMPLE;
    screen_set_mode(SCREEN_DISK);
    font_clear(top_fb, PAL_BG);
    font_clear(bot_fb, PAL_BG);
}

/* ------------------------------------------------------------------ */
/* Waveform drawing (top screen, rows 2-21 = 20 rows = 120px)         */
/* ------------------------------------------------------------------ */

/* Scope region: starts at font row 2 in pixels (varies with font mode),
 * ends just above the parameter header (font_scale_row(20)). */
#define WAVE_X_START  0
#define WAVE_X_WIDTH  256

static inline int wave_y_start(void)  { return 2 * FONT_H; }
static inline int wave_y_height(void) { return font_scale_row(20) * FONT_H - wave_y_start(); }

static void draw_waveform_top(u8 *fb, MT_Sample *s)
{
    int wy = wave_y_start();
    int wh = wave_y_height();

    if (!s || !s->active || !s->pcm_data || s->length == 0) {
        /* Still clear + center-line the scope region so empty slots
         * look consistent with populated ones. */
        WaveformRenderCfg cfg = {
            .fb = fb, .y_top = wy, .y_height = wh,
            .color_bg = PAL_BG, .color_center = PAL_DIM,
        };
        waveform_render(&cfg);
        font_puts(fb, 8, 10, "No sample data", PAL_DIM);
        return;
    }

    WaveformRenderCfg cfg = {
        .fb           = fb,
        .y_top        = wy,
        .y_height     = wh,
        .pcm          = s->pcm_data,
        .is_16bit     = (s->bits == 16),
        .length       = s->length,
        .visible      = waveform_zoom_levels[sv.zoom],
        .scroll       = (u32)sv.scroll,
        .color_bg     = PAL_BG,
        .color_center = PAL_DIM,
        .color_wave   = PAL_PLAY,
        .loop_start   = s->loop_start,
        .loop_length  = s->loop_length,
        .color_loop   = PAL_ORANGE,
    };
    waveform_render(&cfg);
}

/* ------------------------------------------------------------------ */
/* Bottom screen: sample info                                         */
/* ------------------------------------------------------------------ */

static void draw_bottom_sample(u8 *bot_fb, MT_Sample *s)
{
    font_clear(bot_fb, PAL_BG);

    font_fill_row(bot_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(bot_fb, 0, 0, PAL_TEXT, "SAMPLE %02X", sv.selected + 1);

    if (s && s->active) {
        font_printf(bot_fb, 14, 0, PAL_GRAY, "\"%s\"", s->name);

        font_printf(bot_fb, 1, 2, PAL_TEXT, "Length....... %lu", s->length);
        font_printf(bot_fb, 1, 3, PAL_TEXT, "Rate......... %lu Hz", s->base_freq);
        font_printf(bot_fb, 1, 4, PAL_TEXT, "Bits......... %d", s->bits);
        font_printf(bot_fb, 1, 5, PAL_TEXT, "Loop start... %lu", s->loop_start);
        font_printf(bot_fb, 1, 6, PAL_TEXT, "Loop length.. %lu", s->loop_length);
        font_printf(bot_fb, 1, 7, PAL_TEXT, "Volume....... %d", s->default_volume);
        font_printf(bot_fb, 1, 8, PAL_TEXT, "Panning...... %d", s->panning);

        u32 bytes = s->length * (s->bits == 16 ? 2 : 1);
        font_printf(bot_fb, 1, 10, PAL_DIM, "Size: %lu bytes", bytes);
    } else {
        font_puts(bot_fb, 1, 2, "(empty)", PAL_DIM);
        font_puts(bot_fb, 1, 4, "Load a WAV from the disk screen",     PAL_DIM);
#ifdef MAXTRACKER_LFE
        font_puts(bot_fb, 1, 5, "or press Y+A to open the Waveform Editor",
                  PAL_DIM);
#endif
    }

    int help_row = font_scale_row(30);
    int info_row = font_scale_row(31);

    /* Transient status line (row 29 — just above help) so "Saved:" /
     * "Partial load:" messages from the disk screen remain visible
     * after auto-return. Countdown happens in the main loop. */
    int status_row = font_scale_row(29);
    font_fill_row(bot_fb, status_row, 0, FONT_COLS, PAL_BG);
    if (status_timer > 0)
        font_puts(bot_fb, 1, status_row, status_msg, PAL_WHITE);

    font_fill_row(bot_fb, help_row, 0, FONT_COLS, PAL_HEADER_BG);
#ifdef MAXTRACKER_LFE
    font_puts(bot_fb, 0, help_row, "B+A:del  L/R:prev/next  X:zoom  SEL+R:LFE",
              PAL_DIM);
#else
    font_puts(bot_fb, 0, help_row, "B+A:del  L/R:prev/next  X:zoom", PAL_DIM);
#endif
    font_fill_row(bot_fb, info_row, 0, FONT_COLS, PAL_HEADER_BG);
    if (sv.zoom == 0)
        font_printf(bot_fb, 0, info_row, PAL_DIM, "Sample %02X/%02X  Zoom:FIT",
                    sv.selected + 1, song.samp_count);
    else
        font_printf(bot_fb, 0, info_row, PAL_DIM, "Sample %02X/%02X  Zoom:%lu",
                    sv.selected + 1, song.samp_count,
                    (unsigned long)waveform_zoom_levels[sv.zoom]);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void sample_view_draw(u8 *top_fb, u8 *bot_fb)
{
    /* Modal keyboard steals both screens. On the close frame this also
     * does a one-shot fb clear so the keyboard cells don't ghost into
     * the sample view underneath. */
    if (text_input_draw_and_consume(top_fb, bot_fb))
        return;

    MT_Sample *s = (sv.selected < MT_MAX_SAMPLES) ?
                       &song.samples[sv.selected] : NULL;

    /* Top screen */
    font_fill_row(top_fb, 0, 0, FONT_COLS, PAL_HEADER_BG);
    font_printf(top_fb, 0, 0, PAL_TEXT, "SAMPLE %02X", sv.selected + 1);
    if (s && s->active && s->name[0])
        font_printf(top_fb, 12, 0, PAL_GRAY, "\"%s\"", s->name);

    font_printf(top_fb, font_scale_col(40), 0, PAL_DIM, "%d-bit %luHz",
                (s && s->active) ? s->bits : 8,
                (s && s->active) ? (unsigned long)s->base_freq : 8363UL);

    /* Column headers (row 1) */
    font_fill_row(top_fb, 1, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 0, 1, "Waveform", PAL_GRAY);
    if (s && s->active)
        font_printf(top_fb, font_scale_col(20), 1, PAL_DIM,
                    "Len:%lu Loop:%lu+%lu",
                    s->length, s->loop_start, s->loop_length);

    /* Waveform (rows 2-19) */
    draw_waveform_top(top_fb, s);

    /* Info area */
    int param_hdr = font_scale_row(20);
    int footer    = font_scale_row(30);
    int tport     = font_scale_row(31);
    font_fill_row(top_fb, param_hdr, 0, FONT_COLS, PAL_HEADER_BG);
    font_puts(top_fb, 0, param_hdr, "Parameters", PAL_GRAY);

    if (s && s->active) {
        font_printf(top_fb, 1, param_hdr + 1, PAL_TEXT,
                    "Volume:  %3d", s->default_volume);
        font_printf(top_fb, 1, param_hdr + 2, PAL_TEXT,
                    "Panning: %3d", s->panning);
        font_printf(top_fb, 1, param_hdr + 3, PAL_TEXT,
                    "Rate:    %lu Hz", s->base_freq);
    }

    /* Action buttons (Load / Save / Rename) — navigable with UP/DOWN,
     * A triggers. Anchor the action block to the footer so it never
     * overlaps the help/transport strip in BIG mode (where the
     * content area shrinks from 30 to 22 rows). */
    {
        static const char *labels[SV_ACTION_COUNT] = {
            ">> Load .wav",
            ">> Save .wav",
            ">> Rename",
        };
        int base = footer - SV_ACTION_COUNT - 1;  /* 1-row gap above help */
        /* Must not overlap the 3 param rows at param_hdr+1..+3. */
        if (base < param_hdr + 4) base = param_hdr + 4;
        /* Last-resort clamp if even the param rows would overflow
         * (tiny screens / odd font modes). */
        if (base + SV_ACTION_COUNT > footer)
            base = footer - SV_ACTION_COUNT;
        for (int i = 0; i < SV_ACTION_COUNT; i++) {
            int row = base + i;
            bool selected = ((int)sv.action_row == i);
            u8 bg = selected ? PAL_ROW_CURSOR : PAL_BG;
            font_fill_row(top_fb, row, 0, FONT_COLS, bg);
            u8 col = selected ? PAL_ORANGE : PAL_INST;
            font_puts(top_fb, 1, row, labels[i], col);
            if (sv.confirm_pending && selected) {
                const char *msg = (i == SV_ACTION_LOAD)
                    ? "[discard? A=yes B=no]"
                    : "[overwrite? A=yes B=no]";
                font_puts(top_fb, font_scale_col(22), row, msg,
                          selected ? PAL_WHITE : PAL_DIM);
            }
        }
    }

    /* Footer */
    font_fill_row(top_fb, footer, 0, FONT_COLS, PAL_BG);
    if (sv.zoom == 0)
        font_printf(top_fb, 0, footer, PAL_TEXT, "Sample %02X/%02X  Zoom:FIT",
                    sv.selected + 1, song.samp_count);
    else
        font_printf(top_fb, 0, footer, PAL_TEXT, "Sample %02X/%02X  Zoom:%lu",
                    sv.selected + 1, song.samp_count,
                    (unsigned long)waveform_zoom_levels[sv.zoom]);

    font_fill_row(top_fb, tport, 0, FONT_COLS, PAL_HEADER_BG);
    if (sv.delete_confirm_pending) {
        font_puts(top_fb, 0, tport,
                  "Delete sample?  B+A:yes  any:no", PAL_ORANGE);
    } else {
        font_puts(top_fb, 0, tport,
                  "L/R:prev/next  UP/DN:action  A:trigger  X:zoom",
                  PAL_DIM);
    }

    /* Bottom screen — sample metadata. The Waveform Editor (when
     * active) takes over both screens; that case is handled at the
     * top of this function. */
    draw_bottom_sample(bot_fb, s);
}

void sample_view_input(u32 down, u32 held)
{
    /* Modal keyboard — forward and return until it closes. */
    if (text_input_is_active()) {
        text_input_input(down, held);
        return;
    }

    /* LFE is now a proper room (SCREEN_LFE) navigated via SELECT+RIGHT
     * from SCREEN_SAMPLE. No Y+A binding, no is_active delegation — the
     * main loop routes to waveform_view_input directly when in SCREEN_LFE. */

    /* ---- Normal sample view mode ---- */

    /* B+A: delete/clear the current sample (two-tap confirm).
     * Without the confirm, a stray B+A frees PCM that an active
     * playback/LFE draft may still be dereferencing. */
    if ((held & MT_KEY_BACK) && (down & MT_KEY_CONFIRM)) {
        MT_Sample *s = &song.samples[sv.selected];
        if (!s->active) return;
        if (!sv.delete_confirm_pending) {
            sv.delete_confirm_pending = true;
            return;
        }
        sv.delete_confirm_pending = false;
        {
            if (s->pcm_data) {
                free(s->pcm_data);
                s->pcm_data = NULL;
            }
            s->active = false;
            s->length = 0;
            s->loop_start = 0;
            s->loop_length = 0;
            s->loop_type = 0;
            s->format = 0;
            s->bits = 8;
            s->base_freq = 8363;
            s->default_volume = 64;
            s->panning = 128;
            s->global_volume = 64;
            s->drawn = false;
            s->vib_type = 0;
            s->vib_depth = 0;
            s->vib_speed = 0;
            s->vib_rate = 0;
            memset(s->name, 0, sizeof(s->name));
            if (playback_is_playing())
                playback_rebuild_mas();
        }
        mt_mark_song_modified();
        return;
    }

    /* Any non-B+A input cancels a pending delete confirm. */
    if (sv.delete_confirm_pending && down) {
        sv.delete_confirm_pending = false;
        /* Fall through — let the input still do its thing. */
    }

    /* Repeat-aware d-pad: holding a direction navigates continuously
     * after ui_repeat_delay frames, one step per ui_repeat_rate frames. */
    u32 rep = keysDownRepeat();

    if (rep & MT_KEY_SHOULDER_L) {
        if (sv.selected > 0) sv.selected--;
        sv.scroll = 0;
        sv.confirm_pending = false;
    }
    if (rep & MT_KEY_SHOULDER_R) {
        if (sv.selected < MT_MAX_SAMPLES - 1) sv.selected++;
        sv.scroll = 0;
        sv.confirm_pending = false;
    }

    /* Action cursor UP/DOWN + A triggers. */
    if (rep & KEY_UP) {
        if (sv.action_row > 0) sv.action_row--;
        sv.confirm_pending = false;
    }
    if (rep & KEY_DOWN) {
        if (sv.action_row + 1 < SV_ACTION_COUNT) sv.action_row++;
        sv.confirm_pending = false;
    }
    if ((down & MT_KEY_BACK) && sv.confirm_pending) {
        sv.confirm_pending = false;
        return;
    }
    if ((down & MT_KEY_CONFIRM) && !(held & MT_KEY_BACK)) {
        MT_Sample *s = &song.samples[sv.selected];
        if (sv.action_row == SV_ACTION_LOAD) {
            /* LOAD: confirm if the slot has unsaved data. */
            if (s->active && !sv.confirm_pending) {
                sv.confirm_pending = true;
            } else {
                sv.confirm_pending = false;
                sv_open_load_browser();
            }
        } else if (sv.action_row == SV_ACTION_SAVE) {
            /* SAVE opens a folder picker (disk browser in SAVE mode).
             * The overwrite-confirm step is handled by main.c's
             * SCREEN_DISK branch after the user commits with START —
             * so no in-row confirm here. */
            sv.confirm_pending = false;
            sv_open_save_browser();
        } else if (sv.action_row == SV_ACTION_RENAME) {
            sv.confirm_pending = false;
            text_input_open(s->name, sizeof(s->name) - 1,
                            "Rename Sample");
        }
        return;
    }
    if (down & MT_KEY_MOD_PRIMARY) {
        sv.zoom++;
        if (sv.zoom >= WAVEFORM_ZOOM_COUNT) sv.zoom = 0;
        sv.scroll = 0;
    }
    if (rep & KEY_LEFT) {
        if (sv.scroll > 0) {
            u32 vis = waveform_zoom_levels[sv.zoom];
            int step = (vis > 0) ? (int)(vis / 4) : 256;
            sv.scroll -= step;
            if (sv.scroll < 0) sv.scroll = 0;
        }
    }
    if (rep & KEY_RIGHT) {
        MT_Sample *s = &song.samples[sv.selected];
        if (s->active && s->length > 0) {
            u32 vis = waveform_zoom_levels[sv.zoom];
            int step = (vis > 0) ? (int)(vis / 4) : 256;
            sv.scroll += step;
            if ((u32)sv.scroll >= s->length) sv.scroll = s->length - 1;
        }
    }

    /* The plain A key has no action in the read-only sample view.
     * SELECT+RIGHT navigates to the Waveform Editor (SCREEN_LFE). */

    /* (B no longer navigates — use SELECT+LEFT to return to instrument) */
}
