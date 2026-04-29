/*
 * tab_fm4.c — 4-op FM generator tab for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four operators, a 4×4 phase-modulation matrix, a four-slot carrier
 * mix, and two global LFOs. Main page exposes global controls +
 * per-operator carrier levels; three subpages handle the heavier
 * editing:
 *   - Op subpage (X-macro, 9 rows × 4 ops): freq ratio, detune, level,
 *     ADSR.
 *   - Op-mod-matrix subpage: custom 2D navigation over the 4×4 grid.
 *   - LFO subpage (6 rows × 2 LFOs): shape, dest, target, rate, depth.
 *
 * See project_fm4op_plan.md for the design rationale — the one-sample
 * delay on every mod-matrix edge is what makes the topology
 * well-defined regardless of matrix sparsity.
 */

#ifdef MAXTRACKER_LFE

#include "tab_fm4.h"

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

#define FM4_PARAMS(X) \
    X(PRESET,       "Preset",         ffmt_preset,    fadj_preset)    \
    X(PITCH_HZ,     "Pitch (Hz)",     ffmt_pitch,     fadj_pitch)     \
    X(LENGTH_MS,    "Length (ms)",    ffmt_length,    fadj_length)    \
    X(NOTE_OFF_MS,  "NoteOff (ms)",   ffmt_note_off,  fadj_note_off)  \
    X(CARRIER1,     "Carrier 1",      ffmt_car1,      fadj_car1)      \
    X(CARRIER2,     "Carrier 2",      ffmt_car2,      fadj_car2)      \
    X(CARRIER3,     "Carrier 3",      ffmt_car3,      fadj_car3)      \
    X(CARRIER4,     "Carrier 4",      ffmt_car4,      fadj_car4)      \
    X(EDIT_OP,      "Op Editor >>",   ffmt_edit_op,   fadj_edit_op)   \
    X(EDIT_MATRIX,  "Op Matrix >>",   ffmt_edit_mtx,  fadj_edit_mtx)  \
    X(EDIT_LFO,     "LFOs      >>",   ffmt_edit_lfo,  fadj_edit_lfo)

typedef enum {
#define X(name, label, fmt, adj) FM4_PARAM_##name,
    FM4_PARAMS(X)
#undef X
    FM4_PARAM_COUNT,
} Fm4Param;

static const char *fm4_param_labels[FM4_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    FM4_PARAMS(X)
#undef X
};

#define FM4_OP_PARAMS(X) \
    X(OP,       "Op",            fofmt_slot,    foadj_slot)    \
    X(RATIO,    "Freq ratio",    fofmt_ratio,   foadj_ratio)   \
    X(DETUNE,   "Detune (cts)",  fofmt_detune,  foadj_detune)  \
    X(LEVEL,    "Level",         fofmt_level,   foadj_level)   \
    X(ATTACK,   "Attack (ms)",   fofmt_attack,  foadj_attack)  \
    X(DECAY,    "Decay (ms)",    fofmt_decay,   foadj_decay)   \
    X(SUSTAIN,  "Sustain",       fofmt_sustain, foadj_sustain) \
    X(RELEASE,  "Release (ms)",  fofmt_release, foadj_release) \
    X(PEAK,     "Peak",          fofmt_peak,    foadj_peak)

typedef enum {
#define X(name, label, fmt, adj) FM4_OP_PARAM_##name,
    FM4_OP_PARAMS(X)
#undef X
    FM4_OP_PARAM_COUNT,
} Fm4OpParam;

static const char *fm4_op_param_labels[FM4_OP_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    FM4_OP_PARAMS(X)
#undef X
};

#define FM4_LFO_PARAMS(X) \
    X(SLOT,   "LFO",     flfmt_slot,   fladj_slot)   \
    X(SHAPE,  "Shape",   flfmt_shape,  fladj_shape)  \
    X(DEST,   "Dest",    flfmt_dest,   fladj_dest)   \
    X(TARGET, "Target",  flfmt_target, fladj_target) \
    X(RATE,   "Rate Hz", flfmt_rate,   fladj_rate)   \
    X(DEPTH,  "Depth",   flfmt_depth,  fladj_depth)

typedef enum {
#define X(name, label, fmt, adj) FM4_LFO_PARAM_##name,
    FM4_LFO_PARAMS(X)
#undef X
    FM4_LFO_PARAM_COUNT,
} Fm4LfoParam;

static const char *fm4_lfo_param_labels[FM4_LFO_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    FM4_LFO_PARAMS(X)
#undef X
};

static const char *fm4_lfo_shape_names[LFE_LFO_SHAPE_COUNT] = {
    "SINE", "TRI", "SQ", "SAW+", "SAW-",
};

static const char *fm4_lfo_dest_names[LFE_FM4_LFO_DEST_COUNT] = {
    "OFF", "OP_LVL", "MATRIX", "CARRMX", "PITCH",
};

#define FM4_PRESET_COUNT LFE_FM4_PRESET_COUNT
static const char *fm4_preset_names[FM4_PRESET_COUNT] = {
    "EP", "BELL", "BASS", "BRASS", "PLUCK", "WOBBLE", "GROWL",
};

/* File-scoped state (Step 5 encapsulation). `params` is the source of
 * truth; per-op level/sustain/peak shadow the Q15 gain fields in dB
 * Q8.8 so repeated edits don't drift. `matrix_src/dst` is the cursor
 * in the 4×4 mod-matrix subpage. */
static struct {
    int param_row;
    int preset;
    int pitch_hz;
    int note_off_ms;
    lfe_fm4_params params;
    int subpage;        /* 0 main, 1 op editor, 2 matrix editor, 3 LFO editor */
    int op_slot;
    int op_param_row;
    int matrix_src;
    int matrix_dst;
    int lfo_slot;
    int lfo_param_row;
    int op_level_db[LFE_FM4_NUM_OPS];
    int op_sustain_db[LFE_FM4_NUM_OPS];
    int op_peak_db[LFE_FM4_NUM_OPS];
} fm = {
    .pitch_hz  = 220,
};

static ScrollView fm4_sv     = { .row_y = 5, .margin = 2 };
static ScrollView fm4_op_sv  = { .row_y = 5, .margin = 2 };
static ScrollView fm4_lfo_sv = { .row_y = 5, .margin = 2 };

/* ------------------------------------------------------------------ */
/* Preset load + generate                                              */
/* ------------------------------------------------------------------ */

static void fm4_refresh_db_shadows(void)
{
    for (int i = 0; i < LFE_FM4_NUM_OPS; i++) {
        fm.op_level_db[i]   = lfe_q15_to_db((int16_t)fm.params.ops[i].level);
        fm.op_sustain_db[i] = lfe_q15_to_db((int16_t)fm.params.ops[i].env.sustain_level);
        fm.op_peak_db[i]    = lfe_q15_to_db((int16_t)fm.params.ops[i].env.peak_level);
    }
}

static void fm4_load_preset(int preset_idx)
{
    lfe_status rc = lfe_fm4_fill_preset(&fm.params, (lfe_fm4_preset)preset_idx);
    if (rc != LFE_OK) {
        dbg_set_last_error("fm4: fill_preset rc=%d", (int)rc);
        return;
    }
    fm4_refresh_db_shadows();
    fm.pitch_hz = (int)(fm.params.base_hz_q8 >> 8);
    if (fm.pitch_hz < 20) fm.pitch_hz = 220;
    fm.note_off_ms = (int)(fm.params.note_off_sample * 1000u / 32000u);
}

static void fm4_generate(void)
{
    dbg_log("fm4_generate preset=%d hz=%d dur=%d",
            fm.preset, fm.pitch_hz, wv.length_ms);

    const uint32_t rate   = 32000u;
    const uint32_t length = (uint32_t)wv.length_ms * rate / 1000u;
    if (length == 0) {
        snprintf(wv.status, sizeof(wv.status), "Duration too short");
        wv.status_timer = 180;
        return;
    }

    lfe_fm4_params p = fm.params;
    p.base_hz_q8 = (uint32_t)fm.pitch_hz << 8;
    if (fm.note_off_ms <= 0) {
        p.note_off_sample = 0;
    } else {
        uint32_t no = (uint32_t)fm.note_off_ms * rate / 1000u;
        if (no >= length) no = length - 1;
        p.note_off_sample = no;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)length * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("fm4: malloc %lu failed", (unsigned long)length);
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer outbuf = { .data = pcm, .length = length, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_fm4(&outbuf, &p);
    if (rc < 0) {
        free(pcm);
        snprintf(wv.status, sizeof(wv.status), "FM4 gen failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    wv_write_draft_16(pcm, length, rate, 0, 0, "FM4");
    snprintf(wv.status, sizeof(wv.status), "%s %d Hz %d ms",
             fm4_preset_names[fm.preset], fm.pitch_hz, wv.length_ms);
    wv.status_timer = 180;
}

/* ------------------------------------------------------------------ */
/* Per-row functions (main page)                                       */
/* ------------------------------------------------------------------ */

static void ffmt_preset(char *b, int n) { snprintf(b, n, "%s", fm4_preset_names[fm.preset]); }
static void fadj_preset(int d) {
    wv_cycle(&fm.preset, d, FM4_PRESET_COUNT);
    fm4_load_preset(fm.preset);
}
static void ffmt_pitch(char *b, int n)   { snprintf(b, n, "%4d", fm.pitch_hz); }
static void fadj_pitch(int d)            { wv_int_clamped(&fm.pitch_hz, d, 1, 20, 4000); }
static void ffmt_length(char *b, int n)  { wv_format_length_with_size(b, n, wv.length_ms, 32000u); }
static void fadj_length(int d)           { wv_int_clamped(&wv.length_ms, d, 50, 100, 4000); }
static void ffmt_note_off(char *b, int n){
    if (fm.note_off_ms == 0) snprintf(b, n, "off");
    else                          snprintf(b, n, "%4d", fm.note_off_ms);
}
static void fadj_note_off(int d)         { wv_int_clamped(&fm.note_off_ms, d, 50, 0, 4000); }

/* Carrier mix: signed Q15, displayed as signed percent. */
static void fm4_carrier_fmt(char *b, int n, int idx) {
    int v = fm.params.carrier_mix[idx];
    snprintf(b, n, "%+4d%%", (v * 100) / LFE_Q15_ONE);
}
static void fm4_carrier_adj(int idx, int d) {
    int v = fm.params.carrier_mix[idx];
    wv_int_clamped(&v, d, 256, -LFE_Q15_ONE, LFE_Q15_ONE);
    fm.params.carrier_mix[idx] = (int16_t)v;
}
static void ffmt_car1(char *b, int n) { fm4_carrier_fmt(b, n, 0); }
static void ffmt_car2(char *b, int n) { fm4_carrier_fmt(b, n, 1); }
static void ffmt_car3(char *b, int n) { fm4_carrier_fmt(b, n, 2); }
static void ffmt_car4(char *b, int n) { fm4_carrier_fmt(b, n, 3); }
static void fadj_car1(int d) { fm4_carrier_adj(0, d); }
static void fadj_car2(int d) { fm4_carrier_adj(1, d); }
static void fadj_car3(int d) { fm4_carrier_adj(2, d); }
static void fadj_car4(int d) { fm4_carrier_adj(3, d); }

static void ffmt_edit_op(char *b, int n)  { (void)b; (void)n; b[0] = '\0'; }
static void fadj_edit_op(int d)           { if (d >= 0) fm.subpage = 1; }
static void ffmt_edit_mtx(char *b, int n) { (void)b; (void)n; b[0] = '\0'; }
static void fadj_edit_mtx(int d)          { if (d >= 0) fm.subpage = 2; }
static void ffmt_edit_lfo(char *b, int n) { (void)b; (void)n; b[0] = '\0'; }
static void fadj_edit_lfo(int d)          { if (d >= 0) fm.subpage = 3; }

/* ------------------------------------------------------------------ */
/* Per-row functions (op subpage)                                      */
/* ------------------------------------------------------------------ */

#define FM4_OP_CUR()  (&fm.params.ops[fm.op_slot])

static void fofmt_slot(char *b, int n)  { snprintf(b, n, "%d/%d", fm.op_slot + 1, LFE_FM4_NUM_OPS); }
static void foadj_slot(int d)           { wv_cycle(&fm.op_slot, d, LFE_FM4_NUM_OPS); }

/* Freq ratio is Q8.8. Display as "x.xx" with two decimals. Step size
 * 16 Q8.8 units (= 0.0625) per dial click, so A+L/R = 0.0625 and
 * A+UP/DN = 0.625. Clamp 0.0625..16.0 — 1/16 covers sub-bass octaves
 * down, 16x covers typical upper-modulator ratios. */
static void fofmt_ratio(char *b, int n) {
    uint32_t q = FM4_OP_CUR()->freq_ratio_q8;
    unsigned whole = q >> 8;
    unsigned frac  = ((q & 0xFF) * 100) >> 8;
    snprintf(b, n, "%u.%02u", whole, frac);
}
static void foadj_ratio(int d) {
    int v = (int)FM4_OP_CUR()->freq_ratio_q8;
    wv_int_clamped(&v, d, 16, 16, 0x1000);
    FM4_OP_CUR()->freq_ratio_q8 = (uint32_t)v;
}
static void fofmt_detune(char *b, int n) { snprintf(b, n, "%+4d", FM4_OP_CUR()->detune_cents); }
static void foadj_detune(int d) {
    int v = FM4_OP_CUR()->detune_cents;
    wv_int_clamped(&v, d, 1, -100, 100);
    FM4_OP_CUR()->detune_cents = (int16_t)v;
}
static void fofmt_level(char *b, int n) { snprintf(b, n, "%+4d dB", fm.op_level_db[fm.op_slot] >> 8); }
static void foadj_level(int d) {
    wv_db_step(&fm.op_level_db[fm.op_slot], &FM4_OP_CUR()->level, d);
}
static void fofmt_attack(char *b, int n) { snprintf(b, n, "%5lu", (unsigned long)FM4_OP_CUR()->env.attack_ms); }
static void foadj_attack(int d) {
    int v = (int)FM4_OP_CUR()->env.attack_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    FM4_OP_CUR()->env.attack_ms = (uint32_t)v;
}
static void fofmt_decay(char *b, int n)  { snprintf(b, n, "%5lu", (unsigned long)FM4_OP_CUR()->env.decay_ms); }
static void foadj_decay(int d) {
    int v = (int)FM4_OP_CUR()->env.decay_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    FM4_OP_CUR()->env.decay_ms = (uint32_t)v;
}
static void fofmt_sustain(char *b, int n) { snprintf(b, n, "%+4d dB", fm.op_sustain_db[fm.op_slot] >> 8); }
static void foadj_sustain(int d) {
    wv_db_step(&fm.op_sustain_db[fm.op_slot], &FM4_OP_CUR()->env.sustain_level, d);
}
static void fofmt_release(char *b, int n) { snprintf(b, n, "%5lu", (unsigned long)FM4_OP_CUR()->env.release_ms); }
static void foadj_release(int d) {
    int v = (int)FM4_OP_CUR()->env.release_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    FM4_OP_CUR()->env.release_ms = (uint32_t)v;
}
static void fofmt_peak(char *b, int n)    { snprintf(b, n, "%+4d dB", fm.op_peak_db[fm.op_slot] >> 8); }
static void foadj_peak(int d) {
    wv_db_step(&fm.op_peak_db[fm.op_slot], &FM4_OP_CUR()->env.peak_level, d);
}

/* ------------------------------------------------------------------ */
/* Per-row functions (LFO subpage)                                     */
/* ------------------------------------------------------------------ */

#define FM4_LFO_CUR()  (&fm.params.lfos[fm.lfo_slot])

static void flfmt_slot(char *b, int n) {
    snprintf(b, n, "%d/%d", fm.lfo_slot + 1, LFE_FM4_NUM_LFOS);
}
static void fladj_slot(int d) { wv_cycle(&fm.lfo_slot, d, LFE_FM4_NUM_LFOS); }

static void flfmt_shape(char *b, int n) {
    int s = FM4_LFO_CUR()->cfg.shape;
    if (s < 0 || s >= LFE_LFO_SHAPE_COUNT) s = 0;
    snprintf(b, n, "%s", fm4_lfo_shape_names[s]);
}
static void fladj_shape(int d) {
    int v = FM4_LFO_CUR()->cfg.shape;
    wv_cycle(&v, d, LFE_LFO_SHAPE_COUNT);
    FM4_LFO_CUR()->cfg.shape = (uint8_t)v;
}

static void flfmt_dest(char *b, int n) {
    int dd = FM4_LFO_CUR()->dest;
    if (dd < 0 || dd >= LFE_FM4_LFO_DEST_COUNT) dd = 0;
    snprintf(b, n, "%s", fm4_lfo_dest_names[dd]);
}
static void fladj_dest(int d) {
    int v = FM4_LFO_CUR()->dest;
    wv_cycle(&v, d, LFE_FM4_LFO_DEST_COUNT);
    FM4_LFO_CUR()->dest = (uint8_t)v;
    /* Target semantics change with dest — clamp target so it stays
     * valid for the new destination's interpretation. */
    uint8_t t = FM4_LFO_CUR()->target;
    switch (v) {
    case LFE_FM4_LFO_DEST_MATRIX_CELL:
        if (t > 15) FM4_LFO_CUR()->target = 0;
        break;
    case LFE_FM4_LFO_DEST_OP_LEVEL:
    case LFE_FM4_LFO_DEST_CARRIER_MIX:
        if (t > 3) FM4_LFO_CUR()->target = 0;
        break;
    default:
        FM4_LFO_CUR()->target = 0;
        break;
    }
}

/* Target display depends on destination. Matrix cells show "S1→D1"
 * format so the 4×4 grid is readable without cross-referencing the
 * matrix subpage. */
static void flfmt_target(char *b, int n) {
    const lfe_fm4_lfo *l = FM4_LFO_CUR();
    switch (l->dest) {
    case LFE_FM4_LFO_DEST_OP_LEVEL:
    case LFE_FM4_LFO_DEST_CARRIER_MIX:
        snprintf(b, n, "op %d", (l->target & 3) + 1);
        break;
    case LFE_FM4_LFO_DEST_MATRIX_CELL: {
        int src = (l->target >> 2) & 3;
        int dst = l->target & 3;
        snprintf(b, n, "S%d>D%d", src + 1, dst + 1);
        break;
    }
    default:
        snprintf(b, n, "-");
        break;
    }
}
static void fladj_target(int d) {
    lfe_fm4_lfo *l = FM4_LFO_CUR();
    int limit;
    switch (l->dest) {
    case LFE_FM4_LFO_DEST_MATRIX_CELL:                        limit = 16; break;
    case LFE_FM4_LFO_DEST_OP_LEVEL:
    case LFE_FM4_LFO_DEST_CARRIER_MIX:                        limit = 4;  break;
    default:                                                   return;
    }
    int v = l->target;
    wv_cycle(&v, d, limit);
    l->target = (uint8_t)v;
}

/* Rate is Q24.8 Hz. Step by 0.1 Hz per small click, 1 Hz per big. Up
 * to 32 Hz is useful; beyond that it's into audio-rate territory. */
static void flfmt_rate(char *b, int n) {
    uint32_t r = FM4_LFO_CUR()->cfg.rate_hz_q8;
    unsigned whole = r >> 8;
    unsigned frac  = ((r & 0xFF) * 10) >> 8;
    snprintf(b, n, "%u.%u", whole, frac);
}
static void fladj_rate(int d) {
    int v = (int)FM4_LFO_CUR()->cfg.rate_hz_q8;
    /* 26 Q24.8 units ≈ 0.1 Hz per click. Big step (A+UP/DN sends d=10)
     * then gives ~1 Hz. Clamp 0..32 Hz. */
    wv_int_clamped(&v, d, 26, 0, 32 << 8);
    FM4_LFO_CUR()->cfg.rate_hz_q8 = (uint32_t)v;
}

static void flfmt_depth(char *b, int n) {
    int v = FM4_LFO_CUR()->cfg.depth;
    snprintf(b, n, "%4d%%", (v * 100) / LFE_Q15_ONE);
}
static void fladj_depth(int d) {
    int v = FM4_LFO_CUR()->cfg.depth;
    wv_int_clamped(&v, d, 328, 0, LFE_Q15_ONE);   /* ~1% per click */
    FM4_LFO_CUR()->cfg.depth = (uint16_t)v;
}

/* ------------------------------------------------------------------ */
/* Dispatch arrays                                                     */
/* ------------------------------------------------------------------ */

WV_ROW_DISPATCH(fm4,     FM4_PARAMS,     FM4_PARAM_COUNT);
WV_ROW_DISPATCH(fm4_op,  FM4_OP_PARAMS,  FM4_OP_PARAM_COUNT);
WV_ROW_DISPATCH(fm4_lfo, FM4_LFO_PARAMS, FM4_LFO_PARAM_COUNT);

/* ------------------------------------------------------------------ */
/* Input + draw                                                        */
/* ------------------------------------------------------------------ */

/* Matrix editor: plain arrows move the cursor (UP/DN = src, L/R = dst);
 * A+L/R / A+UP/DN adjusts the value under the cursor. */
static void fm4_input_matrix(u32 down, u32 held)
{
    if (!(held & KEY_A)) {
        if (down & KEY_UP)    wv_cycle(&fm.matrix_src, -1, LFE_FM4_NUM_OPS);
        if (down & KEY_DOWN)  wv_cycle(&fm.matrix_src, +1, LFE_FM4_NUM_OPS);
        if (down & KEY_LEFT)  wv_cycle(&fm.matrix_dst, -1, LFE_FM4_NUM_OPS);
        if (down & KEY_RIGHT) wv_cycle(&fm.matrix_dst, +1, LFE_FM4_NUM_OPS);
    } else {
        u32 rep = keysDownRepeat();
        int step = 0;
        if (rep & KEY_LEFT)  step = -1;
        if (rep & KEY_RIGHT) step = +1;
        if (rep & KEY_DOWN)  step = -10;
        if (rep & KEY_UP)    step = +10;
        if (step) {
            int v = fm.params.mod_matrix[fm.matrix_src][fm.matrix_dst];
            wv_int_clamped(&v, step, 256, -LFE_Q15_ONE, LFE_Q15_ONE);
            fm.params.mod_matrix[fm.matrix_src][fm.matrix_dst] = (int16_t)v;
        }
    }
}

static void fm4_input(u32 down, u32 held)
{
    if (down & KEY_X) { fm4_generate(); return; }
    switch (fm.subpage) {
    case 0: wv_handle_row_input(&fm.param_row,     FM4_PARAM_COUNT,     fm4_adjs,     down, held); break;
    case 1: wv_handle_row_input(&fm.op_param_row,  FM4_OP_PARAM_COUNT,  fm4_op_adjs,  down, held); break;
    case 2: fm4_input_matrix(down, held); break;
    case 3: wv_handle_row_input(&fm.lfo_param_row, FM4_LFO_PARAM_COUNT, fm4_lfo_adjs, down, held); break;
    }
}

static void fm4_draw_matrix(u8 *top_fb)
{
    font_puts(top_fb, 1, 3, "OP MOD MATRIX (src -> dst, Q15 %)", PAL_DIM);
    font_puts(top_fb, font_scale_col(7),  5, "D1", PAL_DIM);
    font_puts(top_fb, font_scale_col(13), 5, "D2", PAL_DIM);
    font_puts(top_fb, font_scale_col(19), 5, "D3", PAL_DIM);
    font_puts(top_fb, font_scale_col(25), 5, "D4", PAL_DIM);
    for (int src = 0; src < LFE_FM4_NUM_OPS; src++) {
        int r = 7 + src * 2;
        font_printf(top_fb, 1, r, PAL_DIM, "S%d", src + 1);
        for (int dst = 0; dst < LFE_FM4_NUM_OPS; dst++) {
            int v = fm.params.mod_matrix[src][dst];
            int pct = (v * 100) / LFE_Q15_ONE;
            u8 color = (src == fm.matrix_src && dst == fm.matrix_dst)
                           ? PAL_WHITE : PAL_GRAY;
            font_printf(top_fb, font_scale_col(5 + dst * 6), r, color,
                        "%+4d", pct);
        }
    }
    font_puts(top_fb, 0, font_scale_row(24),
              "Arrows:cell  A+L/R:-+1  A+UP/DN:+-10", PAL_DIM);
    font_puts(top_fb, 0, font_scale_row(26),
              "B:back", PAL_DIM);
}

static void fm4_draw(u8 *top_fb)
{
    int list_height = font_scale_row(26) - 5;
    int vcol = font_scale_col(28);
    switch (fm.subpage) {
    case 0: {
        font_puts(top_fb, 1, 3, "Generator: 4-op FM Synth", PAL_DIM);
        fm4_sv.row_height = list_height;
        fm4_sv.total      = FM4_PARAM_COUNT;
        fm4_sv.cursor     = fm.param_row;

        wv_fader_info faders[FM4_PARAM_COUNT] = {
            [FM4_PARAM_NOTE_OFF_MS]= { fm.note_off_ms,  4000,        PAL_ORANGE },
        };

        wv_draw_rows_ex(top_fb, &fm4_sv, 1, vcol,
                        fm4_param_labels, fm4_fmts, faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  X:generate", PAL_DIM);
        break;
    }
    case 1: {
        font_printf(top_fb, 1, 3, PAL_DIM, "FM4 Op Editor (op %d/%d)",
                    fm.op_slot + 1, LFE_FM4_NUM_OPS);
        fm4_op_sv.row_height = list_height;
        fm4_op_sv.total      = FM4_OP_PARAM_COUNT;
        fm4_op_sv.cursor     = fm.op_param_row;

        int db_max = -LFE_DB_FLOOR_Q8_8;
        wv_fader_info op_faders[FM4_OP_PARAM_COUNT] = {
            [FM4_OP_PARAM_LEVEL]   = { fm.op_level_db[fm.op_slot] - LFE_DB_FLOOR_Q8_8,   db_max, PAL_PLAY   },
            [FM4_OP_PARAM_ATTACK]  = { (int)FM4_OP_CUR()->env.attack_ms,  8000,       PAL_ORANGE },
            [FM4_OP_PARAM_DECAY]   = { (int)FM4_OP_CUR()->env.decay_ms,   8000,       PAL_ORANGE },
            [FM4_OP_PARAM_SUSTAIN] = { fm.op_sustain_db[fm.op_slot] - LFE_DB_FLOOR_Q8_8, db_max, PAL_PLAY   },
            [FM4_OP_PARAM_RELEASE] = { (int)FM4_OP_CUR()->env.release_ms, 8000,       PAL_ORANGE },
            [FM4_OP_PARAM_PEAK]    = { fm.op_peak_db[fm.op_slot] - LFE_DB_FLOOR_Q8_8,    db_max, PAL_PLAY   },
        };

        wv_draw_rows_ex(top_fb, &fm4_op_sv, 1, vcol,
                        fm4_op_param_labels, fm4_op_fmts, op_faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  B:back", PAL_DIM);
        break;
    }
    case 2:
        fm4_draw_matrix(top_fb);
        break;
    case 3: {
        font_printf(top_fb, 1, 3, PAL_DIM, "FM4 LFO Editor (LFO %d/%d)",
                    fm.lfo_slot + 1, LFE_FM4_NUM_LFOS);
        fm4_lfo_sv.row_height = list_height;
        fm4_lfo_sv.total      = FM4_LFO_PARAM_COUNT;
        fm4_lfo_sv.cursor     = fm.lfo_param_row;

        wv_fader_info lfo_faders[FM4_LFO_PARAM_COUNT] = {
            [FM4_LFO_PARAM_RATE]  = { (int)FM4_LFO_CUR()->cfg.rate_hz_q8, 32 << 8,    PAL_ORANGE },
            [FM4_LFO_PARAM_DEPTH] = { FM4_LFO_CUR()->cfg.depth,           LFE_Q15_ONE, PAL_PARAM  },
        };

        wv_draw_rows_ex(top_fb, &fm4_lfo_sv, 1, vcol,
                        fm4_lfo_param_labels, fm4_lfo_fmts, lfo_faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  B:back", PAL_DIM);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Descriptor                                                          */
/* ------------------------------------------------------------------ */

static void fm4_on_open(void)        { fm4_load_preset(fm.preset); }
static bool fm4_has_subpage(void)    { return fm.subpage != 0; }
static void fm4_close_subpage(void)  { fm.subpage = 0; }

const wv_tab wv_tab_fm4 = {
    .name          = "FM4",
    .on_open       = fm4_on_open,
    .input         = fm4_input,
    .subpage_open  = fm4_has_subpage,
    .close_subpage = fm4_close_subpage,
    .draw_params   = fm4_draw,
};

#endif /* MAXTRACKER_LFE */
