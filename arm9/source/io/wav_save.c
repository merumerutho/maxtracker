/*
 * wav_save.c — Minimal RIFF/WAV writer (16-bit signed mono).
 *
 * Writes a complete RIFF/WAVE file in one pass: 44-byte header
 * followed by the raw PCM data. No chunked writes, no buffering
 * beyond what the FAT driver provides. The entire file fits in a
 * single fwrite pair (header + data), which is about as simple as
 * a WAV writer can get.
 */

#include "wav_save.h"

#include <stdio.h>
#include <string.h>

/* Little-endian helpers — ARM9 is LE natively, but explicit packing
 * makes the format spec clear and avoids any endianness surprises. */
static void put_u16_le(u8 *dst, u16 v)
{
    dst[0] = (u8)(v & 0xFF);
    dst[1] = (u8)(v >> 8);
}

static void put_u32_le(u8 *dst, u32 v)
{
    dst[0] = (u8)(v & 0xFF);
    dst[1] = (u8)((v >>  8) & 0xFF);
    dst[2] = (u8)((v >> 16) & 0xFF);
    dst[3] = (u8)((v >> 24) & 0xFF);
}

int wav_save_mono16(const char *path, const s16 *pcm,
                    u32 num_samples, u32 sample_rate)
{
    if (!path || !pcm) return WAV_SAVE_ERR_NULL;
    if (num_samples == 0 || sample_rate == 0) return WAV_SAVE_ERR_NULL;

    const u16 channels       = 1;
    const u16 bits_per_sample = 16;
    const u32 byte_rate      = sample_rate * channels * (bits_per_sample / 8);
    const u16 block_align    = channels * (bits_per_sample / 8);
    const u32 data_bytes     = num_samples * channels * (bits_per_sample / 8);
    const u32 riff_size      = 36 + data_bytes;   /* file size - 8 */

    /* Build the 44-byte header in a stack buffer. */
    u8 hdr[44];
    memcpy(hdr +  0, "RIFF", 4);
    put_u32_le(hdr +  4, riff_size);
    memcpy(hdr +  8, "WAVE", 4);

    memcpy(hdr + 12, "fmt ", 4);
    put_u32_le(hdr + 16, 16);               /* fmt chunk size */
    put_u16_le(hdr + 20, 1);                /* PCM format tag */
    put_u16_le(hdr + 22, channels);
    put_u32_le(hdr + 24, sample_rate);
    put_u32_le(hdr + 28, byte_rate);
    put_u16_le(hdr + 32, block_align);
    put_u16_le(hdr + 34, bits_per_sample);

    memcpy(hdr + 36, "data", 4);
    put_u32_le(hdr + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) return WAV_SAVE_ERR_OPEN;

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return WAV_SAVE_ERR_WRITE;
    }
    if (fwrite(pcm, 1, data_bytes, f) != data_bytes) {
        fclose(f);
        return WAV_SAVE_ERR_WRITE;
    }

    fclose(f);
    return WAV_SAVE_OK;
}
