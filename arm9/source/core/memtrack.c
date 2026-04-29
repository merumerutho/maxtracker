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

u32 mt_mem_estimate_mas(u8 patt_count, u32 sample_region_bytes)
{
    /* Decoded patterns: 64 rows × 32 channels × 5 bytes/cell each.
     * Compressed patterns in the file are much smaller (RLE), but
     * we allocate flat arrays for editing. */
    u32 patt_mem = (u32)patt_count * MT_PATTERN_SIZE(64, MT_MAX_CHANNELS);

    /* Sample PCM is stored uncompressed in MAS, so the on-disk region
     * size (computed from offset tables) closely matches the permanent
     * allocation. Per-sample headers (28 bytes each) are a tiny fraction. */
    u32 samp_mem = sample_region_bytes;

    u32 song_mem = sizeof(MT_Song);

    return patt_mem + samp_mem + song_mem;
}
