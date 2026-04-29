/*
 * wav_load.c — WAV file loader for maxtracker.
 */

#include "wav_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WAV format tags */
#define WAV_FORMAT_PCM    1
#define WAV_FORMAT_FLOAT  3

/* Read a little-endian u16 from buffer */
static u16 read_u16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

/* Read a little-endian u32 from buffer */
static u32 read_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Read 4 bytes from file into buf. Returns false on failure. */
static bool read4(FILE *f, u8 *buf)
{
    return fread(buf, 1, 4, f) == 4;
}

/* Check if 4-byte buffer matches a FourCC string */
static bool fourcc_eq(const u8 *buf, const char *cc)
{
    return memcmp(buf, cc, 4) == 0;
}

/*
 * Convert unsigned 8-bit PCM to signed 8-bit in-place.
 */
static void convert_u8_to_s8(u8 *data, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        data[i] = (u8)((s8)((int)data[i] - 128));
    }
}

/*
 * Mix stereo 8-bit signed to mono in-place.
 * Input: interleaved L,R,L,R... (count = total samples including both channels)
 * Output: mono samples in first half of buffer.
 * Returns number of mono samples.
 */
static u32 mix_stereo_8(s8 *data, u32 count)
{
    u32 mono_count = count / 2;
    for (u32 i = 0; i < mono_count; i++) {
        int l = data[i * 2];
        int r = data[i * 2 + 1];
        data[i] = (s8)((l + r) / 2);
    }
    return mono_count;
}

/*
 * Mix stereo 16-bit signed to mono in-place.
 * Returns number of mono samples.
 */
static u32 mix_stereo_16(s16 *data, u32 count)
{
    u32 mono_count = count / 2;
    for (u32 i = 0; i < mono_count; i++) {
        int l = data[i * 2];
        int r = data[i * 2 + 1];
        data[i] = (s16)((l + r) / 2);
    }
    return mono_count;
}

/*
 * Convert 24-bit PCM to 16-bit signed.
 * Input: packed 24-bit samples (3 bytes each), count samples.
 * Output: 16-bit samples written to out buffer.
 */
static void convert_24_to_16(const u8 *in, s16 *out, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        /* Take upper 16 bits of 24-bit sample (signed) */
        s32 sample = (s32)((u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16));
        /* Sign extend from 24 bits */
        if (sample & 0x800000)
            sample |= (s32)0xFF000000;
        out[i] = (s16)(sample >> 8);
        in += 3;
    }
}

/*
 * Convert 32-bit float PCM to 16-bit signed.
 * Input: float samples (4 bytes each, IEEE 754 LE), count samples.
 * Output: 16-bit samples written to out buffer.
 */
static void convert_float_to_16(const u8 *in, s16 *out, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        /* Read little-endian float */
        float f;
        memcpy(&f, in + i * 4, sizeof(float));

        /* Clamp to [-1.0, 1.0] then scale to s16 range */
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        int val = (int)(f * 32767.0f);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        out[i] = (s16)val;
    }
}

int wav_load(const char *path, u8 **out_data, u32 *out_len,
             u32 *out_rate, u8 *out_bits)
{
    return wav_load_ex(path, 0xFFFFFFFFu, out_data, out_len, out_rate, out_bits);
}

int wav_load_ex(const char *path, u32 max_bytes,
                u8 **out_data, u32 *out_len,
                u32 *out_rate, u8 *out_bits)
{
    FILE *f;
    u8 buf[8];
    u16 format_tag = 0;
    u16 num_channels = 0;
    u32 sample_rate = 0;
    u16 bits_per_sample = 0;
    u32 data_size = 0;
    bool got_fmt = false;
    bool got_data = false;

    *out_data = NULL;
    *out_len = 0;
    *out_rate = 0;
    *out_bits = 0;

    f = fopen(path, "rb");
    if (!f)
        return WAV_ERR_OPEN;

    /* ---- RIFF header ---- */
    if (!read4(f, buf) || !fourcc_eq(buf, "RIFF")) {
        fclose(f);
        return WAV_ERR_NOT_RIFF;
    }

    /* Skip RIFF chunk size */
    if (fread(buf, 1, 4, f) != 4) {
        fclose(f);
        return WAV_ERR_READ;
    }

    /* Verify WAVE */
    if (!read4(f, buf) || !fourcc_eq(buf, "WAVE")) {
        fclose(f);
        return WAV_ERR_NOT_WAVE;
    }

    /* ---- Parse chunks ---- */
    while (fread(buf, 1, 8, f) == 8) {
        u32 chunk_size = read_u32(buf + 4);

        if (fourcc_eq(buf, "fmt ")) {
            /* Read fmt chunk (at least 16 bytes) */
            u8 fmt_buf[40];
            u32 to_read = chunk_size < sizeof(fmt_buf) ? chunk_size : sizeof(fmt_buf);
            if (fread(fmt_buf, 1, to_read, f) != to_read) {
                fclose(f);
                return WAV_ERR_READ;
            }

            format_tag      = read_u16(fmt_buf + 0);
            num_channels    = read_u16(fmt_buf + 2);
            sample_rate     = read_u32(fmt_buf + 4);
            /* skip byte rate (4 bytes) and block align (2 bytes) */
            bits_per_sample = read_u16(fmt_buf + 14);

            got_fmt = true;

            /* Skip remainder of fmt chunk if larger than what we read */
            if (chunk_size > to_read) {
                fseek(f, chunk_size - to_read, SEEK_CUR);
            }

        } else if (fourcc_eq(buf, "data")) {
            data_size = chunk_size;
            got_data = true;
            break;  /* File position is now at start of PCM data */

        } else {
            /* Unknown chunk -- skip it */
            /* Chunks are word-aligned: if odd size, skip one extra byte */
            u32 skip = chunk_size + (chunk_size & 1);
            fseek(f, skip, SEEK_CUR);
        }
    }

    if (!got_fmt) {
        fclose(f);
        return WAV_ERR_NO_FMT;
    }
    if (!got_data) {
        fclose(f);
        return WAV_ERR_NO_DATA;
    }

    /* Validate format */
    if (format_tag != WAV_FORMAT_PCM && format_tag != WAV_FORMAT_FLOAT) {
        fclose(f);
        return WAV_ERR_UNSUPPORTED;
    }
    if (format_tag == WAV_FORMAT_FLOAT && bits_per_sample != 32) {
        fclose(f);
        return WAV_ERR_UNSUPPORTED;
    }
    if (format_tag == WAV_FORMAT_PCM &&
        bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24) {
        fclose(f);
        return WAV_ERR_UNSUPPORTED;
    }
    if (num_channels < 1 || num_channels > 2) {
        fclose(f);
        return WAV_ERR_UNSUPPORTED;
    }

    /* Compute sample count */
    u32 bytes_per_sample = bits_per_sample / 8;
    u32 frame_size = bytes_per_sample * num_channels;
    u32 total_frames = data_size / frame_size;

    if (total_frames == 0) {
        fclose(f);
        return WAV_ERR_READ;
    }

    /* ---- Pre-flight memory budget ----
     * Reject oversized files BEFORE any allocation so a bad load never
     * partially mutates heap state. We check both:
     *   - `resident`: bytes that stay resident after conversion/mono-mix
     *     (what the sample slot ends up holding).
     *   - `peak`: worst-case live allocation during conversion (raw +
     *     buf16 coexist briefly for 24-bit and float inputs).
     */
    u32 raw_size = total_frames * frame_size;
    {
        u32 out_bps = (bits_per_sample == 8) ? 1 : 2;
        u32 resident = total_frames * out_bps;  /* post mono-mix */
        u32 conv_tmp = 0;
        if (bits_per_sample == 24 || format_tag == WAV_FORMAT_FLOAT)
            conv_tmp = total_frames * num_channels * 2;  /* s16 buf16 */
        u32 peak = raw_size + conv_tmp;
        if (resident > max_bytes || peak > max_bytes) {
            fclose(f);
            return WAV_ERR_TOO_BIG;
        }
    }

    /* ---- Read raw PCM data ---- */
    u8 *raw = (u8 *)malloc(raw_size);
    if (!raw) {
        fclose(f);
        return WAV_ERR_ALLOC;
    }

    if (fread(raw, 1, raw_size, f) != raw_size) {
        free(raw);
        fclose(f);
        return WAV_ERR_READ;
    }
    fclose(f);

    /* ---- Convert to output format ---- */

    /*
     * 24-bit and float -> convert to 16-bit first, then handle stereo.
     * 8-bit and 16-bit -> handle stereo on raw data.
     */

    if (bits_per_sample == 24) {
        /* Convert 24-bit to 16-bit */
        u32 total_samples = total_frames * num_channels;
        s16 *buf16 = (s16 *)malloc(total_samples * sizeof(s16));
        if (!buf16) {
            free(raw);
            return WAV_ERR_ALLOC;
        }
        convert_24_to_16(raw, buf16, total_samples);
        free(raw);
        raw = (u8 *)buf16;
        bits_per_sample = 16;
        bytes_per_sample = 2;

    } else if (format_tag == WAV_FORMAT_FLOAT) {
        /* Convert 32-bit float to 16-bit */
        u32 total_samples = total_frames * num_channels;
        s16 *buf16 = (s16 *)malloc(total_samples * sizeof(s16));
        if (!buf16) {
            free(raw);
            return WAV_ERR_ALLOC;
        }
        convert_float_to_16(raw, buf16, total_samples);
        free(raw);
        raw = (u8 *)buf16;
        bits_per_sample = 16;
        bytes_per_sample = 2;
    }

    /* Now data is either 8-bit unsigned or 16-bit signed */

    if (bits_per_sample == 8) {
        /* Convert unsigned to signed */
        convert_u8_to_s8(raw, total_frames * num_channels);

        /* Mix stereo to mono */
        u32 out_samples = total_frames;
        if (num_channels == 2) {
            out_samples = mix_stereo_8((s8 *)raw, total_frames * 2);
        }

        /* Shrink allocation to mono size */
        *out_data = raw;
        *out_len  = out_samples;
        *out_rate = sample_rate;
        *out_bits = 8;

    } else {
        /* 16-bit signed */
        u32 out_samples = total_frames;
        if (num_channels == 2) {
            out_samples = mix_stereo_16((s16 *)raw, total_frames * 2);
        }

        *out_data = raw;
        *out_len  = out_samples * 2;
        *out_rate = sample_rate;
        *out_bits = 16;
    }

    return WAV_OK;
}

/* X-list: keep in sync with error codes in wav_load.h. */
#define WAV_ERR_TABLE(X) \
    X(WAV_OK,              "OK") \
    X(WAV_ERR_OPEN,        "Could not open file") \
    X(WAV_ERR_NOT_RIFF,    "Not a RIFF file") \
    X(WAV_ERR_NOT_WAVE,    "Not a WAVE file") \
    X(WAV_ERR_NO_FMT,      "Missing fmt chunk") \
    X(WAV_ERR_NO_DATA,     "Missing data chunk") \
    X(WAV_ERR_UNSUPPORTED, "Unsupported WAV format") \
    X(WAV_ERR_ALLOC,       "Out of memory") \
    X(WAV_ERR_READ,        "File read error") \
    X(WAV_ERR_TOO_BIG,     "WAV too big for RAM")

const char *wav_strerror(int err)
{
    switch (err) {
#define X(code, msg) case code: return msg;
    WAV_ERR_TABLE(X)
#undef X
    default: return "Unknown error";
    }
}
