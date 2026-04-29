/*
 * song.c — Song model management.
 */

#include "song.h"
#include <stdlib.h>
#include <string.h>

MT_Song song;

/* Pattern lifecycle hooks (registered by playback.c at init). NULL when
 * nothing cares — for example in host tests that don't link playback. */
static song_lifecycle_cb hook_detach   = NULL;
static song_lifecycle_cb hook_reattach = NULL;

void song_set_pattern_lifecycle(song_lifecycle_cb on_detach,
                                song_lifecycle_cb on_reattach)
{
    hook_detach   = on_detach;
    hook_reattach = on_reattach;
}

/* Initialize an envelope to disabled/empty state */
static void envelope_init(MT_Envelope *env)
{
    memset(env, 0, sizeof(MT_Envelope));
    env->loop_start = 255;
    env->loop_end   = 255;
    env->sus_start  = 255;
    env->sus_end    = 255;
    env->enabled    = false;
}

void song_init(void)
{
    memset(&song, 0, sizeof(MT_Song));

    strcpy(song.name, "untitled");
    song.initial_speed  = 6;
    song.initial_tempo  = 125;
    song.global_volume  = 64;
    song.repeat_position = 0;
    song.channel_count  = MT_MAX_CHANNELS;  /* always 32 channels available */
    song.freq_linear    = true;
    song.xm_mode        = true;
    song.old_mode       = false;
    song.link_gxx       = false;

    for (int i = 0; i < MT_MAX_CHANNELS; i++) {
        song.channel_volume[i]  = 64;
        song.channel_panning[i] = 128;
    }

    song.order_count = 1;
    song.orders[0]   = 0;
    song.patt_count  = 1;
    song.inst_count  = 1;

    /* Initialize all instrument envelope markers to "none" */
    for (int i = 0; i < MT_MAX_INSTRUMENTS; i++) {
        envelope_init(&song.instruments[i].env_vol);
        envelope_init(&song.instruments[i].env_pan);
        envelope_init(&song.instruments[i].env_pitch);
    }

    /* Instrument 1 (index 0) gets sensible defaults */
    song.instruments[0].active        = true;
    song.instruments[0].global_volume = 128;
    song.instruments[0].panning       = 0xC0; /* 0x80 | 64 = center stereo */
    song.instruments[0].sample        = 1;

    /* Sample 1 (index 0): default square wave so playback works out of the box */
    {
        u8 *pcm = (u8 *)malloc(256);
        if (pcm) {
            /* Generate a simple square wave: -64 / +63 */
            for (int i = 0; i < 256; i++)
                pcm[i] = (u8)((i < 128) ? 0xC0 : 0x3F);  /* signed: -64 / +63 */

            song.samples[0].active         = true;
            song.samples[0].pcm_data       = pcm;
            song.samples[0].length         = 256;
            song.samples[0].format         = 0;    /* 8-bit */
            song.samples[0].bits           = 8;
            song.samples[0].base_freq      = 8363;
            song.samples[0].default_volume = 64;
            song.samples[0].panning        = 0xC0; /* 0x80 | 64 = center stereo */
            song.samples[0].global_volume  = 64;
            song.samples[0].loop_start     = 0;
            song.samples[0].loop_length    = 256;
            song.samples[0].loop_type      = 1;    /* forward loop */
            song.samples[0].drawn          = false;
            strcpy(song.samples[0].name, "Square");
            song.samp_count = 1;
        }
    }

    /* Allocate pattern 0 */
    song_ensure_pattern(0);
}

MT_Pattern *song_ensure_pattern(u8 index)
{
    if (song.patterns[index])
        return song.patterns[index];

    return song_alloc_pattern(index, 64, song.channel_count);
}

MT_Pattern *song_alloc_pattern(u8 index, u16 nrows, u8 ncols)
{
    if (nrows == 0) nrows = 64;
    if (ncols == 0) ncols = 8;
    if (nrows > MT_MAX_ROWS) nrows = MT_MAX_ROWS;
    if (ncols > MT_MAX_CHANNELS) ncols = MT_MAX_CHANNELS;

    u32 size = MT_PATTERN_SIZE(nrows, ncols);
    MT_Pattern *pat = (MT_Pattern *)malloc(size);
    if (!pat) return NULL;

    pat->nrows = nrows;
    pat->ncols = ncols;
    pat->_pad  = 0;

    /* Fill all cells with empty values */
    u32 total_cells = (u32)nrows * (u32)ncols;
    for (u32 i = 0; i < total_cells; i++) {
        pat->cells[i].note  = NOTE_EMPTY;
        pat->cells[i].inst  = 0;
        pat->cells[i].vol   = 0;
        pat->cells[i].fx    = 0;
        pat->cells[i].param = 0;
    }

    /* Free existing pattern at this index if any.
     * Notify subsystems holding the cells pointer first (e.g. the playback
     * engine sharing it with ARM7) so they can detach before the memory
     * goes away. */
    if (song.patterns[index]) {
        if (hook_detach) hook_detach();
        free(song.patterns[index]);
    }

    song.patterns[index] = pat;

    if (index >= song.patt_count)
        song.patt_count = index + 1;

    /* Notify subsystems that a fresh pattern is in place. The hook itself
     * decides whether anything needs to happen (e.g. only reattach if
     * playback is currently active). */
    if (hook_reattach) hook_reattach();

    return pat;
}

MT_Pattern *song_resize_pattern(u8 index, u16 new_nrows)
{
    MT_Pattern *old = song.patterns[index];
    if (!old) return NULL;
    if (new_nrows == 0)            new_nrows = 1;
    if (new_nrows > MT_MAX_ROWS)   new_nrows = MT_MAX_ROWS;
    if (new_nrows == old->nrows)   return old;

    u8 ncols = old->ncols;
    u32 size = MT_PATTERN_SIZE(new_nrows, ncols);
    MT_Pattern *pat = (MT_Pattern *)malloc(size);
    if (!pat) return NULL;

    pat->nrows = new_nrows;
    pat->ncols = ncols;
    pat->_pad  = 0;

    u16 copy_rows = old->nrows < new_nrows ? old->nrows : new_nrows;
    u32 copy_cells = (u32)copy_rows * (u32)ncols;
    memcpy(pat->cells, old->cells, copy_cells * sizeof(MT_Cell));

    if (new_nrows > old->nrows) {
        u32 new_cells = (u32)(new_nrows - old->nrows) * (u32)ncols;
        MT_Cell *dst = &pat->cells[copy_cells];
        for (u32 i = 0; i < new_cells; i++) {
            dst[i].note  = NOTE_EMPTY;
            dst[i].inst  = 0;
            dst[i].vol   = 0;
            dst[i].fx    = 0;
            dst[i].param = 0;
        }
    }

    if (hook_detach) hook_detach();
    free(old);
    song.patterns[index] = pat;
    if (hook_reattach) hook_reattach();

    return pat;
}

void song_free(void)
{
    /* Notify subsystems before any pattern memory is released. */
    if (hook_detach) hook_detach();

    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i]) {
            free(song.patterns[i]);
            song.patterns[i] = NULL;
        }
    }

    /* Free any malloc'd sample PCM data */
    for (int i = 0; i < MT_MAX_SAMPLES; i++) {
        if (song.samples[i].pcm_data) {
            free(song.samples[i].pcm_data);
            song.samples[i].pcm_data = NULL;
        }
        song.samples[i].active = false;
    }
}
