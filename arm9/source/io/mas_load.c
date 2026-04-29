/*
 * mas_load.c -- Parse a .mas binary file into the MT_Song model.
 *
 * Pattern decompression implements MF flag carry-forward (bits 7-4 of
 * the mask byte).  Without this, most notes/instruments are silently
 * dropped because the value-cache compression suppresses repeated
 * values.  See mas_spec/patterns.md section 2.2 and D.1 errata.
 *
 * Reference implementation: mas2xm/src/mas_read.c (battle-tested
 * against 46 XM files in batch roundtrip tests).
 */

#include "mas_load.h"
#include "memtrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  MAS format constants                                               */
/* ------------------------------------------------------------------ */

#define MAS_TYPE_SONG       0
#define MAS_VERSION         0x18

/* Header flags */
#define MAS_FLAG_LINK_GXX   (1 << 0)
#define MAS_FLAG_FREQ_MODE  (1 << 2)
#define MAS_FLAG_XM_MODE    (1 << 3)
#define MAS_FLAG_OLD_MODE   (1 << 5)

/* Envelope flags in instrument env_flags byte */
/* env_flags bit layout (matches mmutil source/mas.h):
 *   bit 0 — volume envelope exists
 *   bit 1 — panning envelope exists
 *   bit 2 — pitch envelope exists
 *   bit 3 — volume envelope enabled
 *   bits 4-7 — unused (mmutil writes 0)
 *
 * Pan and pitch envelopes have no separate "enabled" bit in mmutil. The
 * playback engine applies them whenever the EXISTS bit is set, so on the
 * model side we represent their enabled state as "node_count > 0". */
#define MAS_ENV_VOL_EXISTS   (1 << 0)
#define MAS_ENV_PAN_EXISTS   (1 << 1)
#define MAS_ENV_PITCH_EXISTS (1 << 2)
#define MAS_ENV_VOL_ENABLED  (1 << 3)

/* Pattern compression flags (lower nibble of mask byte) */
#define COMPR_FLAG_NOTE  (1 << 0)
#define COMPR_FLAG_INSTR (1 << 1)
#define COMPR_FLAG_VOLC  (1 << 2)
#define COMPR_FLAG_EFFC  (1 << 3)

/* NDS sample repeat modes */
#define MM_SREPEAT_FORWARD 1
#define MM_SREPEAT_OFF     2

/* MAS header sizes */
#define MAS_PREFIX_SIZE    8
#define MAS_HEADER_SIZE    276   /* 12 + 32 + 32 + 200 */
#define MAS_MAX_CHANNELS   32
#define MAS_MAX_ORDERS     200

/* ------------------------------------------------------------------ */
/*  Buffer reader helpers (bounds-checked)                             */
/* ------------------------------------------------------------------ */

static u8 buf_u8(const u8 *buf, u32 size, u32 *pos)
{
    if (*pos >= size) return 0;
    return buf[(*pos)++];
}

static u16 buf_u16(const u8 *buf, u32 size, u32 *pos)
{
    u8 lo = buf_u8(buf, size, pos);
    u8 hi = buf_u8(buf, size, pos);
    return (u16)(lo | (hi << 8));
}

static u32 buf_u32(const u8 *buf, u32 size, u32 *pos)
{
    u16 lo = buf_u16(buf, size, pos);
    u16 hi = buf_u16(buf, size, pos);
    return (u32)(lo | (hi << 16));
}

/* ------------------------------------------------------------------ */
/*  Channel-count detection helper                                     */
/* ------------------------------------------------------------------ */

/*
 * After all patterns are loaded, scan for the highest channel index
 * that contains any non-empty data, then round up to a standard
 * channel count (4, 8, 16, 24, 32).
 */
static u8 detect_channel_count(const MT_Song *s)
{
    int highest = 0;

    for (int p = 0; p < s->patt_count; p++) {
        const MT_Pattern *pat = s->patterns[p];
        if (!pat) continue;

        for (int r = 0; r < pat->nrows; r++) {
            for (int c = MT_MAX_CHANNELS - 1; c >= highest; c--) {
                const MT_Cell *cell = MT_CELL(pat, r, c);
                if (cell->note != NOTE_EMPTY || cell->inst != 0 ||
                    cell->vol != 0 || cell->fx != 0 || cell->param != 0) {
                    highest = c + 1;  /* 1-based count */
                    break;
                }
            }
        }
    }

    /* Round up to standard sizes */
    if (highest <= 4)  return 4;
    if (highest <= 8)  return 8;
    if (highest <= 16) return 16;
    if (highest <= 24) return 24;
    return 32;
}

/* ------------------------------------------------------------------ */
/*  Instrument parsing                                                 */
/* ------------------------------------------------------------------ */

/*
 * Parse one envelope from the buffer into an MT_Envelope struct.
 *
 * MAS envelope encoding per node (4 bytes each):
 *   s16 delta   — slope from this node to the next (unused for reconstruction)
 *   u16 packed  — low 7 bits = base (y value), bits 7..15 = range (x distance to next node)
 *
 * Reconstruction:
 *   node[i].y = base_i
 *   node[0].x = 0
 *   node[i].x = node[i-1].x + range_{i-1}
 *
 * Returns 0 on success, -1 on bounds error.
 */
static int parse_envelope(const u8 *buf, u32 size, u32 *pos, MT_Envelope *env)
{
    /* mm_mas_envelope header is 8 bytes:
     *   byte 0: size (total envelope size in bytes, including header)
     *   byte 1: loop_start
     *   byte 2: loop_end
     *   byte 3: sus_start
     *   byte 4: sus_end
     *   byte 5: node_count
     *   byte 6: is_filter
     *   byte 7: wasted/padding
     */
    if (*pos + 8 > size) return -1;

    buf_u8(buf, size, pos);             /* size (used for skipping, we don't need it) */
    env->loop_start = buf_u8(buf, size, pos);
    env->loop_end   = buf_u8(buf, size, pos);
    env->sus_start  = buf_u8(buf, size, pos);
    env->sus_end    = buf_u8(buf, size, pos);
    env->node_count = buf_u8(buf, size, pos);
    buf_u8(buf, size, pos);             /* is_filter */
    buf_u8(buf, size, pos);             /* wasted */

    u8 count = env->node_count;
    if (count > MT_MAX_ENV_NODES)
        count = MT_MAX_ENV_NODES;

    if (*pos + (u32)count * 4 > size) return -1;

    u16 prev_range = 0;

    for (u8 i = 0; i < count; i++) {
        /* s16 delta (unused for reconstruction) */
        buf_u16(buf, size, pos);

        u16 packed = buf_u16(buf, size, pos);
        u8  base   = (u8)(packed & 0x7F);
        u16 range  = (packed >> 7) & 0x1FF;

        env->nodes[i].y = base;

        if (i == 0)
            env->nodes[i].x = 0;
        else
            env->nodes[i].x = env->nodes[i - 1].x + prev_range;

        prev_range = range;
    }

    /* enabled is set by the caller based on env_flags */
    return 0;
}

/*
 * Parse one instrument into an MT_Instrument struct.
 *
 * Reads the 12-byte fixed header, optional envelopes (vol/pan/pitch),
 * and the note-sample map (either compact single-sample or full 120-entry).
 */
static int parse_instrument(const u8 *buf, u32 size, u32 base_offset,
                            u32 instr_offset, MT_Instrument *ins)
{
    u32 pos = base_offset + instr_offset;

    if (pos + 12 > size) return -1;

    /* Fixed fields (12 bytes) */
    ins->global_volume = buf_u8(buf, size, &pos);
    ins->fadeout       = buf_u8(buf, size, &pos);
    ins->random_volume = buf_u8(buf, size, &pos);
    ins->dct           = buf_u8(buf, size, &pos);
    ins->nna           = buf_u8(buf, size, &pos);
    u8 env_flags       = buf_u8(buf, size, &pos);
    ins->panning       = buf_u8(buf, size, &pos);
    ins->dca           = buf_u8(buf, size, &pos);
    u16 notemap_flag   = buf_u16(buf, size, &pos);
    buf_u16(buf, size, &pos);  /* reserved */

    /* Parse envelopes based on env_flags */
    memset(&ins->env_vol,   0, sizeof(MT_Envelope));
    memset(&ins->env_pan,   0, sizeof(MT_Envelope));
    memset(&ins->env_pitch, 0, sizeof(MT_Envelope));

    if (env_flags & MAS_ENV_VOL_EXISTS) {
        if (parse_envelope(buf, size, &pos, &ins->env_vol) != 0)
            return -1;
    }
    ins->env_vol.enabled = (env_flags & MAS_ENV_VOL_ENABLED) != 0;

    if (env_flags & MAS_ENV_PAN_EXISTS) {
        if (parse_envelope(buf, size, &pos, &ins->env_pan) != 0)
            return -1;
    }
    ins->env_pan.enabled = (ins->env_pan.node_count > 0);

    if (env_flags & MAS_ENV_PITCH_EXISTS) {
        if (parse_envelope(buf, size, &pos, &ins->env_pitch) != 0)
            return -1;
    }
    ins->env_pitch.enabled = (ins->env_pitch.node_count > 0);

    /* Parse note-sample map */
    if (notemap_flag & 0x8000) {
        /* Compact form: bits 14-0 hold the sample index used for all 120
         * notes. The cast to u8 is bounded by MT_MAX_SAMPLES (currently 128)
         * but the mask must follow the spec so the intent is clear if the
         * sample limit is ever raised. */
        ins->has_full_notemap = false;
        ins->sample = (u8)(notemap_flag & 0x7FFF);
    } else {
        /* Full 120-entry note map */
        ins->has_full_notemap = true;
        if (pos + 120 * 2 > size) return -1;
        for (int n = 0; n < MT_MAX_NOTEMAP; n++)
            ins->notemap[n] = buf_u16(buf, size, &pos);
    }

    ins->active = true;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Sample parsing                                                     */
/* ------------------------------------------------------------------ */

/*
 * Parse one sample's info and inline NDS data into an MT_Sample struct.
 *
 * Reads the 12-byte sample info header, the 16-byte NDS sample header,
 * allocates and copies the PCM data.
 */
static int parse_sample(const u8 *buf, u32 size, u32 base_offset,
                        u32 samp_offset, MT_Sample *smp)
{
    u32 pos = base_offset + samp_offset;

    if (pos + 12 > size) return -1;

    /* Sample info (12 bytes) — field order matches mm_mas_sample_info struct */
    smp->default_volume = buf_u8(buf, size, &pos);
    smp->panning        = buf_u8(buf, size, &pos);
    u16 frequency       = buf_u16(buf, size, &pos);
    smp->vib_type       = buf_u8(buf, size, &pos);
    smp->vib_depth      = buf_u8(buf, size, &pos);
    smp->vib_speed      = buf_u8(buf, size, &pos);
    smp->global_volume  = buf_u8(buf, size, &pos);
    smp->vib_rate       = buf_u16(buf, size, &pos);
    /* msl_id (u16) — 0xFFFF = inline sample, else external */
    u16 msl_id          = buf_u16(buf, size, &pos);

    smp->base_freq = (u32)frequency * 4;  /* MAS stores freq/4 */

    if (msl_id != 0xFFFF) {
        /* External sample (MSL reference) — no inline PCM data */
        smp->active = false;
        return 0;
    }

    if (pos + 16 > size) return -1;

    /* Inline NDS sample header (16 bytes).
     *
     * mmutil encodes the second u32 as a UNION:
     *   - non-looping: total sample length (in words)
     *   - looping:     loop REGION length (in words), NOT total
     *
     * The first u32 (loop_start) is also in words, scaled by format:
     *   - 8-bit:  byte_offset / 4
     *   - 16-bit: byte_offset / 2
     */
    u32 field_start         = buf_u32(buf, size, &pos);  /* loop_start (words) */
    u32 field_length        = buf_u32(buf, size, &pos);  /* length or loop_length (words) */
    u8  format_byte         = buf_u8(buf, size, &pos);
    u8  loop_flag           = buf_u8(buf, size, &pos);
    buf_u16(buf, size, &pos);  /* sample_rate (already in base_freq) */
    buf_u32(buf, size, &pos);  /* reserved */

    smp->format    = format_byte;
    smp->bits      = format_byte ? 16 : 8;
    /* Use the repeat_mode flag directly — some samples have
     * MM_SREPEAT_FORWARD with loop_length=0 (loop to end, or ADPCM). */
    smp->loop_type = (loop_flag == MM_SREPEAT_FORWARD) ? 1 : 0;

    /* The DS hardware stores lengths/offsets in 32-bit words regardless of
     * sample format. word_mul is always 4 to convert words → bytes. */
    u32 bytes_per_sample = (smp->format == 1) ? 2 : 1;
    u32 word_mul = 4;

    if (loop_flag == MM_SREPEAT_FORWARD) {
        /* Looping: field_start = loop_start in words, field_length = loop region in words.
         * Total PCM = loop_start + loop_length (both in words → convert to samples). */
        u32 ls_bytes  = field_start  * word_mul;
        u32 ll_bytes  = field_length * word_mul;
        smp->loop_start  = ls_bytes / bytes_per_sample;
        smp->loop_length = ll_bytes / bytes_per_sample;
        smp->length      = smp->loop_start + smp->loop_length;
    } else {
        /* Non-looping: field_length = total length in format-dependent units.
         * 8-bit: units of 4 bytes (words).  16-bit: units of 2 bytes (halfwords). */
        u32 total_bytes = field_length * word_mul;
        smp->length      = total_bytes / bytes_per_sample;
        smp->loop_start  = 0;
        smp->loop_length = 0;
    }

    /* Allocate and copy PCM data */
    u32 total_pcm_bytes = smp->length * bytes_per_sample;
    if (total_pcm_bytes > 0) {
        u32 alloc_bytes = total_pcm_bytes;
        if (pos + alloc_bytes > size)
            return -1;

        smp->pcm_data = (u8 *)malloc(alloc_bytes);
        if (!smp->pcm_data) return -1;

        memcpy(smp->pcm_data, buf + pos, alloc_bytes);
        pos += alloc_bytes;

        /* Skip 4 bytes wraparound padding */
        u32 pad = 4;
        if (pos + pad <= size)
            pos += pad;
        else
            pos = size;
    } else {
        smp->pcm_data = NULL;
    }

    /* Align pos to 4 */
    pos = (pos + 3) & ~3u;

    smp->active = true;
    smp->drawn  = false;
    smp->name[0] = '\0';

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Pattern decompression                                              */
/* ------------------------------------------------------------------ */

/*
 * Decompress one MAS pattern into an MT_Pattern.
 *
 * This is the critical function.  It implements the full IT-style RLE
 * decompression with MF flag carry-forward, exactly matching the
 * logic in mas2xm/src/mas_read.c read_pattern().
 *
 * Key insight:  mask bits 7-4 (MF flags) indicate that a field is
 * semantically active even though its value was suppressed by the
 * per-channel value cache.  When MF bit is set but the corresponding
 * COMPR_FLAG is clear, carry forward the last value for that channel.
 * Without this, most notes/instruments after the first row are lost.
 */
static int parse_pattern(const u8 *buf, u32 size, u32 base_offset,
                         u32 patt_offset, MT_Pattern *pat, bool xm_mode)
{
    u32 pos = base_offset + patt_offset;

    /* Pattern header: 1 byte, value = nrows - 1 */
    pat->nrows = buf_u8(buf, size, &pos) + 1;

    u8 empty_vol = xm_mode ? 0 : 0xFF;

    /* Initialize all cells to empty */
    for (int r = 0; r < pat->nrows; r++) {
        for (int c = 0; c < pat->ncols; c++) {
            MT_Cell *cell = MT_CELL(pat, r, c);
            cell->note  = NOTE_EMPTY;
            cell->inst  = 0;
            cell->vol   = empty_vol;
            cell->fx    = 0;
            cell->param = 0;
        }
    }

    /* Per-channel state for mask reuse and value carry-forward */
    u8 last_mask[MAS_MAX_CHANNELS];
    u8 last_note[MAS_MAX_CHANNELS];
    u8 last_inst[MAS_MAX_CHANNELS];
    u8 last_vol[MAS_MAX_CHANNELS];
    u8 last_fx[MAS_MAX_CHANNELS];
    u8 last_param[MAS_MAX_CHANNELS];

    memset(last_mask,  0,          sizeof(last_mask));
    memset(last_note,  NOTE_EMPTY, sizeof(last_note));
    memset(last_inst,  0,          sizeof(last_inst));
    memset(last_vol,   empty_vol,  sizeof(last_vol));
    memset(last_fx,    0,          sizeof(last_fx));
    memset(last_param, 0,          sizeof(last_param));

    for (int row = 0; row < pat->nrows; row++) {
        for (;;) {
            u8 byte = buf_u8(buf, size, &pos);
            u8 chan = byte & 0x7F;

            if (chan == 0)
                break;  /* end of row */

            chan -= 1;  /* convert to 0-based */
            if (chan >= MAS_MAX_CHANNELS || chan >= pat->ncols)
                break;  /* safety: don't write beyond allocated columns */

            /* Mask byte: new if bit 7 set, else reuse last for channel */
            u8 mask;
            if (byte & 0x80) {
                mask = buf_u8(buf, size, &pos);
                last_mask[chan] = mask;
            } else {
                mask = last_mask[chan];
            }

            MT_Cell *cell = MT_CELL(pat, row, chan);

            /* MF flags in upper nibble */
            u8 mf = mask >> 4;

            /* ---- Note ---- */
            if (mask & COMPR_FLAG_NOTE) {
                u8 n = buf_u8(buf, size, &pos);
                cell->note = n;
                last_note[chan] = n;
            } else if (mf & 0x01) {
                /* MF_START: note cached, carry forward last value */
                cell->note = last_note[chan];
            }

            /* ---- Instrument ---- */
            if (mask & COMPR_FLAG_INSTR) {
                u8 i = buf_u8(buf, size, &pos);
                cell->inst = i;
                last_inst[chan] = i;
            } else if (mf & 0x02) {
                /* MF_DVOL: instrument cached, carry forward */
                cell->inst = last_inst[chan];
            }

            /* ---- Volume command ---- */
            if (mask & COMPR_FLAG_VOLC) {
                u8 v = buf_u8(buf, size, &pos);
                cell->vol = v;
                last_vol[chan] = v;
            } else if (mf & 0x04) {
                /* MF_HASVCMD: volume cached, carry forward */
                cell->vol = last_vol[chan];
            }

            /* ---- Effect + parameter ---- */
            if (mask & COMPR_FLAG_EFFC) {
                u8 fx = buf_u8(buf, size, &pos);
                u8 pm = buf_u8(buf, size, &pos);
                cell->fx    = fx;
                cell->param = pm;
                last_fx[chan]    = fx;
                last_param[chan] = pm;
            } else if (mf & 0x08) {
                /* MF_HASFX: effect cached, carry forward */
                cell->fx    = last_fx[chan];
                cell->param = last_param[chan];
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main loader                                                        */
/* ------------------------------------------------------------------ */

int mas_load(const char *path, MT_Song *s)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_len < (long)(MAS_PREFIX_SIZE + MAS_HEADER_SIZE)) {
        fclose(f);
        return -2;
    }

    u32 file_size = (u32)file_len;

    /* Read entire file into a temporary buffer */
    u8 *buf = (u8 *)malloc(file_size);
    if (!buf) {
        fclose(f);
        return -4;
    }

    if (fread(buf, 1, file_size, f) != file_size) {
        free(buf);
        fclose(f);
        return -2;
    }
    fclose(f);

    /* ---- MAS prefix (8 bytes) ---- */
    u32 pos = 0;
    /* u32 body_size = */ buf_u32(buf, file_size, &pos);
    u8 type    = buf_u8(buf, file_size, &pos);
    /* u8 version = */ buf_u8(buf, file_size, &pos);
    buf_u8(buf, file_size, &pos);  /* reserved */
    buf_u8(buf, file_size, &pos);  /* reserved */

    if (type != MAS_TYPE_SONG) {
        free(buf);
        return -3;
    }

    /* ---- Reset the song model ---- */
    song_free();
    memset(s, 0, sizeof(MT_Song));

    /*
     * base_offset: all internal offsets in the MAS file are relative
     * to this position (the start of the module header, right after
     * the 8-byte prefix).
     */
    u32 base_offset = MAS_PREFIX_SIZE;

    /* ---- Module header (276 bytes) ---- */
    pos = base_offset;

    u8 order_count = buf_u8(buf, file_size, &pos);
    u8 inst_count  = buf_u8(buf, file_size, &pos);
    u8 samp_count  = buf_u8(buf, file_size, &pos);
    u8 patt_count  = buf_u8(buf, file_size, &pos);
    u8 flags       = buf_u8(buf, file_size, &pos);

    s->link_gxx     = (flags & MAS_FLAG_LINK_GXX)   ? true : false;
    s->old_effects  = (flags & (1 << 1))           ? true : false;
    s->freq_linear  = (flags & MAS_FLAG_FREQ_MODE) ? true : false;
    s->xm_mode      = (flags & MAS_FLAG_XM_MODE)   ? true : false;
    s->old_mode     = (flags & MAS_FLAG_OLD_MODE)   ? true : false;

    s->global_volume   = buf_u8(buf, file_size, &pos);
    s->initial_speed   = buf_u8(buf, file_size, &pos);
    s->initial_tempo   = buf_u8(buf, file_size, &pos);
    s->repeat_position = buf_u8(buf, file_size, &pos);

    /* 3 reserved bytes */
    buf_u8(buf, file_size, &pos);
    buf_u8(buf, file_size, &pos);
    buf_u8(buf, file_size, &pos);

    /* Channel volumes (32 bytes) */
    for (int i = 0; i < MAS_MAX_CHANNELS; i++)
        s->channel_volume[i] = buf_u8(buf, file_size, &pos);

    /* Channel panning (32 bytes) */
    for (int i = 0; i < MAS_MAX_CHANNELS; i++)
        s->channel_panning[i] = buf_u8(buf, file_size, &pos);

    /* Pattern order table (200 bytes) */
    s->order_count = order_count;
    for (int i = 0; i < MAS_MAX_ORDERS; i++)
        s->orders[i] = buf_u8(buf, file_size, &pos);

    /* Flags -> MT_Song fields */
    s->freq_linear = (flags & MAS_FLAG_FREQ_MODE) != 0;
    s->xm_mode     = (flags & MAS_FLAG_XM_MODE) != 0;

    /* Store counts */
    s->inst_count = inst_count;
    s->samp_count = samp_count;
    s->patt_count = patt_count;

    s->name[0] = '\0';  /* MAS has no song name */

    /* ---- Offset tables ---- */
    /* pos is now at base_offset + 276 */

    /* ---- Pre-flight memory check ----
     *
     * mt_mem_estimate_mas is a conservative upper bound on how much
     * RAM the decoded song will occupy. If we don't have the budget,
     * abort up-front rather than do a partial load that leaves the
     * user with half a song and a cryptic "LOW RAM" message. The
     * caller maps return code -6 to "file too large for RAM".
     *
     * Mid-load allocation failures (pattern alloc that the pre-flight
     * missed) still fall through to the `mem_warning` path below and
     * return rc=1 — a user-visible "partial load" notice. */
    bool mem_warning = false;
    {
        u32 estimated = mt_mem_estimate_mas(patt_count, samp_count, file_size);
        if (!mt_mem_check(estimated)) {
            free(buf);
            return -6;
        }
    }

    u32 *instr_offsets = NULL;
    u32 *samp_offsets  = NULL;
    u32 *patt_offsets  = NULL;

    if (inst_count > 0) {
        instr_offsets = (u32 *)malloc(inst_count * sizeof(u32));
        if (!instr_offsets) { free(buf); return -4; }
        for (int i = 0; i < inst_count; i++)
            instr_offsets[i] = buf_u32(buf, file_size, &pos);
    }

    if (samp_count > 0) {
        samp_offsets = (u32 *)malloc(samp_count * sizeof(u32));
        if (!samp_offsets) { free(instr_offsets); free(buf); return -4; }
        for (int i = 0; i < samp_count; i++)
            samp_offsets[i] = buf_u32(buf, file_size, &pos);
    }

    if (patt_count > 0) {
        patt_offsets = (u32 *)malloc(patt_count * sizeof(u32));
        if (!patt_offsets) { free(samp_offsets); free(instr_offsets); free(buf); return -4; }
        for (int i = 0; i < patt_count; i++)
            patt_offsets[i] = buf_u32(buf, file_size, &pos);
    }

    /* ---- Instruments ---- */
    for (int i = 0; i < inst_count; i++) {
        if (parse_instrument(buf, file_size, base_offset,
                             instr_offsets[i], &s->instruments[i]) != 0) {
            free(patt_offsets); free(samp_offsets); free(instr_offsets);
            free(buf);
            song_free();
            return -5;
        }
    }

    /* ---- Samples ---- */
    for (int i = 0; i < samp_count; i++) {
        if (parse_sample(buf, file_size, base_offset,
                         samp_offsets[i], &s->samples[i]) != 0) {
            free(patt_offsets); free(samp_offsets); free(instr_offsets);
            free(buf);
            song_free();
            return -5;
        }
    }

    /* ---- Channel count (must be set before pattern allocation) ---- */
    /* Always use 32 channels — the user may want to add more channels,
     * and external MAS files may have data in any of the 32 channels. */
    s->channel_count = MT_MAX_CHANNELS;

    /* ---- Patterns ---- */
    bool xm_mode = (flags & MAS_FLAG_XM_MODE) != 0;

    for (int i = 0; i < patt_count; i++) {
        /* Pre-read the row count from the pattern data (first byte = nrows-1) */
        u32 patt_pos = base_offset + patt_offsets[i];
        u16 actual_nrows = 64;
        if (patt_pos < file_size)
            actual_nrows = (u16)buf[patt_pos] + 1;

        MT_Pattern *pat = song_alloc_pattern((u8)i, actual_nrows, s->channel_count);
        if (!pat) {
            /* Out of memory — stop loading patterns but keep what we have */
            mem_warning = true;
            break;
        }

        if (parse_pattern(buf, file_size, base_offset, patt_offsets[i],
                          pat, xm_mode) != 0) {
            /* Parse error on this pattern — skip it but continue */
            mem_warning = true;
        }
    }

    /* ---- Cleanup ---- */
    free(patt_offsets);
    free(samp_offsets);
    free(instr_offsets);
    free(buf);

    /* Return 1 for "loaded with warnings" (some patterns/samples may be missing) */
    return mem_warning ? 1 : 0;
}
