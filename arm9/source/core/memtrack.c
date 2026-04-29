/*
 * memtrack.c — Memory usage tracking for maxtracker.
 */

#include "memtrack.h"
#include "song.h"

void mt_mem_usage(MT_MemUsage *out)
{
    out->song_struct = sizeof(MT_Song);
    out->patterns = 0;
    out->samples = 0;
    out->patt_count = 0;
    out->samp_count = 0;

    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i]) {
            out->patterns += MT_PATTERN_SIZE(song.patterns[i]->nrows,
                                               song.patterns[i]->ncols);
            out->patt_count++;
        }
    }

    for (int i = 0; i < MT_MAX_SAMPLES; i++) {
        if (song.samples[i].active && song.samples[i].pcm_data) {
            u32 bps = (song.samples[i].format == 1) ? 2 : 1;
            out->samples += song.samples[i].length * bps;
            out->samp_count++;
        }
    }

    out->total = out->song_struct + out->patterns + out->samples;
    out->available = MT_RAM_AVAILABLE;
}

u32 mt_mem_free_budget(void)
{
    MT_MemUsage u;
    mt_mem_usage(&u);
    if (u.total >= MT_RAM_AVAILABLE) return 0;
    return MT_RAM_AVAILABLE - u.total;
}

u32 mt_mem_estimate_mas(u8 patt_count, u8 samp_count, u32 file_size)
{
    /* Estimate: each pattern uses full 256 rows x 32 channels */
    u32 patt_mem = (u32)patt_count * MT_PATTERN_SIZE(MT_MAX_ROWS, MT_MAX_CHANNELS);

    /* Sample PCM is duplicated: once in the file buffer (temporary),
     * once in malloc'd sample slots (permanent).
     * Rough estimate: file minus header/pattern overhead = sample data.
     * Header+offsets ~= 300 + patt_count*40 (RLE patterns are small).
     * We only count permanent allocations, not the temporary file buffer
     * since that's freed before playback. */
    u32 header_est = 300 + (u32)patt_count * 40;
    u32 samp_mem = (file_size > header_est) ? (file_size - header_est) : 0;

    /* MT_Song struct (always allocated in BSS) */
    u32 song_mem = sizeof(MT_Song);

    /* The file buffer is temporary (freed after parsing) so we don't
     * count it against the permanent budget. However, during loading
     * both the file buffer AND the song data coexist briefly. */
    u32 peak_load = file_size; /* temporary, freed after parse */

    /* Return permanent usage estimate (what stays allocated after loading).
     * The temporary file buffer (~file_size) also needed during load but
     * freed after parsing. We check permanent against budget. */
    (void)peak_load;
    return patt_mem + samp_mem + song_mem;
}
