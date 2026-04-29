/*
 * wav_load.h — WAV file loader for maxtracker.
 *
 * Minimal RIFF/WAV parser. Supports 8-bit unsigned, 16-bit signed,
 * 24-bit, and 32-bit float PCM. Stereo is mixed down to mono.
 */

#ifndef MT_WAV_LOAD_H
#define MT_WAV_LOAD_H

#include <nds.h>

/* Error codes */
#define WAV_OK              0
#define WAV_ERR_OPEN       -1   /* Could not open file */
#define WAV_ERR_NOT_RIFF   -2   /* Missing RIFF header */
#define WAV_ERR_NOT_WAVE   -3   /* Missing WAVE identifier */
#define WAV_ERR_NO_FMT    -4   /* No fmt chunk found */
#define WAV_ERR_NO_DATA    -5   /* No data chunk found */
#define WAV_ERR_UNSUPPORTED -6  /* Unsupported format (not PCM/float) */
#define WAV_ERR_ALLOC      -7   /* Memory allocation failed */
#define WAV_ERR_READ       -8   /* File read error */
#define WAV_ERR_TOO_BIG    -9   /* Exceeds caller-provided memory budget */

/*
 * wav_load — Load a WAV file, returning allocated PCM data.
 *
 * @param path      Path to .wav file on SD card.
 * @param out_data  Receives malloc'd buffer of PCM data (caller must free).
 *                  8-bit data is converted to signed. Stereo is mixed to mono.
 *                  24-bit and 32-bit float are converted to 16-bit signed.
 * @param out_len   Receives length of PCM data in bytes.
 * @param out_rate  Receives sample rate in Hz.
 * @param out_bits  Receives bits per sample of output (8 or 16).
 *
 * @return WAV_OK on success, negative error code on failure.
 */
int wav_load(const char *path, u8 **out_data, u32 *out_len,
             u32 *out_rate, u8 *out_bits);

/*
 * wav_load_ex — same as wav_load, but aborts with WAV_ERR_TOO_BIG
 * before performing any allocation if either the final resident PCM
 * size OR the transient peak (raw + conversion buffer) would exceed
 * `max_bytes`. On WAV_ERR_TOO_BIG nothing is allocated and *out_data
 * stays NULL.
 *
 * Pass UINT32_MAX for an unchecked load (equivalent to wav_load).
 */
int wav_load_ex(const char *path, u32 max_bytes,
                u8 **out_data, u32 *out_len,
                u32 *out_rate, u8 *out_bits);

/*
 * wav_strerror — Return human-readable string for a WAV error code.
 */
const char *wav_strerror(int err);

#endif /* MT_WAV_LOAD_H */
