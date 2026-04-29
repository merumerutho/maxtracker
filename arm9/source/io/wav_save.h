/*
 * wav_save.h — WAV file writer for maxtracker.
 *
 * Minimal RIFF/WAV writer. Supports 16-bit signed mono only — that's
 * the format every LFE generator produces, and it's sufficient for
 * exporting draft samples to disk.
 *
 * Error codes mirror wav_load for consistency.
 */

#ifndef MT_WAV_SAVE_H
#define MT_WAV_SAVE_H

#include <nds.h>

#define WAV_SAVE_OK          0
#define WAV_SAVE_ERR_NULL   -1
#define WAV_SAVE_ERR_OPEN   -2
#define WAV_SAVE_ERR_WRITE  -3

/*
 * wav_save_mono16 — Write 16-bit signed mono PCM data to a RIFF/WAV file.
 *
 * @param path         Destination file path (FAT filesystem).
 * @param pcm          Pointer to int16 sample array.
 * @param num_samples  Number of samples (NOT bytes — bytes = num_samples * 2).
 * @param sample_rate  Sample rate in Hz (e.g. 32000).
 *
 * @return WAV_SAVE_OK on success, negative error code on failure.
 *
 * Overwrites the file if it already exists. Creates parent directories
 * only if the FAT driver supports it (libfat generally does not; the
 * caller should ensure the directory exists before calling).
 */
int wav_save_mono16(const char *path, const s16 *pcm,
                    u32 num_samples, u32 sample_rate);

#endif /* MT_WAV_SAVE_H */
