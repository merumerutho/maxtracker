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
 * The caller should call song_free() before this if the song already
 * has allocated data.  On success the global `song` is fully populated
 * with the imported data.
 *
 * Returns 0 on success, negative on error:
 *   -1  Could not open file
 *   -2  File too small / truncated
 *   -3  Not a song-type MAS file
 *   -4  Memory allocation failure
 *   -5  Parse error (corrupt data)
 */
int mas_load(const char *path, MT_Song *song);

#endif /* MAS_LOAD_H */
