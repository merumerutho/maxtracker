/*
 * engine_stubs.c — Global variables and stubs for host-native maxmod engine.
 *
 * Provides the globals normally defined in main_ds7.c and mixer.c.
 * Compiled with -include mm_engine_shim.h just like the engine sources.
 */

#include <mm_mas.h>
#include "core/channel_types.h"

/* Mixer channel array (normally in ds/arm7/mixer.c) */
mm_mixer_channel mm_mix_channels[NUM_CHANNELS];

/* Memory tracking stub (normally in memtrack.c).
 * Return 0 = "no bytes needed" so mt_mem_check (which uses `<=`) always
 * passes. Returning 0xFFFFFFFF would mean "needs 4GB" and fail the check. */
u32 mt_mem_estimate_mas(u8 patt_count, u32 sample_region_bytes)
{
    (void)patt_count; (void)sample_region_bytes;
    return 0;
}

/* Soundbank pointers (normally in ds/arm7/main_ds7.c) */
mm_addr  *mmModuleBank = NULL;
mm_word  *mmSampleBank = NULL;
msl_head *mp_solution  = NULL;
