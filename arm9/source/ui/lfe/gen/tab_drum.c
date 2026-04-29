/*
 * tab_drum.c — Drum generator tab for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Percussion generator driven by a kit of presets (kick/snare/hat/
 * tom/clap) plus full explicit control over tone oscillator, noise,
 * filter, master gain, and three modulation slots. The mod slots have
 * their own subpage (opened with A+R on the "Mod Slots >>" row); the
 * subpage uses the parent's B-to-close gate.
 *
 * Follows the tab-module template:
 *   - DRUM_PARAMS / DRUM_MOD_PARAMS X-macro lists drive the enum,
 *     label table, and per-row dispatch arrays.
 *   - `dm.params` is the source of truth; dB shadows avoid LUT
 *     round-trip drift.
 *   - Exports `wv_tab_drum` via tab_drum.h for the dispatcher.
 */

#ifdef MAXTRACKER_LFE

#include "tab_drum.h"

#include "wv_internal.h"
#include "wv_common.h"
#include "lfe_row_ui.h"

#include "font.h"
#include "screen.h"
#include "debug_view.h"

#include "lfe.h"
#include "lfe_dbmath.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Row lists                                                           */
/* ------------------------------------------------------------------ */

#define DRUM_PARAMS(X) \
    X(PRESET,       "Preset",        dfmt_preset,    dadj_preset)    \
    X(LENGTH_MS,    "Length (ms)",   dfmt_length,    dadj_length)    \
    X(TONE_HZ,      "Tone (Hz)",     dfmt_tone_hz,   dadj_tone_hz)   \
    X(TONE_WAVE,    "Tone Wave",     dfmt_tone_wave, dadj_tone_wave) \
    X(TONE_LEVEL,   "Tone Level",    dfmt_tone_lvl,  dadj_tone_lvl)  \
    X(NOISE_LEVEL,  "Noise Level",   dfmt_noise_lvl, dadj_noise_lvl) \
    X(FILTER_MODE,  "Filter Mode",   dfmt_fmode,     dadj_fmode)     \
    X(FILTER_CUT,   "Filter Cut",    dfmt_fcut,      dadj_fcut)      \
    X(FILTER_Q,     "Filter Q",      dfmt_fq,        dadj_fq)        \
    X(DRIVE,        "Drive",         dfmt_drive,     dadj_drive)     \
    X(MASTER,       "Master",        dfmt_master,    dadj_master)    \
    X(EDIT_MODS,    "Mod Slots >>",  dfmt_edit_mods, dadj_edit_mods) \
    X(EDIT_LFO,     "LFO       >>",  dfmt_edit_lfo,  dadj_edit_lfo)

typedef enum {
#define X(name, label, fmt, adj) DRUM_PARAM_##name,
    DRUM_PARAMS(X)
#undef X
    DRUM_PARAM_COUNT,
} DrumParam;

static const char *drum_param_labels[DRUM_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    DRUM_PARAMS(X)
#undef X
};

/* Mod-slot subpage. Drum env has no release (no note-off in this
 * generator), so the subpage skips that field. */
#define DRUM_MOD_PARAMS(X) \
    X(SLOT,     "Slot",         dmfmt_slot,    dmadj_slot)    \
    X(TARGET,   "Target",       dmfmt_target,  dmadj_target)  \
    X(DEPTH,    "Depth",        dmfmt_depth,   dmadj_depth)   \
    X(ATTACK,   "Attack (ms)",  dmfmt_attack,  dmadj_attack)  \
    X(DECAY,    "Decay (ms)",   dmfmt_decay,   dmadj_decay)   \
    X(SUSTAIN,  "Sustain",      dmfmt_sustain, dmadj_sustain) \
    X(PEAK,     "Peak",         dmfmt_peak,    dmadj_peak)

typedef enum {
#define X(name, label, fmt, adj) DRUM_MOD_PARAM_##name,
    DRUM_MOD_PARAMS(X)
#undef X
    DRUM_MOD_PARAM_COUNT,
} DrumModParam;

static const char *drum_mod_param_labels[DRUM_MOD_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    DRUM_MOD_PARAMS(X)
#undef X
};

/* LFO subpage (single LFO slot — the drum has one, unlike FM4's two).
 * All drum LFO destinations take one implicit target (no target-byte
 * cursor needed), so the subpage drops that row entirely. */
#define DRUM_LFO_PARAMS(X) \
    X(SHAPE,    "Shape",        dlfmt_shape,   dladj_shape)   \
    X(DEST,     "Dest",         dlfmt_dest,    dladj_dest)    \
    X(RATE,     "Rate Hz",      dlfmt_rate,    dladj_rate)    \
    X(DEPTH,    "Depth",        dlfmt_depth,   dladj_depth)

typedef enum {
#define X(name, label, fmt, adj) DRUM_LFO_PARAM_##name,
    DRUM_LFO_PARAMS(X)
#undef X
    DRUM_LFO_PARAM_COUNT,
} DrumLfoParam;

static const char *drum_lfo_param_labels[DRUM_LFO_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    DRUM_LFO_PARAMS(X)
#undef X
};

#define DRUM_PRESET_COUNT LFE_DRUM_PRESET_COUNT
static const char *drum_preset_names[DRUM_PRESET_COUNT] = {
    "KICK", "SNARE", "HAT CLOSED", "HAT OPEN", "TOM", "CLAP",
    "KICK 808", "COWBELL",
};

#define DRUM_MOD_TARGET_COUNT 6
static const char *drum_mod_target_names[DRUM_MOD_TARGET_COUNT] = {
    "NONE", "AMP", "PITCH", "FILTER", "TONE.LV", "NOISE.LV",
};

/* Tone waveshape names (indexed by lfe_drum_wave). */
static const char *drum_wave_names[LFE_DRUM_WAVE_COUNT] = {
    "SINE", "TRI", "SQ", "SAW",
};

/* Shared LFO shape labels. Same copy lives in tab_fm4 — duplicating
 * the short strings is cheaper than coupling the two tabs. */
static const char *drum_lfo_shape_names[LFE_LFO_SHAPE_COUNT] = {
    "SINE", "TRI", "SQ", "SAW+", "SAW-",
};

static const char *drum_lfo_dest_names[LFE_DRUM_LFO_DEST_COUNT] = {
    "OFF", "TONE_HZ", "TONE_LV", "NOISE_LV", "FILT_CUT", "MASTER", "DRIVE",
};

/* Filter mode names. Same four modes the synth tab labels; kept local
 * here so tab_drum doesn't depend on tab_synth. */
static const char *drum_filter_names[4] = { "LP", "HP", "BP", "NOTCH" };

/* File-scoped state (Step 5 encapsulation). `params` is the source of
 * truth; `*_db` fields shadow the Q15 gain fields in dB Q8.8 so
 * repeated edits don't drift through the LUT round-trip. */
static struct {
    int param_row;
    int preset;
    int tone_db;
    int noise_db;
    int master_db;
    lfe_drum_params params;
    int subpage;         /* 0 main, 1 mod-slot editor, 2 LFO editor */
    int mod_slot;
    int mod_param_row;
    int lfo_param_row;
    int mod_sustain_db[LFE_DRUM_NUM_MODS];
    int mod_peak_db[LFE_DRUM_NUM_MODS];
} dm;

static ScrollView drum_sv     = { .row_y = 5, .margin = 2 };
static ScrollView drum_mod_sv = { .row_y = 5, .margin = 2 };
static ScrollView drum_lfo_sv = { .row_y = 5, .margin = 2 };

/* ------------------------------------------------------------------ */
/* Preset load + generate                                              */
/* ------------------------------------------------------------------ */

static void drum_refresh_db_shadows(void)
{
    dm.tone_db   = lfe_q15_to_db((int16_t)dm.params.tone_level);
    dm.noise_db  = lfe_q15_to_db((int16_t)dm.params.noise_level);
    dm.master_db = lfe_q15_to_db((int16_t)dm.params.master_level);
    for (int i = 0; i < LFE_DRUM_NUM_MODS; i++) {
        dm.mod_sustain_db[i] =
            lfe_q15_to_db((int16_t)dm.params.mods[i].env.sustain_level);
        dm.mod_peak_db[i] =
            lfe_q15_to_db((int16_t)dm.params.mods[i].env.peak_level);
    }
}

static void drum_load_preset(int preset_idx)
{
    lfe_status rc = lfe_drum_fill_preset(&dm.params,
                                         (lfe_drum_preset)preset_idx);
    if (rc != LFE_OK) {
        dbg_set_last_error("drum: fill_preset rc=%d", (int)rc);
        return;
    }
    drum_refresh_db_shadows();
}

static void drum_generate(void)
{
    dbg_log("drum_generate preset=%d len=%d", dm.preset, wv.length_ms);

    const uint32_t rate   = 32000u;
    const uint32_t length = (uint32_t)wv.length_ms * rate / 1000u;
    if (length == 0) {
        dbg_set_last_error("drum: zero length");
        snprintf(wv.status, sizeof(wv.status), "Duration too short");
        wv.status_timer = 180;
        return;
    }

    lfe_drum_params params = dm.params;

    int16_t *pcm = (int16_t *)malloc((size_t)length * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("drum: malloc failed (%lu bytes)",
                           (unsigned long)(length * sizeof(int16_t)));
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer out = { .data = pcm, .length = length, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_drum(&out, &params);
    if (rc < 0) {
        free(pcm);
        dbg_set_last_error("drum: gen rc=%d", (int)rc);
        snprintf(wv.status, sizeof(wv.status), "Generate failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    wv_write_draft_16(pcm, length, rate, 0, 0, "Drum");

    const char *name = drum_preset_names[dm.preset];
    if (rc == LFE_WARN_CLIPPED)
        snprintf(wv.status, sizeof(wv.status), "%s %d ms (clipped)", name, wv.length_ms);
    else
        snprintf(wv.status, sizeof(wv.status), "%s %d ms", name, wv.length_ms);
    wv.status_timer = 180;
}

/* ------------------------------------------------------------------ */
/* Per-row functions (main page)                                       */
/* ------------------------------------------------------------------ */

static void dfmt_preset(char *b, int n) { snprintf(b, n, "%s", drum_preset_names[dm.preset]); }
static void dadj_preset(int d) {
    wv_cycle(&dm.preset, d, DRUM_PRESET_COUNT);
    drum_load_preset(dm.preset);
}

static void dfmt_length(char *b, int n) { wv_format_length_with_size(b, n, wv.length_ms, 32000u); }
static void dadj_length(int d)          { wv_int_clamped(&wv.length_ms, d, 50, 50, 4000); }

static void dfmt_tone_hz(char *b, int n) { snprintf(b, n, "%5lu", (unsigned long)(dm.params.tone_base_hz_q8 >> 8)); }
static void dadj_tone_hz(int d) {
    int hz = (int)(dm.params.tone_base_hz_q8 >> 8);
    wv_int_clamped(&hz, d, 1, 20, 8000);
    dm.params.tone_base_hz_q8 = (uint32_t)hz << 8;
}

static void dfmt_tone_lvl(char *b, int n)  { snprintf(b, n, "%+4d dB", dm.tone_db >> 8); }
static void dadj_tone_lvl(int d)           { wv_db_step(&dm.tone_db, &dm.params.tone_level, d); }
static void dfmt_noise_lvl(char *b, int n) { snprintf(b, n, "%+4d dB", dm.noise_db >> 8); }
static void dadj_noise_lvl(int d)          { wv_db_step(&dm.noise_db, &dm.params.noise_level, d); }

static void dfmt_fmode(char *b, int n) { snprintf(b, n, "%s", drum_filter_names[dm.params.filter_mode]); }
static void dadj_fmode(int d)          { int v = dm.params.filter_mode; wv_cycle(&v, d, 4); dm.params.filter_mode = (lfe_drum_filter_mode)v; }
static void dfmt_fcut(char *b, int n)  { snprintf(b, n, "%5lu", (unsigned long)dm.params.filter_base_hz); }
static void dadj_fcut(int d) {
    int v = (int)dm.params.filter_base_hz;
    wv_int_clamped(&v, d, 50, 50, 16000);
    dm.params.filter_base_hz = (uint32_t)v;
}
static void dfmt_fq(char *b, int n) { snprintf(b, n, "%3d%%", (dm.params.filter_q * 100) / LFE_Q15_ONE); }
static void dadj_fq(int d) {
    int v = dm.params.filter_q;
    wv_int_clamped(&v, d, 256, 0, LFE_Q15_ONE);
    dm.params.filter_q = (uint16_t)v;
}

static void dfmt_master(char *b, int n) { snprintf(b, n, "%+4d dB", dm.master_db >> 8); }
static void dadj_master(int d)          { wv_db_step(&dm.master_db, &dm.params.master_level, d); }

static void dfmt_tone_wave(char *b, int n) {
    int w = dm.params.tone_wave;
    if (w < 0 || w >= LFE_DRUM_WAVE_COUNT) w = 0;
    snprintf(b, n, "%s", drum_wave_names[w]);
}
static void dadj_tone_wave(int d) {
    int v = dm.params.tone_wave;
    wv_cycle(&v, d, LFE_DRUM_WAVE_COUNT);
    dm.params.tone_wave = (uint8_t)v;
}

static void dfmt_drive(char *b, int n) {
    int v = dm.params.drive;
    snprintf(b, n, "%3d%%", (v * 100) / LFE_Q15_ONE);
}
static void dadj_drive(int d) {
    int v = dm.params.drive;
    wv_int_clamped(&v, d, 328, 0, LFE_Q15_ONE);   /* ~1% per click */
    dm.params.drive = (uint16_t)v;
}

static void dfmt_edit_mods(char *b, int n) { (void)b; (void)n; b[0] = '\0'; }
static void dadj_edit_mods(int d)          { if (d >= 0) dm.subpage = 1; }
static void dfmt_edit_lfo(char *b, int n)  { (void)b; (void)n; b[0] = '\0'; }
static void dadj_edit_lfo(int d)           { if (d >= 0) dm.subpage = 2; }

/* ------------------------------------------------------------------ */
/* Per-row functions (mod subpage)                                     */
/* ------------------------------------------------------------------ */

#define DRUM_MOD_CUR()  (&dm.params.mods[dm.mod_slot])

static int  dmod_target(void)         { return dm.params.mods[dm.mod_slot].target; }
static bool dmod_target_is_q15(int t) {
    return t == LFE_DRUM_MOD_AMP || t == LFE_DRUM_MOD_TONE_LEVEL ||
           t == LFE_DRUM_MOD_NOISE_LEVEL;
}
static bool dmod_target_is_hz(int t)  {
    return t == LFE_DRUM_MOD_PITCH || t == LFE_DRUM_MOD_FILTER;
}

static void dmfmt_slot(char *b, int n) { snprintf(b, n, "%d/%d", dm.mod_slot + 1, LFE_DRUM_NUM_MODS); }
static void dmadj_slot(int d)          { wv_cycle(&dm.mod_slot, d, LFE_DRUM_NUM_MODS); }

static void dmfmt_target(char *b, int n) { snprintf(b, n, "%s", drum_mod_target_names[dmod_target()]); }
static void dmadj_target(int d) {
    int v = DRUM_MOD_CUR()->target;
    wv_cycle(&v, d, DRUM_MOD_TARGET_COUNT);
    DRUM_MOD_CUR()->target = (lfe_drum_mod_target)v;
}

static void dmfmt_depth(char *b, int n) {
    int t = dmod_target();
    int v = DRUM_MOD_CUR()->depth;
    if (t == LFE_DRUM_MOD_NONE)         snprintf(b, n, "---");
    else if (dmod_target_is_q15(t))     snprintf(b, n, "%+4d %%", (v * 100) / LFE_Q15_ONE);
    else if (dmod_target_is_hz(t))      snprintf(b, n, "%+5d Hz", v);
    else                                 snprintf(b, n, "%+5d", v);
}
static void dmadj_depth(int d) {
    int t = dmod_target();
    int step, lo = -32768, hi = 32767;
    if (t == LFE_DRUM_MOD_NONE) return;
    if      (dmod_target_is_q15(t)) { step = 256; lo = -LFE_Q15_ONE; hi = LFE_Q15_ONE; }
    else if (dmod_target_is_hz(t))  { step = 50;  lo = -8000;        hi = 8000; }
    else                             { step = 256; }
    int v = DRUM_MOD_CUR()->depth;
    wv_int_clamped(&v, d, step, lo, hi);
    DRUM_MOD_CUR()->depth = v;
}

static void dmfmt_attack(char *b, int n) { snprintf(b, n, "%5lu", (unsigned long)DRUM_MOD_CUR()->env.attack_ms); }
static void dmadj_attack(int d) {
    int v = (int)DRUM_MOD_CUR()->env.attack_ms;
    wv_int_clamped(&v, d, 10, 0, 4000);
    DRUM_MOD_CUR()->env.attack_ms = (uint32_t)v;
}
static void dmfmt_decay(char *b, int n)  { snprintf(b, n, "%5lu", (unsigned long)DRUM_MOD_CUR()->env.decay_ms); }
static void dmadj_decay(int d) {
    int v = (int)DRUM_MOD_CUR()->env.decay_ms;
    wv_int_clamped(&v, d, 10, 0, 4000);
    DRUM_MOD_CUR()->env.decay_ms = (uint32_t)v;
}
static void dmfmt_sustain(char *b, int n) { snprintf(b, n, "%+4d dB", dm.mod_sustain_db[dm.mod_slot] >> 8); }
static void dmadj_sustain(int d) {
    wv_db_step(&dm.mod_sustain_db[dm.mod_slot],
               &DRUM_MOD_CUR()->env.sustain_level, d);
}
static void dmfmt_peak(char *b, int n)    { snprintf(b, n, "%+4d dB", dm.mod_peak_db[dm.mod_slot] >> 8); }
static void dmadj_peak(int d) {
    wv_db_step(&dm.mod_peak_db[dm.mod_slot],
               &DRUM_MOD_CUR()->env.peak_level, d);
}

/* ------------------------------------------------------------------ */
/* Per-row functions (LFO subpage)                                     */
/* ------------------------------------------------------------------ */

static void dlfmt_shape(char *b, int n) {
    int s = dm.params.lfo.cfg.shape;
    if (s < 0 || s >= LFE_LFO_SHAPE_COUNT) s = 0;
    snprintf(b, n, "%s", drum_lfo_shape_names[s]);
}
static void dladj_shape(int d) {
    int v = dm.params.lfo.cfg.shape;
    wv_cycle(&v, d, LFE_LFO_SHAPE_COUNT);
    dm.params.lfo.cfg.shape = (uint8_t)v;
}

static void dlfmt_dest(char *b, int n) {
    int dd = dm.params.lfo.dest;
    if (dd < 0 || dd >= LFE_DRUM_LFO_DEST_COUNT) dd = 0;
    snprintf(b, n, "%s", drum_lfo_dest_names[dd]);
}
static void dladj_dest(int d) {
    int v = dm.params.lfo.dest;
    wv_cycle(&v, d, LFE_DRUM_LFO_DEST_COUNT);
    dm.params.lfo.dest = (uint8_t)v;
}

static void dlfmt_rate(char *b, int n) {
    uint32_t r = dm.params.lfo.cfg.rate_hz_q8;
    unsigned whole = r >> 8;
    unsigned frac  = ((r & 0xFF) * 10) >> 8;
    snprintf(b, n, "%u.%u", whole, frac);
}
static void dladj_rate(int d) {
    int v = (int)dm.params.lfo.cfg.rate_hz_q8;
    /* 26 Q24.8 units ≈ 0.1 Hz per click; big step (d=10) ≈ 1 Hz. */
    wv_int_clamped(&v, d, 26, 0, 32 << 8);
    dm.params.lfo.cfg.rate_hz_q8 = (uint32_t)v;
}

static void dlfmt_depth(char *b, int n) {
    int v = dm.params.lfo.cfg.depth;
    snprintf(b, n, "%4d%%", (v * 100) / LFE_Q15_ONE);
}
static void dladj_depth(int d) {
    int v = dm.params.lfo.cfg.depth;
    wv_int_clamped(&v, d, 328, 0, LFE_Q15_ONE);
    dm.params.lfo.cfg.depth = (uint16_t)v;
}

/* ------------------------------------------------------------------ */
/* Dispatch arrays                                                     */
/* ------------------------------------------------------------------ */

WV_ROW_DISPATCH(drum,     DRUM_PARAMS,     DRUM_PARAM_COUNT);
WV_ROW_DISPATCH(drum_mod, DRUM_MOD_PARAMS, DRUM_MOD_PARAM_COUNT);
WV_ROW_DISPATCH(drum_lfo, DRUM_LFO_PARAMS, DRUM_LFO_PARAM_COUNT);

/* ------------------------------------------------------------------ */
/* Input + draw                                                        */
/* ------------------------------------------------------------------ */

static void drum_input(u32 down, u32 held)
{
    if (down & KEY_X) { drum_generate(); return; }
    switch (dm.subpage) {
    case 0: wv_handle_row_input(&dm.param_row,     DRUM_PARAM_COUNT,     drum_adjs,     down, held); break;
    case 1: wv_handle_row_input(&dm.mod_param_row, DRUM_MOD_PARAM_COUNT, drum_mod_adjs, down, held); break;
    case 2: wv_handle_row_input(&dm.lfo_param_row, DRUM_LFO_PARAM_COUNT, drum_lfo_adjs, down, held); break;
    }
}

static void drum_draw(u8 *top_fb)
{
    int list_height = font_scale_row(26) - 5;
    int vcol = font_scale_col(28);
    switch (dm.subpage) {
    case 0: {
        font_puts(top_fb, 1, 3, "Generator: Drum", PAL_DIM);
        drum_sv.row_height = list_height;
        drum_sv.total      = DRUM_PARAM_COUNT;
        drum_sv.cursor     = dm.param_row;

        int db_max = -LFE_DB_FLOOR_Q8_8;
        wv_fader_info faders[DRUM_PARAM_COUNT] = {
            [DRUM_PARAM_TONE_LEVEL]= { dm.tone_db - LFE_DB_FLOOR_Q8_8,   db_max,      PAL_PLAY   },
            [DRUM_PARAM_NOISE_LEVEL]={ dm.noise_db - LFE_DB_FLOOR_Q8_8,  db_max,      PAL_PLAY   },
            [DRUM_PARAM_FILTER_Q]  = { dm.params.filter_q,             LFE_Q15_ONE, PAL_EFFECT },
            [DRUM_PARAM_DRIVE]     = { dm.params.drive,                LFE_Q15_ONE, PAL_RED    },
            [DRUM_PARAM_MASTER]    = { dm.master_db - LFE_DB_FLOOR_Q8_8, db_max,      PAL_PLAY   },
        };

        wv_draw_rows_ex(top_fb, &drum_sv, 1, vcol,
                        drum_param_labels, drum_fmts, faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  X:generate", PAL_DIM);
        break;
    }
    case 1: {
        font_printf(top_fb, 1, 3, PAL_DIM, "Drum Mod Editor (slot %d/%d)",
                    dm.mod_slot + 1, LFE_DRUM_NUM_MODS);
        drum_mod_sv.row_height = list_height;
        drum_mod_sv.total      = DRUM_MOD_PARAM_COUNT;
        drum_mod_sv.cursor     = dm.mod_param_row;

        lfe_drum_mod *m = DRUM_MOD_CUR();
        int mdb_max = -LFE_DB_FLOOR_Q8_8;
        wv_fader_info mod_faders[DRUM_MOD_PARAM_COUNT] = {
            [DRUM_MOD_PARAM_ATTACK]  = { (int)m->env.attack_ms,  4000,    PAL_ORANGE },
            [DRUM_MOD_PARAM_DECAY]   = { (int)m->env.decay_ms,   4000,    PAL_ORANGE },
            [DRUM_MOD_PARAM_SUSTAIN] = { dm.mod_sustain_db[dm.mod_slot] - LFE_DB_FLOOR_Q8_8, mdb_max, PAL_PLAY },
            [DRUM_MOD_PARAM_PEAK]    = { dm.mod_peak_db[dm.mod_slot] - LFE_DB_FLOOR_Q8_8,    mdb_max, PAL_PLAY },
        };

        wv_draw_rows_ex(top_fb, &drum_mod_sv, 1, vcol,
                        drum_mod_param_labels, drum_mod_fmts, mod_faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  B:back", PAL_DIM);
        break;
    }
    case 2: {
        font_puts(top_fb, 1, 3, "Drum LFO Editor", PAL_DIM);
        drum_lfo_sv.row_height = list_height;
        drum_lfo_sv.total      = DRUM_LFO_PARAM_COUNT;
        drum_lfo_sv.cursor     = dm.lfo_param_row;

        wv_fader_info lfo_faders[DRUM_LFO_PARAM_COUNT] = {
            [DRUM_LFO_PARAM_RATE]  = { (int)dm.params.lfo.cfg.rate_hz_q8, 32 << 8,    PAL_ORANGE },
            [DRUM_LFO_PARAM_DEPTH] = { dm.params.lfo.cfg.depth,           LFE_Q15_ONE, PAL_PARAM  },
        };

        wv_draw_rows_ex(top_fb, &drum_lfo_sv, 1, vcol,
                        drum_lfo_param_labels, drum_lfo_fmts, lfo_faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  B:back", PAL_DIM);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Descriptor                                                          */
/* ------------------------------------------------------------------ */

static void drum_on_open(void)        { drum_load_preset(dm.preset); }
static bool drum_has_subpage(void)    { return dm.subpage != 0; }
static void drum_close_subpage(void)  { dm.subpage = 0; }

const wv_tab wv_tab_drum = {
    .name          = "Drum",
    .on_open       = drum_on_open,
    .input         = drum_input,
    .subpage_open  = drum_has_subpage,
    .close_subpage = drum_close_subpage,
    .draw_params   = drum_draw,
};

#endif /* MAXTRACKER_LFE */
