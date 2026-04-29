/*
 * wav_test.c — Host-native tests for wav_load.c.
 *
 * Verifies that wav_load correctly parses the bit depths and sample
 * rates that the SAMPLE view's LOAD action is expected to handle:
 *
 *   Bit depths: 8-bit unsigned PCM, 16-bit signed PCM, 24-bit signed
 *               PCM, 32-bit IEEE float (all quantized to 8- or 16-bit
 *               signed at the output).
 *   Rates:      8000, 16000, 32000, 44100, 48000 Hz.
 *   Channels:   mono and stereo (stereo is mixed to mono).
 *
 * Each test writes a synthetic WAV file (440 Hz sine, 0.1 s) to a
 * temporary path, calls wav_load, verifies the output fields, then
 * spot-checks the decoded sample range. Temp files are unlinked after
 * each test so the working directory stays clean on failure too.
 *
 * Build: make -C test wav_test
 * Run:   ./wav_test
 */

#include "wav_load.h"
#include "nds_shim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- minimal test harness (doesn't reuse MT_ASSERT so this file can
 *      run without pulling in the whole mt_run_tests machinery) ---- */

static int g_total  = 0;
static int g_failed = 0;

#define CHECK(cond, ...) do { \
    g_total++; \
    if (!(cond)) { \
        g_failed++; \
        fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* ---- WAV writer (pure byte builder; intentionally NOT wav_save.c so
 *      this test exercises wav_load against independent code) ---- */

static void put_u16(u8 *dst, u16 v)
{
    dst[0] = (u8)(v & 0xFF);
    dst[1] = (u8)(v >> 8);
}

static void put_u32(u8 *dst, u32 v)
{
    dst[0] = (u8)(v & 0xFF);
    dst[1] = (u8)((v >>  8) & 0xFF);
    dst[2] = (u8)((v >> 16) & 0xFF);
    dst[3] = (u8)((v >> 24) & 0xFF);
}

/*
 * Write a synthetic WAV file.
 *
 *   format_tag:        1 = PCM, 3 = IEEE float
 *   bits_per_sample:   8 / 16 / 24 for PCM; 32 for float
 *   num_channels:      1 or 2 (mono or interleaved stereo)
 *   sample_rate:       any rate in Hz
 *   num_frames:        count of frames (not samples!)
 *
 * Sample content: 440 Hz sine at ~-3 dBFS, amplitude chosen so even
 * stereo mixdown stays well below clipping. Left and right are
 * identical so mixing is straight averaging — simplifies verification.
 */
static int write_sine_wav(const char *path,
                          u16 format_tag, u16 bits_per_sample,
                          u16 num_channels, u32 sample_rate,
                          u32 num_frames)
{
    u16 bytes_per_sample = bits_per_sample / 8;
    u32 block_align      = bytes_per_sample * num_channels;
    u32 byte_rate        = sample_rate * block_align;
    u32 data_size        = num_frames * block_align;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    u8 hdr[44];
    memcpy(hdr +  0, "RIFF", 4);
    put_u32(hdr +  4, 36 + data_size);
    memcpy(hdr +  8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_u32(hdr + 16, 16);
    put_u16(hdr + 20, format_tag);
    put_u16(hdr + 22, num_channels);
    put_u32(hdr + 24, sample_rate);
    put_u32(hdr + 28, byte_rate);
    put_u16(hdr + 32, (u16)block_align);
    put_u16(hdr + 34, bits_per_sample);
    memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, data_size);
    fwrite(hdr, 1, 44, f);

    /* Generate identical sine on both channels. Keep peak at ~0.5 so
     * we can spot-check maxima without worrying about clipping. */
    const double twopi = 6.2831853071795864769;
    const double freq  = 440.0;
    const double amp   = 0.5;

    for (u32 i = 0; i < num_frames; i++) {
        double t = (double)i / (double)sample_rate;
        double v = amp * sin(twopi * freq * t);

        for (u16 c = 0; c < num_channels; c++) {
            if (format_tag == 1 && bits_per_sample == 8) {
                int s = (int)(v * 127.0) + 128;   /* unsigned PCM */
                if (s < 0) s = 0;
                if (s > 255) s = 255;
                u8 b = (u8)s;
                fwrite(&b, 1, 1, f);
            } else if (format_tag == 1 && bits_per_sample == 16) {
                int s = (int)(v * 32767.0);
                if (s < -32768) s = -32768;
                if (s >  32767) s =  32767;
                u8 b[2];
                put_u16(b, (u16)(s16)s);
                fwrite(b, 1, 2, f);
            } else if (format_tag == 1 && bits_per_sample == 24) {
                int s = (int)(v * 8388607.0);
                if (s < -8388608) s = -8388608;
                if (s >  8388607) s =  8388607;
                u8 b[3];
                b[0] = (u8)(s & 0xFF);
                b[1] = (u8)((s >>  8) & 0xFF);
                b[2] = (u8)((s >> 16) & 0xFF);
                fwrite(b, 1, 3, f);
            } else if (format_tag == 3 && bits_per_sample == 32) {
                float fv = (float)v;
                fwrite(&fv, sizeof(float), 1, f);
            }
        }
    }

    fclose(f);
    return 0;
}

/* ---- Assertions on a decoded buffer ---- */

static s32 peak_s16(const s16 *buf, u32 count)
{
    s32 peak = 0;
    for (u32 i = 0; i < count; i++) {
        s32 v = buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

static s32 peak_s8(const s8 *buf, u32 count)
{
    s32 peak = 0;
    for (u32 i = 0; i < count; i++) {
        s32 v = buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

/* ---- Individual test cases ---- */

static void run_format_rate_channel(const char *label,
                                    u16 format_tag, u16 bits_in,
                                    u16 channels,
                                    u32 rate,
                                    u8  expect_bits_out)
{
    const char *path = "wav_test_tmp.wav";
    const u32 frames = rate / 10;   /* 0.1 s, enough for ~44 cycles */

    if (write_sine_wav(path, format_tag, bits_in, channels, rate, frames) != 0) {
        fprintf(stderr, "  %s: could not create temp file\n", label);
        g_total++; g_failed++;
        return;
    }

    u8 *data = NULL;
    u32 len = 0, got_rate = 0;
    u8 got_bits = 0;
    int err = wav_load(path, &data, &len, &got_rate, &got_bits);

    CHECK(err == WAV_OK, "%s: wav_load err=%d (%s)",
          label, err, wav_strerror(err));
    if (err != WAV_OK) goto cleanup;

    CHECK(got_rate == rate, "%s: rate got %u expected %u",
          label, got_rate, rate);
    CHECK(got_bits == expect_bits_out,
          "%s: bits got %u expected %u",
          label, got_bits, expect_bits_out);

    /* Length should equal mono frame count (stereo mixed down). */
    u32 expected_samples = frames;
    u32 got_samples = (got_bits == 16) ? len / 2 : len;
    CHECK(got_samples == expected_samples,
          "%s: samples got %u expected %u",
          label, got_samples, expected_samples);

    /* Peak amplitude: the sine was written at ~0.5 full-scale. After
     * stereo mixdown (identical channels) it's still 0.5. Accept any
     * peak in [0.35, 0.65] of full-scale. */
    if (got_bits == 16) {
        s32 peak = peak_s16((const s16 *)data, got_samples);
        CHECK(peak > (s32)(0.35 * 32767) && peak < (s32)(0.65 * 32767),
              "%s: 16-bit peak %d outside expected band", label, peak);
    } else {
        /* wav_load returns 8-bit as signed (via convert_u8_to_s8). */
        s32 peak = peak_s8((const s8 *)data, got_samples);
        CHECK(peak > (s32)(0.35 * 127) && peak < (s32)(0.65 * 127),
              "%s: 8-bit peak %d outside expected band", label, peak);
    }

cleanup:
    if (data) free(data);
    unlink(path);
}

/* Separate negative-path tests: malformed files must return errors,
 * not crash. */
static void run_negative_tests(void)
{
    const char *path = "wav_test_tmp.wav";
    u8 *data = NULL;
    u32 len, rate;
    u8 bits;

    /* 1. non-existent file */
    int err = wav_load("definitely_not_a_file.wav", &data, &len, &rate, &bits);
    CHECK(err == WAV_ERR_OPEN, "missing file: err=%d", err);

    /* 2. not RIFF */
    FILE *f = fopen(path, "wb");
    fwrite("NOTRIFF0NOTWAVE ", 1, 16, f);
    fclose(f);
    err = wav_load(path, &data, &len, &rate, &bits);
    CHECK(err == WAV_ERR_NOT_RIFF, "not-RIFF: err=%d", err);
    unlink(path);

    /* 3. unsupported bit depth (12-bit PCM) */
    if (write_sine_wav(path, 1, 12, 1, 22050, 256) == 0) {
        err = wav_load(path, &data, &len, &rate, &bits);
        CHECK(err == WAV_ERR_UNSUPPORTED, "12-bit: err=%d", err);
        unlink(path);
    }
}

int main(void)
{
    printf("=== wav_load host tests ===\n");

    /* Bit-depth coverage at a single rate. */
    run_format_rate_channel("8-bit/mono/22050",   1,  8, 1, 22050,  8);
    run_format_rate_channel("16-bit/mono/22050",  1, 16, 1, 22050, 16);
    run_format_rate_channel("24-bit/mono/22050",  1, 24, 1, 22050, 16);
    run_format_rate_channel("float/mono/22050",   3, 32, 1, 22050, 16);

    /* Stereo mixdown. */
    run_format_rate_channel("8-bit/stereo/22050", 1,  8, 2, 22050,  8);
    run_format_rate_channel("16-bit/stereo/22050",1, 16, 2, 22050, 16);
    run_format_rate_channel("24-bit/stereo/22050",1, 24, 2, 22050, 16);
    run_format_rate_channel("float/stereo/22050", 3, 32, 2, 22050, 16);

    /* Rate coverage for the canonical 16-bit case. */
    run_format_rate_channel("16-bit/mono/8000",  1, 16, 1,  8000, 16);
    run_format_rate_channel("16-bit/mono/16000", 1, 16, 1, 16000, 16);
    run_format_rate_channel("16-bit/mono/32000", 1, 16, 1, 32000, 16);
    run_format_rate_channel("16-bit/mono/44100", 1, 16, 1, 44100, 16);
    run_format_rate_channel("16-bit/mono/48000", 1, 16, 1, 48000, 16);

    run_negative_tests();

    printf("--- %d/%d passed", g_total - g_failed, g_total);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf(" ---\n");
    return g_failed == 0 ? 0 : 1;
}
