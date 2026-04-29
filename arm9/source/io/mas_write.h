/*
 * mas_write.h -- Serialize MT_Song to .mas binary format.
 *
 * The output is compatible with stock maxmod (mmPlayMAS).
 */

#ifndef MAS_WRITE_H
#define MAS_WRITE_H

#include <nds.h>
#include "song.h"

/*
 * Write the song to a .mas file at the given path.
 *
 * Returns 0 on success, negative on error:
 *   -1  Could not open file for writing
 *   -2  Could not allocate serialization buffer
 *   -3  Write error (disk full / SD removed)
 */
int mas_write(const char *path, const MT_Song *song);

#endif /* MAS_WRITE_H */
