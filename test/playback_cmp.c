/*
 * playback_cmp.c — End-to-end playback comparison test.
 *
 * Loads a .mas file and runs tick-by-tick playback through both:
 *   A) Original RLE pattern reader  (mt_shared = NULL)
 *   B) Flat-cell pattern reader     (mt_shared active)
 *
 * After each tick, snapshots per-active-channel state and compares the two
 * runs.  Any difference in envelope position, fade, flags, volume, or period
 * indicates a divergence between the two codepaths.
 *
 * Usage:  playback_cmp <input.mas> [max_ticks]
 *         Default max_ticks: 10000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mm_mas.h>
#include "core/channel_types.h"
#include "core/player_types.h"
#include "core/mas.h"

#include "song.h"
#include "mas_load.h"

/* song.h already defines MT_Cell. Prevent mt_shared.h from redefining it
   (the ARM7-side guard defines a duplicate struct). */
#define MT_CELL_DEFINED
#include "mt_shared.h"

/* ---- Engine globals (defined in mas.c, mas_arm.c) ---- */
extern mm_active_channel  *mm_achannels;
extern mm_module_channel  *mm_pchannels;
extern mm_module_channel   mm_schannels[];
extern mm_word             mm_num_mch;
extern mm_word             mm_num_ach;
extern mm_module_channel  *mpp_channels;
extern mm_byte             mpp_nchannels;
extern mm_layer_type       mpp_clayer;
extern mpl_layer_information mmLayerMain;
extern mpl_layer_information mmLayerSub;
extern mpv_active_information mpp_vars;
extern mpl_layer_information *mpp_layerp;
extern mm_word             mm_ch_mask;
extern mm_mixer_channel    mm_mix_channels[];

/* ---- Snapshot of one active channel ---- */
typedef struct {
    mm_word     period;
    mm_hword    fade;
    mm_hword    envc_vol;
    mm_hword    envc_pan;
    mm_hword    envc_pic;
    mm_byte     fvol;
    mm_byte     type;
    mm_byte     inst;
    mm_byte     panning;
    mm_byte     volume;
    mm_byte     sample;
    mm_byte     parent;
    mm_byte     flags;
    mm_byte     envn_vol;
    mm_byte     envn_pan;
    mm_byte     envn_pic;
} AChanSnap;

/* ---- Snapshot of one module channel ---- */
typedef struct {
    mm_byte     flags;
    mm_byte     inst;
    mm_byte     volume;
    mm_byte     panning;
    mm_byte     alloc;
    mm_word     period;
    mm_hword    bflags;
} MChanSnap;

/* ---- Per-tick snapshot ---- */
#define MAX_ACH 32
#define MAX_MCH 32

typedef struct {
    int         tick_num;
    mm_byte     layer_tick;
    mm_byte     layer_row;
    mm_byte     layer_pos;
    mm_byte     layer_playing;
    AChanSnap   ach[MAX_ACH];
    MChanSnap   mch[MAX_MCH];
} TickSnap;

/* ---- Global trace storage ---- */
static int       g_max_ticks = 10000;
static TickSnap *g_trace_a = NULL;  /* original reader */
static TickSnap *g_trace_b = NULL;  /* flat-cell reader */
static int       g_trace_len_a = 0;
static int       g_trace_len_b = 0;

/* ---- Engine allocation / init ---- */
static mm_active_channel  g_achannels[MAX_ACH];
static mm_module_channel  g_pchannels[MAX_MCH];

static void engine_init(int num_channels)
{
    /* Set up globals */
    mm_achannels  = g_achannels;
    mm_pchannels  = g_pchannels;
    mm_num_mch    = num_channels;
    mm_num_ach    = MAX_ACH;
    mm_ch_mask    = 0xFFFFFFFF;  /* all 32 channels available */

    memset(g_achannels, 0, sizeof(g_achannels));
    memset(g_pchannels, 0, sizeof(g_pchannels));
    memset(mm_mix_channels, 0, sizeof(mm_mixer_channel) * MAX_ACH);
    memset(&mpp_vars, 0, sizeof(mpp_vars));
    memset(&mmLayerMain, 0, sizeof(mmLayerMain));
    memset(&mmLayerSub, 0, sizeof(mmLayerSub));

    /* mm_masterpitch defaults to 1024 (1.0 in .10 fixed point) */
    extern mm_word mm_masterpitch;
    mm_masterpitch = 1024;
    memset(mm_schannels, 0, sizeof(mm_module_channel) * 4);

    mpp_channels  = g_pchannels;
    mpp_nchannels = num_channels;
    mpp_clayer    = MM_MAIN;
    mpp_layerp    = &mmLayerMain;
}

static void take_snapshot(TickSnap *snap, int tick_num, int num_mch)
{
    snap->tick_num      = tick_num;
    snap->layer_tick    = mmLayerMain.tick;
    snap->layer_row     = mmLayerMain.row;
    snap->layer_pos     = mmLayerMain.position;
    snap->layer_playing = mmLayerMain.isplaying;

    for (int i = 0; i < MAX_ACH; i++) {
        mm_active_channel *ac = &mm_achannels[i];
        AChanSnap *s = &snap->ach[i];
        s->period   = ac->period;
        s->fade     = ac->fade;
        s->envc_vol = ac->envc_vol;
        s->envc_pan = ac->envc_pan;
        s->envc_pic = ac->envc_pic;
        s->fvol     = ac->fvol;
        s->type     = ac->type;
        s->inst     = ac->inst;
        s->panning  = ac->panning;
        s->volume   = ac->volume;
        s->sample   = ac->sample;
        s->parent   = ac->parent;
        s->flags    = ac->flags;
        s->envn_vol = ac->envn_vol;
        s->envn_pan = ac->envn_pan;
        s->envn_pic = ac->envn_pic;
    }

    for (int i = 0; i < MAX_MCH && i < num_mch; i++) {
        mm_module_channel *mc = &mm_pchannels[i];
        MChanSnap *s = &snap->mch[i];
        s->flags   = mc->flags;
        s->inst    = mc->inst;
        s->volume  = mc->volume;
        s->panning = mc->panning;
        s->alloc   = mc->alloc;
        s->period  = mc->period;
        s->bflags  = mc->bflags;
    }
}

/* ---- Shared state for flat-cell reader ---- */
static MT_SharedPatternState g_shared_state;

/*
 * Callback: track position changes and update shared state cells pointer
 * when the engine advances to a new pattern.
 */
static mm_byte g_last_pos = 0xFF;

static uint32_t playback_callback(uint32_t msg, uint32_t param)
{
    if (msg == MMCB_SONGTICK) {
        mm_byte pos = (param >> 24) & 0xFF;
        if (pos != g_last_pos && mt_shared != NULL) {
            /* Position changed — update cells pointer */
            g_last_pos = pos;
            if (pos < song.order_count) {
                u8 patt_idx = song.orders[pos];
                MT_Pattern *pat = song.patterns[patt_idx];
                if (pat) {
                    g_shared_state.cells = (volatile MT_Cell *)pat->cells;
                    g_shared_state.nrows = pat->nrows;
                } else {
                    g_shared_state.cells = NULL;
                    g_shared_state.nrows = 64;
                }
                g_shared_state.channel_count = song.channel_count;
            }
        }
    }
    return 0;
}

/* ---- Run one full trace ---- */
static int run_trace(const u8 *mas_data, int use_flat_cells, TickSnap *trace)
{
    int num_ch = song.channel_count;
    engine_init(num_ch);

    /* Install callback */
    extern void mmSetEventHandler(mm_callback handler);
    mmSetEventHandler(playback_callback);

    if (use_flat_cells) {
        /* Set up shared state for flat-cell reader */
        memset(&g_shared_state, 0, sizeof(g_shared_state));
        g_shared_state.active = 1;
        g_shared_state.channel_count = num_ch;
        g_shared_state.mute_mask = 0;

        /* Point to first pattern */
        mm_mas_head *hdr = (mm_mas_head *)(mas_data + sizeof(mm_mas_prefix));
        u8 first_patt = hdr->sequence[0];
        if (first_patt < song.patt_count && song.patterns[first_patt]) {
            g_shared_state.cells = (volatile MT_Cell *)song.patterns[first_patt]->cells;
            g_shared_state.nrows = song.patterns[first_patt]->nrows;
        }

        mt_shared = &g_shared_state;
        g_last_pos = 0;
    } else {
        mt_shared = NULL;
        g_last_pos = 0xFF;
    }

    /* mpp_resolution must be set before mmPlayMAS (used by mpp_setbpm).
     * On real NDS: ~59.8261 Hz * 2.5 * 64 = 9572.  mmSetResolution(9572). */
    extern void mmSetResolution(mm_word divider);
    mmSetResolution(9572);

    /* Master tempo/pitch: 1.0 = 1024 in .10 fixed point */
    extern void mmSetModuleTempo(mm_word tempo);
    mmSetModuleTempo(1024);

    /* Start playback via mmPlayMAS */
    mmPlayMAS((uintptr_t)mas_data, MM_PLAY_ONCE, MM_MAIN);

    /* Set up for tick processing */
    mpp_channels  = mm_pchannels;
    mpp_nchannels = num_ch;
    mpp_clayer    = MM_MAIN;
    mpp_layerp    = &mmLayerMain;

    int tick_count = 0;
    while (tick_count < g_max_ticks && mmLayerMain.isplaying) {
        /* Before each tick, ensure mt_shared->cells points to the current
         * pattern.  The engine updates layer->position inside mppProcessTick
         * (via mpp_setposition), so we must sync cells to match. */
        if (use_flat_cells && mmLayerMain.isplaying) {
            u8 pos = mmLayerMain.position;
            if (pos < song.order_count) {
                u8 patt_idx = song.orders[pos];
                if (patt_idx < song.patt_count && song.patterns[patt_idx]) {
                    g_shared_state.cells = (volatile MT_Cell *)song.patterns[patt_idx]->cells;
                    g_shared_state.nrows = song.patterns[patt_idx]->nrows;
                }
            }
            g_shared_state.channel_count = num_ch;
        }

        mppProcessTick();
        take_snapshot(&trace[tick_count], tick_count, num_ch);
        tick_count++;
    }

    return tick_count;
}

/* ---- Compare traces ---- */

static const char *flag_name(int bit)
{
    switch (bit) {
    case 0x01: return "KEYON";
    case 0x02: return "FADE";
    case 0x04: return "START";
    case 0x08: return "UPDATED";
    case 0x10: return "ENVEND";
    case 0x20: return "VOLENV";
    case 0x40: return "SUB";
    case 0x80: return "EFFECT";
    default:   return "?";
    }
}

static void print_flag_diff(const char *prefix, mm_byte a, mm_byte b)
{
    mm_byte diff = a ^ b;
    printf("  %s: orig=0x%02X flat=0x%02X (", prefix, a, b);
    for (int bit = 1; bit <= 0x80; bit <<= 1) {
        if (diff & bit)
            printf(" %c%s", (a & bit) ? '-' : '+', flag_name(bit));
    }
    printf(" )\n");
}

static int compare_traces(int len_a, int len_b, int num_ch)
{
    int total_diffs = 0;
    int len = len_a < len_b ? len_a : len_b;

    if (len_a != len_b) {
        printf("WARNING: trace lengths differ: orig=%d flat=%d\n", len_a, len_b);
        total_diffs++;
    }

    for (int t = 0; t < len; t++) {
        TickSnap *a = &g_trace_a[t];
        TickSnap *b = &g_trace_b[t];

        /* Check layer state */
        if (a->layer_row != b->layer_row || a->layer_pos != b->layer_pos ||
            a->layer_tick != b->layer_tick) {
            if (total_diffs < 20)
                printf("TICK %d: position mismatch orig=pos%d/row%d/tick%d flat=pos%d/row%d/tick%d\n",
                       t, a->layer_pos, a->layer_row, a->layer_tick,
                       b->layer_pos, b->layer_row, b->layer_tick);
            total_diffs++;
            continue;  /* positions diverged — further comparison meaningless */
        }

        /* Compare active channels */
        for (int ch = 0; ch < MAX_ACH; ch++) {
            AChanSnap *aa = &a->ach[ch];
            AChanSnap *ba = &b->ach[ch];

            /* Skip if both disabled */
            if (aa->type == ACHN_DISABLED && ba->type == ACHN_DISABLED)
                continue;

            int diff = 0;

            if (aa->flags != ba->flags)    diff |= 1;
            if (aa->fade != ba->fade)      diff |= 2;
            if (aa->fvol != ba->fvol)      diff |= 4;
            if (aa->period != ba->period)  diff |= 8;
            if (aa->envn_vol != ba->envn_vol || aa->envc_vol != ba->envc_vol) diff |= 16;
            if (aa->envn_pan != ba->envn_pan || aa->envc_pan != ba->envc_pan) diff |= 32;
            if (aa->envn_pic != ba->envn_pic || aa->envc_pic != ba->envc_pic) diff |= 64;
            if (aa->type != ba->type)      diff |= 128;
            if (aa->volume != ba->volume)  diff |= 256;
            if (aa->inst != ba->inst)      diff |= 512;
            if (aa->sample != ba->sample)  diff |= 1024;
            if (aa->panning != ba->panning) diff |= 2048;

            if (diff) {
                total_diffs++;
                if (total_diffs <= 50) {
                    printf("TICK %d (pos%d row%02X tick%d) ACH %d:\n",
                           t, a->layer_pos, a->layer_row, a->layer_tick, ch);

                    if (diff & 1)
                        print_flag_diff("flags", aa->flags, ba->flags);
                    if (diff & 2)
                        printf("  fade:     orig=%d flat=%d\n", aa->fade, ba->fade);
                    if (diff & 4)
                        printf("  fvol:     orig=%d flat=%d\n", aa->fvol, ba->fvol);
                    if (diff & 8)
                        printf("  period:   orig=%u flat=%u\n", aa->period, ba->period);
                    if (diff & 16)
                        printf("  vol_env:  orig=node%d/cnt%d flat=node%d/cnt%d\n",
                               aa->envn_vol, aa->envc_vol, ba->envn_vol, ba->envc_vol);
                    if (diff & 32)
                        printf("  pan_env:  orig=node%d/cnt%d flat=node%d/cnt%d\n",
                               aa->envn_pan, aa->envc_pan, ba->envn_pan, ba->envc_pan);
                    if (diff & 64)
                        printf("  pic_env:  orig=node%d/cnt%d flat=node%d/cnt%d\n",
                               aa->envn_pic, aa->envc_pic, ba->envn_pic, ba->envc_pic);
                    if (diff & 128)
                        printf("  type:     orig=%d flat=%d\n", aa->type, ba->type);
                    if (diff & 256)
                        printf("  volume:   orig=%d flat=%d\n", aa->volume, ba->volume);
                    if (diff & 512)
                        printf("  inst:     orig=%d flat=%d\n", aa->inst, ba->inst);
                    if (diff & 1024)
                        printf("  sample:   orig=%d flat=%d\n", aa->sample, ba->sample);
                    if (diff & 2048)
                        printf("  panning:  orig=%d flat=%d\n", aa->panning, ba->panning);
                }
            }
        }

        /* Compare module channels */
        for (int ch = 0; ch < num_ch && ch < MAX_MCH; ch++) {
            MChanSnap *am = &a->mch[ch];
            MChanSnap *bm = &b->mch[ch];

            int diff = 0;
            if (am->volume != bm->volume)  diff |= 1;
            if (am->period != bm->period)  diff |= 2;
            if (am->alloc != bm->alloc)    diff |= 4;
            if (am->bflags != bm->bflags)  diff |= 8;
            if (am->panning != bm->panning) diff |= 16;

            if (diff) {
                total_diffs++;
                if (total_diffs <= 50) {
                    printf("TICK %d (pos%d row%02X tick%d) MCH %d:\n",
                           t, a->layer_pos, a->layer_row, a->layer_tick, ch);
                    if (diff & 1)
                        printf("  volume:   orig=%d flat=%d\n", am->volume, bm->volume);
                    if (diff & 2)
                        printf("  period:   orig=%u flat=%u\n", am->period, bm->period);
                    if (diff & 4)
                        printf("  alloc:    orig=%d flat=%d\n", am->alloc, bm->alloc);
                    if (diff & 8)
                        printf("  bflags:   orig=0x%04X flat=0x%04X\n", am->bflags, bm->bflags);
                    if (diff & 16)
                        printf("  panning:  orig=%d flat=%d\n", am->panning, bm->panning);
                }
            }
        }
    }

    return total_diffs;
}

/* ---- Main ---- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <input.mas> [max_ticks]\n", argv[0]);
        printf("  Runs tick-by-tick playback via original RLE reader and flat-cell reader,\n");
        printf("  comparing per-channel state after each tick.\n");
        return 1;
    }

    if (argc > 2)
        g_max_ticks = atoi(argv[2]);

    /* Load raw MAS file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *mas_data = (u8 *)malloc(sz);
    if (!mas_data) { fclose(f); return 1; }
    fread(mas_data, 1, sz, f);
    fclose(f);

    /* Parse into MT_Song (for flat cell arrays) */
    song_init();
    int rc = mas_load(argv[1], &song);
    if (rc < 0) {
        printf("mas_load failed: %d\n", rc);
        free(mas_data);
        return 1;
    }

    int num_ch = song.channel_count;
    printf("Loaded: %s (%d channels, %d patterns, %d orders)\n",
           argv[1], num_ch, song.patt_count, song.order_count);
    printf("Flags: %s%s%s%s\n",
           song.xm_mode ? "XM " : "IT ",
           song.freq_linear ? "LINEAR " : "AMIGA ",
           song.old_mode ? "OLD " : "",
           song.link_gxx ? "GXX " : "");
    printf("Max ticks: %d\n", g_max_ticks);

    /* Allocate trace buffers */
    g_trace_a = (TickSnap *)calloc(g_max_ticks, sizeof(TickSnap));
    g_trace_b = (TickSnap *)calloc(g_max_ticks, sizeof(TickSnap));
    if (!g_trace_a || !g_trace_b) {
        printf("Out of memory for traces\n");
        free(mas_data);
        return 1;
    }

    /* Run A: original RLE reader */
    printf("\n--- Run A: original RLE reader ---\n");
    g_trace_len_a = run_trace(mas_data, 0, g_trace_a);
    printf("  %d ticks processed (stopped at pos%d row%d)\n",
           g_trace_len_a,
           g_trace_len_a > 0 ? g_trace_a[g_trace_len_a - 1].layer_pos : 0,
           g_trace_len_a > 0 ? g_trace_a[g_trace_len_a - 1].layer_row : 0);

    /* Run B: flat-cell reader */
    printf("\n--- Run B: flat-cell reader ---\n");
    g_trace_len_b = run_trace(mas_data, 1, g_trace_b);
    printf("  %d ticks processed (stopped at pos%d row%d)\n",
           g_trace_len_b,
           g_trace_len_b > 0 ? g_trace_b[g_trace_len_b - 1].layer_pos : 0,
           g_trace_len_b > 0 ? g_trace_b[g_trace_len_b - 1].layer_row : 0);

    /* Compare */
    printf("\n--- Comparison ---\n");
    int diffs = compare_traces(g_trace_len_a, g_trace_len_b, num_ch);

    if (diffs == 0)
        printf("PASS: %d ticks, zero differences.\n", g_trace_len_a);
    else
        printf("\nFAIL: %d total differences across %d ticks.\n",
               diffs, g_trace_len_a < g_trace_len_b ? g_trace_len_a : g_trace_len_b);

    free(g_trace_a);
    free(g_trace_b);
    free(mas_data);
    song_free();

    return diffs > 0 ? 1 : 0;
}
