/*
 * mas_load.h -- Parse a .mas binary file into the MT_Song model.
 *
 * Supports standalone .mas files with embedded samples (NDS target).
 * MSL soundbank references (external samples) are not yet supported.
 */

#ifndef MAS_LOAD_H
#define MAS_LOAD_H

#include <nds.h>
#include "song.h"

/*
 * Load a .mas file from disk and populate the song model.
 *
 * Frees the current song internally. If a song is already loaded,
 * a swap backup is written first; on load failure the backup is
 * restored so the previous song survives. On NitroFS (no write
 * support) the backup step is silently skipped.
 *
 * Returns:
 *    0  Success
 *    1  Partial load (some patterns dropped due to RAM)
 *    2  MAS_LOAD_RESTORED — load failed, previous song restored
 *   -1  Could not open file
 *   -2  File too small / truncated
 *   -3  Not a song-type MAS file
 *   -4  Memory allocation failure
 *   -5  Parse error (corrupt data)
 *   -6  MAS too big (won't fit in RAM)
 */
#define MAS_LOAD_RESTORED 2

int mas_load(const char *path, MT_Song *song);

#endif /* MAS_LOAD_H */
