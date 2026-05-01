/*
 * tab_synth.c — Subtractive + Combine synth tab for the Waveform Editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two oscillators (saw/square/triangle/sine with detune, level, PWM)
 * summed or combined via one of five modes (MIX / HARD_SYNC / FM /
 * RING_MOD / CALVARIO) plus LFSR noise, all fed into an SVF with four
 * filter shapes, gated by a master envelope. Four modulation slots
 * reach any meaningful parameter including the combine params.
 *
 * Main page is SYNTH_PARAMS (21 rows); mod-slot subpage is
 * SYNTH_MOD_PARAMS (8 rows × 4 slots). See the project memo
 * project_synth_osc_combine_plan.md for the design rationale behind
 * the combine modes.
 */

#ifdef MAXTRACKER_LFE

#include "tab_synth.h"

#include "wv_internal.h"
#include "wv_common.h"
#include "lfe_row_ui.h"

#include "font.h"
#include "screen.h"
#include "debug_view.h"

#include "lfe.h"
#include "lfe_dbmath.h"

#include <nds.h>

#include "keybind.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Row lists                                                           */
/* ------------------------------------------------------------------ */

#define SYNTH_PARAMS(X) \
    X(PRESET,       "Preset",        sfmt_preset,      sadj_preset)      \
    X(PITCH_HZ,     "Pitch (Hz)",    sfmt_pitch,       sadj_pitch)       \
    X(LENGTH_MS,    "Length (ms)",   sfmt_length,      sadj_length)      \
    X(NOTE_OFF_MS,  "NoteOff (ms)",  sfmt_note_off,    sadj_note_off)    \
    X(OSC1_WAVE,    "Osc1 Wave",     sfmt_osc1_wave,   sadj_osc1_wave)   \
    X(OSC1_DETUNE,  "Osc1 Detune",   sfmt_osc1_detune, sadj_osc1_detune) \
    X(OSC1_LEVEL,   "Osc1 Level",    sfmt_osc1_level,  sadj_osc1_level)  \
    X(OSC1_PW,      "Osc1 PW",       sfmt_osc1_pw,     sadj_osc1_pw)     \
    X(OSC2_WAVE,    "Osc2 Wave",     sfmt_osc2_wave,   sadj_osc2_wave)   \
    X(OSC2_DETUNE,  "Osc2 Detune",   sfmt_osc2_detune, sadj_osc2_detune) \
    X(OSC2_LEVEL,   "Osc2 Level",    sfmt_osc2_level,  sadj_osc2_level)  \
    X(OSC2_PW,      "Osc2 PW",       sfmt_osc2_pw,     sadj_osc2_pw)     \
    X(NOISE_LEVEL,  "Noise Level",   sfmt_noise,       sadj_noise)       \
    X(FILTER_MODE,  "Filter Mode",   sfmt_fmode,       sadj_fmode)       \
    X(FILTER_CUT,   "Filter Cut",    sfmt_fcut,        sadj_fcut)        \
    X(FILTER_Q,     "Filter Q",      sfmt_fq,          sadj_fq)          \
    X(MASTER,       "Master",        sfmt_master,      sadj_master)      \
    X(COMBINE,      "Combine",       sfmt_combine,     sadj_combine)     \
    X(COMBINE_P1,   "Param 1",       sfmt_cp1,         sadj_cp1)         \
    X(COMBINE_P2,   "Param 2",       sfmt_cp2,         sadj_cp2)         \
    X(EDIT_MODS,    "Mod Slots >>",  sfmt_edit_mods,   sadj_edit_mods)

typedef enum {
#define X(name, label, fmt, adj) SYNTH_PARAM_##name,
    SYNTH_PARAMS(X)
#undef X
    SYNTH_PARAM_COUNT,
} SynthParam;

static const char *synth_param_labels[SYNTH_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    SYNTH_PARAMS(X)
#undef X
};

/* Mod-slot subpage. The "Slot" row picks which of the 4 mod slots the
 * rest of the rows operate on; everything else is per-slot fields of
 * `lfe_synth_mod`. */
#define SYNTH_MOD_PARAMS(X) \
    X(SLOT,     "Slot",         smfmt_slot,    smadj_slot)    \
    X(TARGET,   "Target",       smfmt_target,  smadj_target)  \
    X(DEPTH,    "Depth",        smfmt_depth,   smadj_depth)   \
    X(ATTACK,   "Attack (ms)",  smfmt_attack,  smadj_attack)  \
    X(DECAY,    "Decay (ms)",   smfmt_decay,   smadj_decay)   \
    X(SUSTAIN,  "Sustain",      smfmt_sustain, smadj_sustain) \
    X(RELEASE,  "Release (ms)", smfmt_release, smadj_release) \
    X(PEAK,     "Peak",         smfmt_peak,    smadj_peak)

typedef enum {
#define X(name, label, fmt, adj) SYNTH_MOD_PARAM_##name,
    SYNTH_MOD_PARAMS(X)
#undef X
    SYNTH_MOD_PARAM_COUNT,
} SynthModParam;

static const char *synth_mod_param_labels[SYNTH_MOD_PARAM_COUNT] = {
#define X(name, label, fmt, adj) label,
    SYNTH_MOD_PARAMS(X)
#undef X
};

static const char *synth_wave_names[4]   = { "SAW", "SQR", "TRI", "SIN" };
static const char *synth_filter_names[4] = { "LP", "HP", "BP", "NOTCH" };

/* Mod target names — order locked to lfe_synth_mod_target in lfe.h. */
#define SYNTH_MOD_TARGET_COUNT 10
static const char *synth_mod_target_names[SYNTH_MOD_TARGET_COUNT] = {
    "NONE", "AMP", "PITCH", "FILTER",
    "OSC1.LV", "OSC2.LV", "NOISE.LV", "PULSE.W",
    "CMB.P1", "CMB.P2",
};

#define SYNTH_PRESET_COUNT 4
static const char *synth_preset_names[SYNTH_PRESET_COUNT] = {
    "LEAD", "PAD", "PLUCK", "BASS",
};

#define SYNTH_COMBINE_COUNT 5
static const char *synth_combine_names[SYNTH_COMBINE_COUNT] = {
    "MIX", "HARD SYNC", "FM", "RING MOD", "CALVARIO",
};

/* File-scoped state (Step 5 encapsulation). `params` is the source of
 * truth for every dialable field; the dB shadows (Q8.8) avoid LUT
 * round-trip drift on repeated edits. `pitch_hz` and `note_off_ms`
 * are UI-only overlays applied at generate time. */
static struct {
    int param_row;
    int preset;
    int pitch_hz;
    int note_off_ms;
    int osc1_db;
    int osc2_db;
    int noise_db;
    int master_db;
    lfe_synth_params params;
    int subpage;
    int mod_slot;
    int mod_param_row;
    int mod_sustain_db[LFE_SYNTH_NUM_MODS];
    int mod_peak_db[LFE_SYNTH_NUM_MODS];
} sy = {
    .pitch_hz    = 220,    /* A3 */
    .note_off_ms = 700,
};

/* Viewports for the main list and the mod-slot subpage. Viewport height
 * is recomputed per frame so BIG mode (24 grid rows) doesn't overflow
 * the help text on row 26. */
static ScrollView synth_sv     = { .row_y = 5, .margin = 2 };
static ScrollView synth_mod_sv = { .row_y = 5, .margin = 2 };

/* ------------------------------------------------------------------ */
/* Preset load + generate                                              */
/* ------------------------------------------------------------------ */

static void synth_refresh_db_shadows(void)
{
    sy.osc1_db   = lfe_q15_to_db((int16_t)sy.params.osc1.level);
    sy.osc2_db   = lfe_q15_to_db((int16_t)sy.params.osc2.level);
    sy.noise_db  = lfe_q15_to_db((int16_t)sy.params.noise_level);
    sy.master_db = lfe_q15_to_db((int16_t)sy.params.master_level);
    for (int i = 0; i < LFE_SYNTH_NUM_MODS; i++) {
        sy.mod_sustain_db[i] =
            lfe_q15_to_db((int16_t)sy.params.mods[i].env.sustain_level);
        sy.mod_peak_db[i] =
            lfe_q15_to_db((int16_t)sy.params.mods[i].env.peak_level);
    }
}

static void synth_load_preset(int preset_idx)
{
    lfe_status rc = lfe_synth_fill_preset(&sy.params,
                                          (lfe_synth_preset)preset_idx);
    if (rc != LFE_OK) {
        dbg_set_last_error("synth: fill_preset rc=%d", (int)rc);
        return;
    }
    synth_refresh_db_shadows();
    /* Seed pitch from the preset; length + note_off stay UI-driven. */
    sy.pitch_hz = (int)(sy.params.base_hz_q8 >> 8);
    if (sy.pitch_hz < 20) sy.pitch_hz = 220;
}

static void synth_generate(void)
{
    dbg_log("synth_generate preset=%d pitch=%d len=%d",
            sy.preset, sy.pitch_hz, wv.length_ms);

    const uint32_t rate   = 32000u;
    const uint32_t length = (uint32_t)wv.length_ms * rate / 1000u;
    if (length == 0) {
        dbg_set_last_error("synth: zero length");
        snprintf(wv.status, sizeof(wv.status), "Duration too short");
        wv.status_timer = 180;
        return;
    }

    lfe_synth_params params = sy.params;
    params.base_hz_q8 = (uint32_t)sy.pitch_hz << 8;

    if (sy.note_off_ms <= 0) {
        params.note_off_sample = 0;
    } else {
        uint32_t no = (uint32_t)sy.note_off_ms * rate / 1000u;
        if (no >= length) no = length - 1;
        params.note_off_sample = no;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)length * sizeof(int16_t));
    if (!pcm) {
        dbg_set_last_error("synth: malloc failed (%lu bytes)",
                           (unsigned long)(length * sizeof(int16_t)));
        snprintf(wv.status, sizeof(wv.status), "Out of memory");
        wv.status_timer = 180;
        return;
    }

    lfe_buffer out = { .data = pcm, .length = length, .rate = LFE_RATE_32000 };
    lfe_status rc = lfe_gen_synth(&out, &params);
    if (rc < 0) {
        free(pcm);
        dbg_set_last_error("synth: gen rc=%d", (int)rc);
        snprintf(wv.status, sizeof(wv.status),
                 "Generate failed (%d)", (int)rc);
        wv.status_timer = 180;
        return;
    }

    wv_write_draft_16(pcm, length, rate, 0, 0, "Synth");

    const char *name = synth_preset_names[sy.preset];
    if (rc == LFE_WARN_CLIPPED) {
        snprintf(wv.status, sizeof(wv.status),
                 "%s %d Hz %d ms (clipped)",
                 name, sy.pitch_hz, wv.length_ms);
    } else {
        snprintf(wv.status, sizeof(wv.status),
                 "%s %d Hz %d ms",
                 name, sy.pitch_hz, wv.length_ms);
    }
    wv.status_timer = 180;
}

/* ------------------------------------------------------------------ */
/* Per-row functions (main page)                                       */
/* ------------------------------------------------------------------ */

static void sfmt_preset(char *b, int n)  { snprintf(b, n, "%s", synth_preset_names[sy.preset]); }
static void sadj_preset(int d)
{
    wv_cycle(&sy.preset, d, SYNTH_PRESET_COUNT);
    synth_load_preset(sy.preset);
}

static void sfmt_pitch(char *b, int n)  { snprintf(b, n, "%4d", sy.pitch_hz); }
static void sadj_pitch(int d)           { wv_int_clamped(&sy.pitch_hz, d, 1, 20, 4000); }

static void sfmt_length(char *b, int n) { wv_format_length_with_size(b, n, wv.length_ms, 32000u); }
static void sadj_length(int d)          { wv_int_clamped(&wv.length_ms, d, 50, 100, 4000); }

static void sfmt_note_off(char *b, int n) {
    if (sy.note_off_ms == 0) snprintf(b, n, "off");
    else                            snprintf(b, n, "%4d", sy.note_off_ms);
}
static void sadj_note_off(int d)        { wv_int_clamped(&sy.note_off_ms, d, 50, 0, 4000); }

static void sfmt_osc1_wave(char *b, int n)   { snprintf(b, n, "%s", synth_wave_names[sy.params.osc1.wave]); }
static void sadj_osc1_wave(int d)            { int v = sy.params.osc1.wave; wv_cycle(&v, d, 4); sy.params.osc1.wave = (lfe_synth_waveform)v; }
static void sfmt_osc1_detune(char *b, int n) { snprintf(b, n, "%+5ld", (long)sy.params.osc1.detune_hz); }
static void sadj_osc1_detune(int d)          { int v = sy.params.osc1.detune_hz; wv_int_clamped(&v, d, 1, -1000, 1000); sy.params.osc1.detune_hz = v; }
static void sfmt_osc1_level(char *b, int n)  { snprintf(b, n, "%+4d dB", sy.osc1_db >> 8); }
static void sadj_osc1_level(int d)           { wv_db_step(&sy.osc1_db, &sy.params.osc1.level, d); }
static void sfmt_osc1_pw(char *b, int n)     { snprintf(b, n, "%3d%%", (sy.params.osc1.pulse_width * 100) / LFE_Q15_ONE); }
static void sadj_osc1_pw(int d) {
    int v = sy.params.osc1.pulse_width;
    wv_int_clamped(&v, d, 256, 256, LFE_Q15_ONE - 256);
    sy.params.osc1.pulse_width = (uint16_t)v;
}

static void sfmt_osc2_wave(char *b, int n)   { snprintf(b, n, "%s", synth_wave_names[sy.params.osc2.wave]); }
static void sadj_osc2_wave(int d)            { int v = sy.params.osc2.wave; wv_cycle(&v, d, 4); sy.params.osc2.wave = (lfe_synth_waveform)v; }
static void sfmt_osc2_detune(char *b, int n) { snprintf(b, n, "%+5ld", (long)sy.params.osc2.detune_hz); }
static void sadj_osc2_detune(int d)          { int v = sy.params.osc2.detune_hz; wv_int_clamped(&v, d, 1, -1000, 1000); sy.params.osc2.detune_hz = v; }
static void sfmt_osc2_level(char *b, int n)  { snprintf(b, n, "%+4d dB", sy.osc2_db >> 8); }
static void sadj_osc2_level(int d)           { wv_db_step(&sy.osc2_db, &sy.params.osc2.level, d); }
static void sfmt_osc2_pw(char *b, int n)     { snprintf(b, n, "%3d%%", (sy.params.osc2.pulse_width * 100) / LFE_Q15_ONE); }
static void sadj_osc2_pw(int d) {
    int v = sy.params.osc2.pulse_width;
    wv_int_clamped(&v, d, 256, 256, LFE_Q15_ONE - 256);
    sy.params.osc2.pulse_width = (uint16_t)v;
}

static void sfmt_noise(char *b, int n) { snprintf(b, n, "%+4d dB", sy.noise_db >> 8); }
static void sadj_noise(int d)          { wv_db_step(&sy.noise_db, &sy.params.noise_level, d); }

static void sfmt_fmode(char *b, int n) { snprintf(b, n, "%s", synth_filter_names[sy.params.filter_mode]); }
static void sadj_fmode(int d)          { int v = sy.params.filter_mode; wv_cycle(&v, d, 4); sy.params.filter_mode = (lfe_drum_filter_mode)v; }
static void sfmt_fcut(char *b, int n)  { snprintf(b, n, "%5lu", (unsigned long)sy.params.filter_base_hz); }
static void sadj_fcut(int d) {
    int v = (int)sy.params.filter_base_hz;
    wv_int_clamped(&v, d, 50, 50, 16000);
    sy.params.filter_base_hz = (uint32_t)v;
}
static void sfmt_fq(char *b, int n)    { snprintf(b, n, "%3d%%", (sy.params.filter_q * 100) / LFE_Q15_ONE); }
static void sadj_fq(int d) {
    int v = sy.params.filter_q;
    wv_int_clamped(&v, d, 256, 0, LFE_Q15_ONE);
    sy.params.filter_q = (uint16_t)v;
}

static void sfmt_master(char *b, int n) { snprintf(b, n, "%+4d dB", sy.master_db >> 8); }
static void sadj_master(int d)          { wv_db_step(&sy.master_db, &sy.params.master_level, d); }

static void sfmt_combine(char *b, int n) { snprintf(b, n, "%s", synth_combine_names[sy.params.combine]); }
static void sadj_combine(int d)          { int v = sy.params.combine; wv_cycle(&v, d, SYNTH_COMBINE_COUNT); sy.params.combine = (lfe_synth_osc_combine)v; }

/* Combine params are mode-specific. P1 is FM depth (Q15 %) or CALVARIO
 * gain1 (Q8.8 dB) or unused; P2 is CALVARIO gain2 or unused. Both step
 * in 1 dB / 256-LSB units regardless of the encoding — at 0..Q15_ONE
 * that's ~0.78% per step, which is fine for the FM-depth knob too. */
static void sfmt_cp_inner(char *b, int n, int raw, int which) {
    switch (sy.params.combine) {
    case LFE_SYNTH_COMBINE_FM:
        if (which == 1) {
            int pct = (raw * 100) / LFE_Q15_ONE;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            snprintf(b, n, "%3d%%", pct);
        } else { snprintf(b, n, "---"); }
        break;
    case LFE_SYNTH_COMBINE_CALVARIO:
        snprintf(b, n, "%3d dB", raw >> 8);
        break;
    default:
        snprintf(b, n, "---");
        break;
    }
}
static void sfmt_cp1(char *b, int n) { sfmt_cp_inner(b, n, sy.params.combine_param1, 1); }
static void sfmt_cp2(char *b, int n) { sfmt_cp_inner(b, n, sy.params.combine_param2, 2); }
static void sadj_cp1(int d) {
    int v = sy.params.combine_param1;
    wv_int_clamped(&v, d, 256, -16384, LFE_Q15_ONE);
    sy.params.combine_param1 = v;
}
static void sadj_cp2(int d) {
    int v = sy.params.combine_param2;
    wv_int_clamped(&v, d, 256, -16384, LFE_Q15_ONE);
    sy.params.combine_param2 = v;
}

static void sfmt_edit_mods(char *b, int n) { (void)b; (void)n; b[0] = '\0'; }
static void sadj_edit_mods(int d)          { if (d >= 0) sy.subpage = 1; }

/* ------------------------------------------------------------------ */
/* Per-row functions (mod-slot subpage)                                */
/* ------------------------------------------------------------------ */

#define SYNTH_MOD_CUR()  (&sy.params.mods[sy.mod_slot])

static int  smod_target(void)         { return sy.params.mods[sy.mod_slot].target; }
static bool smod_target_is_q15(int t) {
    return t == LFE_SYNTH_MOD_AMP || t == LFE_SYNTH_MOD_OSC1_LEVEL ||
           t == LFE_SYNTH_MOD_OSC2_LEVEL || t == LFE_SYNTH_MOD_NOISE_LEVEL ||
           t == LFE_SYNTH_MOD_PULSE_WIDTH;
}
static bool smod_target_is_hz(int t)  {
    return t == LFE_SYNTH_MOD_PITCH || t == LFE_SYNTH_MOD_FILTER;
}

static void smfmt_slot(char *b, int n)    { snprintf(b, n, "%d/%d", sy.mod_slot + 1, LFE_SYNTH_NUM_MODS); }
static void smadj_slot(int d)             { wv_cycle(&sy.mod_slot, d, LFE_SYNTH_NUM_MODS); }

static void smfmt_target(char *b, int n)  { snprintf(b, n, "%s", synth_mod_target_names[smod_target()]); }
static void smadj_target(int d) {
    int v = SYNTH_MOD_CUR()->target;
    wv_cycle(&v, d, SYNTH_MOD_TARGET_COUNT);
    SYNTH_MOD_CUR()->target = (lfe_synth_mod_target)v;
}

static void smfmt_depth(char *b, int n) {
    int t = smod_target();
    int v = SYNTH_MOD_CUR()->depth;
    if (t == LFE_SYNTH_MOD_NONE)        snprintf(b, n, "---");
    else if (smod_target_is_q15(t))     snprintf(b, n, "%+4d %%", (v * 100) / LFE_Q15_ONE);
    else if (smod_target_is_hz(t))      snprintf(b, n, "%+5d Hz", v);
    else                                 snprintf(b, n, "%+5d", v);
}
static void smadj_depth(int d) {
    int t = smod_target();
    int step, lo = -32768, hi = 32767;
    if (t == LFE_SYNTH_MOD_NONE) return;
    if      (smod_target_is_q15(t)) { step = 256; lo = -LFE_Q15_ONE; hi = LFE_Q15_ONE; }
    else if (smod_target_is_hz(t))  { step = 50;  lo = -8000;        hi = 8000; }
    else                             { step = 256; }
    int v = SYNTH_MOD_CUR()->depth;
    wv_int_clamped(&v, d, step, lo, hi);
    SYNTH_MOD_CUR()->depth = v;
}

static void smfmt_attack(char *b, int n)  { snprintf(b, n, "%5lu", (unsigned long)SYNTH_MOD_CUR()->env.attack_ms); }
static void smadj_attack(int d) {
    int v = (int)SYNTH_MOD_CUR()->env.attack_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    SYNTH_MOD_CUR()->env.attack_ms = (uint32_t)v;
}
static void smfmt_decay(char *b, int n)   { snprintf(b, n, "%5lu", (unsigned long)SYNTH_MOD_CUR()->env.decay_ms); }
static void smadj_decay(int d) {
    int v = (int)SYNTH_MOD_CUR()->env.decay_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    SYNTH_MOD_CUR()->env.decay_ms = (uint32_t)v;
}
static void smfmt_sustain(char *b, int n) { snprintf(b, n, "%+4d dB", sy.mod_sustain_db[sy.mod_slot] >> 8); }
static void smadj_sustain(int d) {
    wv_db_step(&sy.mod_sustain_db[sy.mod_slot],
               &SYNTH_MOD_CUR()->env.sustain_level, d);
}
static void smfmt_release(char *b, int n) { snprintf(b, n, "%5lu", (unsigned long)SYNTH_MOD_CUR()->env.release_ms); }
static void smadj_release(int d) {
    int v = (int)SYNTH_MOD_CUR()->env.release_ms;
    wv_int_clamped(&v, d, 10, 0, 8000);
    SYNTH_MOD_CUR()->env.release_ms = (uint32_t)v;
}
static void smfmt_peak(char *b, int n)    { snprintf(b, n, "%+4d dB", sy.mod_peak_db[sy.mod_slot] >> 8); }
static void smadj_peak(int d) {
    wv_db_step(&sy.mod_peak_db[sy.mod_slot],
               &SYNTH_MOD_CUR()->env.peak_level, d);
}

/* ------------------------------------------------------------------ */
/* Dispatch arrays                                                     */
/* ------------------------------------------------------------------ */

WV_ROW_DISPATCH(synth,     SYNTH_PARAMS,     SYNTH_PARAM_COUNT);
WV_ROW_DISPATCH(synth_mod, SYNTH_MOD_PARAMS, SYNTH_MOD_PARAM_COUNT);

/* ------------------------------------------------------------------ */
/* Input + draw                                                        */
/* ------------------------------------------------------------------ */

static void synth_input(u32 down, u32 held)
{
    if (down & MT_KEY_MOD_PRIMARY) { synth_generate(); return; }
    if (sy.subpage == 0)
        wv_handle_row_input(&sy.param_row, SYNTH_PARAM_COUNT, synth_adjs, down, held);
    else
        wv_handle_row_input(&sy.mod_param_row, SYNTH_MOD_PARAM_COUNT, synth_mod_adjs, down, held);
}

static void synth_draw(u8 *top_fb)
{
    int list_height = font_scale_row(26) - 5;
    if (sy.subpage == 0) {
        font_puts(top_fb, 1, 3, "Generator: Subtractive + Combine", PAL_DIM);
        synth_sv.row_height = list_height;
        synth_sv.total      = SYNTH_PARAM_COUNT;
        synth_sv.cursor     = sy.param_row;

        int db_max = -LFE_DB_FLOOR_Q8_8;
        wv_fader_info faders[SYNTH_PARAM_COUNT] = {
            [SYNTH_PARAM_NOTE_OFF_MS]= { sy.note_off_ms,               4000,   PAL_ORANGE },
            [SYNTH_PARAM_OSC1_LEVEL] = { sy.osc1_db - LFE_DB_FLOOR_Q8_8,   db_max, PAL_PLAY   },
            [SYNTH_PARAM_OSC1_PW]    = { sy.params.osc1.pulse_width,   LFE_Q15_ONE, PAL_PARAM  },
            [SYNTH_PARAM_OSC2_LEVEL] = { sy.osc2_db - LFE_DB_FLOOR_Q8_8,   db_max, PAL_PLAY   },
            [SYNTH_PARAM_OSC2_PW]    = { sy.params.osc2.pulse_width,   LFE_Q15_ONE, PAL_PARAM  },
            [SYNTH_PARAM_NOISE_LEVEL]= { sy.noise_db - LFE_DB_FLOOR_Q8_8,  db_max, PAL_PLAY   },
            [SYNTH_PARAM_FILTER_Q]   = { sy.params.filter_q,           LFE_Q15_ONE, PAL_EFFECT },
            [SYNTH_PARAM_MASTER]     = { sy.master_db - LFE_DB_FLOOR_Q8_8,  db_max, PAL_PLAY   },
        };

        wv_draw_rows_ex(top_fb, &synth_sv, 1, font_scale_col(28),
                        synth_param_labels, synth_fmts, faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  X:generate", PAL_DIM);
    } else {
        font_printf(top_fb, 1, 3, PAL_DIM, "Mod Slot Editor (slot %d/%d)",
                    sy.mod_slot + 1, LFE_SYNTH_NUM_MODS);
        synth_mod_sv.row_height = list_height;
        synth_mod_sv.total      = SYNTH_MOD_PARAM_COUNT;
        synth_mod_sv.cursor     = sy.mod_param_row;

        lfe_synth_mod *m = SYNTH_MOD_CUR();
        int mdb_max = -LFE_DB_FLOOR_Q8_8;
        wv_fader_info mod_faders[SYNTH_MOD_PARAM_COUNT] = {
            [SYNTH_MOD_PARAM_ATTACK]  = { (int)m->env.attack_ms,   8000,   PAL_ORANGE },
            [SYNTH_MOD_PARAM_DECAY]   = { (int)m->env.decay_ms,    8000,   PAL_ORANGE },
            [SYNTH_MOD_PARAM_SUSTAIN] = { sy.mod_sustain_db[sy.mod_slot] - LFE_DB_FLOOR_Q8_8, mdb_max, PAL_PLAY },
            [SYNTH_MOD_PARAM_RELEASE] = { (int)m->env.release_ms,  8000,   PAL_ORANGE },
            [SYNTH_MOD_PARAM_PEAK]    = { sy.mod_peak_db[sy.mod_slot] - LFE_DB_FLOOR_Q8_8,    mdb_max, PAL_PLAY },
        };

        wv_draw_rows_ex(top_fb, &synth_mod_sv, 1, font_scale_col(28),
                        synth_mod_param_labels, synth_mod_fmts, mod_faders);
        font_puts(top_fb, 0, font_scale_row(26),
                  "A+L/R:1  A+UP/DN:10  B:back", PAL_DIM);
    }
}

/* ------------------------------------------------------------------ */
/* Descriptor                                                          */
/* ------------------------------------------------------------------ */

static void synth_on_open(void)        { synth_load_preset(sy.preset); }
static bool synth_has_subpage(void)    { return sy.subpage != 0; }
static void synth_close_subpage(void)  { sy.subpage = 0; }

const wv_tab wv_tab_synth = {
    .name          = "Synth",
    .on_open       = synth_on_open,
    .input         = synth_input,
    .subpage_open  = synth_has_subpage,
    .close_subpage = synth_close_subpage,
    .draw_params   = synth_draw,
};

#endif /* MAXTRACKER_LFE */
