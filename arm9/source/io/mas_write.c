/*
 * mas_write.c -- Serialize MT_Song to .mas binary format.
 *
 * Produces files playable by stock maxmod on Nintendo DS.
 * Pattern RLE compression follows mmutil's Write_Pattern() exactly,
 * including value caching and MF flag carry-forward.
 *
 * Instruments are serialized with envelope delta/base/range encoding
 * (ported from mmutil's Write_Instrument_Envelope).
 * Samples include full PCM data with NDS alignment and wraparound.
 */

#include "mas_write.h"
#include "../core/util.h"
#include <mm_mas.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define MAS_VERSION         0x18
#define MAS_TYPE_SONG       0
#define BYTESMASHER         0xBA

#define MAS_HEADER_SIZE     276   /* 12 + 32 + 32 + 200 */
#define MAS_PREFIX_SIZE     8

/* Pattern compression flags (lower nibble of mask byte) */
#define COMPR_FLAG_NOTE     (1 << 0)
#define COMPR_FLAG_INSTR    (1 << 1)
#define COMPR_FLAG_VOLC     (1 << 2)
#define COMPR_FLAG_EFFC     (1 << 3)

/* MF flags (upper nibble of mask byte, pre-shifted) */
#define MF_START            (1 << 4)
#define MF_DVOL             (2 << 4)
#define MF_HASVCMD          (4 << 4)
#define MF_HASFX            (8 << 4)

/* Empty volume sentinel -- XM mode uses 0, IT mode uses 0xFF.
 * maxtracker is XM-mode by default. */
#define EMPTY_VOL_XM        0
#define EMPTY_VOL_IT        0xFF

/* Serialization buffer size (256 KB). */
#define WRITE_BUF_SIZE      (256 * 1024)

/* ------------------------------------------------------------------ */
/*  Write-buffer helpers                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    u8  *buf;       /* base pointer (malloc'd) */
    u32  pos;       /* current write position   */
    u32  cap;       /* allocated capacity        */
} WBuf;

static inline void wb_init(WBuf *wb, u8 *buf, u32 cap)
{
    wb->buf = buf;
    wb->pos = 0;
    wb->cap = cap;
}

static inline void wb_w8(WBuf *wb, u8 v)
{
    if (wb->pos < wb->cap)
        wb->buf[wb->pos] = v;
    wb->pos++;
}

static inline void wb_w16(WBuf *wb, u16 v)
{
    wb_w8(wb, (u8)(v & 0xFF));
    wb_w8(wb, (u8)(v >> 8));
}

static inline void wb_w32(WBuf *wb, u32 v)
{
    wb_w8(wb, (u8)(v & 0xFF));
    wb_w8(wb, (u8)((v >> 8) & 0xFF));
    wb_w8(wb, (u8)((v >> 16) & 0xFF));
    wb_w8(wb, (u8)(v >> 24));
}

/* Overwrite a u32 at an absolute position (for back-patching). */
static inline void wb_patch32(WBuf *wb, u32 offset, u32 v)
{
    if (offset + 4 <= wb->cap) {
        wb->buf[offset + 0] = (u8)(v & 0xFF);
        wb->buf[offset + 1] = (u8)((v >> 8) & 0xFF);
        wb->buf[offset + 2] = (u8)((v >> 16) & 0xFF);
        wb->buf[offset + 3] = (u8)(v >> 24);
    }
}

/* Align write position up to 4-byte boundary. */
static inline void wb_align32(WBuf *wb)
{
    while (wb->pos & 3)
        wb_w8(wb, BYTESMASHER);
}

/* ------------------------------------------------------------------ */
/*  Mark pattern rows that need cache reset                            */
/*  (row 0 + targets of Cxx / SBx effects)                            */
/* ------------------------------------------------------------------ */

/* Per-pattern marks stored as a flat bool array on the stack. */
typedef struct {
    bool marks[MT_MAX_ROWS];
} PatternMarks;

/*
 * Scan all orders/patterns for Cxx (pattern break, effect 3 in IT
 * numbering) and SBx (pattern loop, effect 19 param 0xB0) to mark
 * rows where the value cache must be reset.
 */
static void mark_patterns(const MT_Song *song,
                          PatternMarks   marks[/*patt_count*/])
{
    /* Clear all marks and set row 0 for every pattern. */
    for (int p = 0; p < song->patt_count; p++) {
        memset(marks[p].marks, 0, sizeof(marks[p].marks));
        marks[p].marks[0] = true;
    }

    for (int o = 0; o < song->order_count; o++) {
        u8 ordval = song->orders[o];
        if (ordval == 255) break;
        if (ordval >= 254) continue;
        if (ordval >= song->patt_count) continue;

        const MT_Pattern *pat = song->patterns[ordval];
        if (!pat) continue;

        for (int row = 0; row < pat->nrows; row++) {
            for (int ch = 0; ch < MT_MAX_CHANNELS; ch++) {
                const MT_Cell *cell = MT_CELL(pat, row, ch);

                if (cell->fx == 3 && cell->param != 0) {
                    /* Pattern break to row cell->param in next order. */
                    int next_o = o + 1;
                    if (next_o < song->order_count) {
                        u8 nxt = song->orders[next_o];
                        if (nxt == 255) nxt = song->orders[0];
                        while (nxt == 254 && next_o + 1 < song->order_count) {
                            next_o++;
                            nxt = song->orders[next_o];
                        }
                        if (nxt < song->patt_count && cell->param < MT_MAX_ROWS) {
                            marks[nxt].marks[cell->param] = true;
                        }
                    }
                } else if (cell->fx == 19 && cell->param == 0xB0) {
                    /* Pattern loop start -- mark this row. */
                    marks[ordval].marks[row] = true;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Pattern RLE compressor                                             */
/*                                                                     */
/*  Faithfully reproduces mmutil Write_Pattern() logic:                */
/*   - Per-channel caching of mask, note, inst, vol, fx, param        */
/*   - Cache reset on marked rows                                      */
/*   - MF flags in upper nibble of mask byte                           */
/*   - Note-off/cut (>250) clear MF_START and MF_DVOL, and            */
/*     reset note cache to 256 to prevent false matches                */
/* ------------------------------------------------------------------ */

static void write_pattern(WBuf *wb, const MT_Pattern *pat,
                          const PatternMarks *pm, bool xm_vol)
{
    u16 last_mask[MT_MAX_CHANNELS];
    u16 last_note[MT_MAX_CHANNELS];
    u16 last_inst[MT_MAX_CHANNELS];
    u16 last_vol [MT_MAX_CHANNELS];
    u16 last_fx  [MT_MAX_CHANNELS];
    u16 last_param[MT_MAX_CHANNELS];

    u8 emptyvol = xm_vol ? EMPTY_VOL_XM : EMPTY_VOL_IT;

    /* Write pattern header: row count minus 1. */
    wb_w8(wb, (u8)(pat->nrows - 1));

    for (int row = 0; row < pat->nrows; row++) {

        /* Reset caches on marked rows. */
        if (pm->marks[row]) {
            for (int c = 0; c < MT_MAX_CHANNELS; c++) {
                last_mask[c]  = 256;
                last_note[c]  = 256;
                last_inst[c]  = 256;
                last_vol[c]   = 256;
                last_fx[c]    = 256;
                last_param[c] = 256;
            }
        }

        for (int col = 0; col < MT_MAX_CHANNELS; col++) {
            const MT_Cell *pe = MT_CELL(pat, row, col);

            /* Skip completely empty cells. */
            if (pe->note == NOTE_EMPTY && pe->inst == 0 &&
                pe->vol == emptyvol && pe->fx == 0 && pe->param == 0)
                continue;

            u8 maskvar = 0;
            u8 chanvar = (u8)(col + 1); /* 1-based channel */

            /* --- Determine which fields are semantically present --- */

            if (pe->note != NOTE_EMPTY)
                maskvar |= COMPR_FLAG_NOTE | MF_START;

            if (pe->inst != 0)
                maskvar |= COMPR_FLAG_INSTR | MF_DVOL;

            /* Note-off / note-cut: clear start + default-volume flags. */
            if (pe->note > NOTE_EMPTY)  /* 254 or 255 */
                maskvar &= ~(MF_START | MF_DVOL);

            if (pe->vol != emptyvol)
                maskvar |= COMPR_FLAG_VOLC | MF_HASVCMD;

            if (pe->fx != 0 || pe->param != 0)
                maskvar |= COMPR_FLAG_EFFC | MF_HASFX;

            /* --- Value caching: suppress fields matching last value --- */

            if (maskvar & COMPR_FLAG_NOTE) {
                if (pe->note == last_note[col]) {
                    maskvar &= ~COMPR_FLAG_NOTE;
                } else {
                    last_note[col] = pe->note;
                    /* Prevent note-off/cut from caching (force re-emit). */
                    if (pe->note == NOTE_CUT || pe->note == NOTE_OFF)
                        last_note[col] = 256;
                }
            }

            if (maskvar & COMPR_FLAG_INSTR) {
                if (pe->inst == last_inst[col]) {
                    maskvar &= ~COMPR_FLAG_INSTR;
                } else {
                    last_inst[col] = pe->inst;
                }
            }

            if (maskvar & COMPR_FLAG_VOLC) {
                if (pe->vol == last_vol[col]) {
                    maskvar &= ~COMPR_FLAG_VOLC;
                } else {
                    last_vol[col] = pe->vol;
                }
            }

            if (maskvar & COMPR_FLAG_EFFC) {
                if (pe->fx == last_fx[col] && pe->param == last_param[col]) {
                    maskvar &= ~COMPR_FLAG_EFFC;
                } else {
                    last_fx[col]    = pe->fx;
                    last_param[col] = pe->param;
                }
            }

            /* --- Mask byte caching --- */

            if (maskvar != last_mask[col]) {
                chanvar |= 0x80;            /* bit 7 = new mask follows */
                last_mask[col] = maskvar;
            }

            /* --- Emit bytes --- */

            wb_w8(wb, chanvar);

            if (chanvar & 0x80)
                wb_w8(wb, maskvar);

            if (maskvar & COMPR_FLAG_NOTE)
                wb_w8(wb, pe->note);
            if (maskvar & COMPR_FLAG_INSTR)
                wb_w8(wb, pe->inst);
            if (maskvar & COMPR_FLAG_VOLC)
                wb_w8(wb, pe->vol);
            if (maskvar & COMPR_FLAG_EFFC) {
                wb_w8(wb, pe->fx);
                wb_w8(wb, pe->param);
            }
        }

        /* End-of-row marker. */
        wb_w8(wb, 0x00);
    }
}

/* ------------------------------------------------------------------ */
/*  Write envelope (delta/base/range encoding)                         */
/* ------------------------------------------------------------------ */

static void write_envelope(WBuf *wb, const MT_Envelope *env)
{
    /* mm_mas_envelope header: 8 bytes
     * byte 0: size (total envelope size = 8 + node_count * 4)
     * byte 1: loop_start
     * byte 2: loop_end
     * byte 3: sus_start
     * byte 4: sus_end
     * byte 5: node_count
     * byte 6: is_filter (0)
     * byte 7: wasted (0)
     */
    u8 env_size = 8 + env->node_count * 4;
    wb_w8(wb, env_size);
    wb_w8(wb, env->loop_start);
    wb_w8(wb, env->loop_end);
    wb_w8(wb, env->sus_start);
    wb_w8(wb, env->sus_end);
    wb_w8(wb, env->node_count);
    wb_w8(wb, 0);  /* is_filter */
    wb_w8(wb, 0);  /* wasted */

    for (int i = 0; i < env->node_count; i++) {
        s32 base = env->nodes[i].y;
        s32 delta, range;

        if (i < env->node_count - 1) {
            range = clamp_s32((s32)env->nodes[i + 1].x - (s32)env->nodes[i].x, 1, 511);
            s32 dy = (s32)env->nodes[i + 1].y - base;
            delta = clamp_s32((dy * 512 + range / 2) / range, -32768, 32767);

            /* Overflow correction (use >> 9 to match MAS playback) */
            while ((base + ((delta * range) >> 9)) > 64) delta--;
            while ((base + ((delta * range) >> 9)) < 0)  delta++;
        } else {
            range = 0;
            delta = 0;
        }

        wb_w16(wb, (u16)(s16)delta);
        wb_w16(wb, (u16)(base | (range << 7)));
    }
}

/* ------------------------------------------------------------------ */
/*  Write instrument                                                   */
/* ------------------------------------------------------------------ */

static void write_instrument(WBuf *wb, const MT_Instrument *ins)
{
    /* Build env_flags. Matches mmutil's bit layout: bits 0-2 are EXISTS
     * flags, bit 3 is the volume envelope's enabled flag, bits 4-7 are
     * unused. Pan and pitch envelopes have no separate enabled bit — they
     * play whenever the EXISTS bit is set. */
    u8 env_flags = 0;
    if (ins->env_vol.node_count > 0)   env_flags |= (1 << 0);
    if (ins->env_pan.node_count > 0)   env_flags |= (1 << 1);
    if (ins->env_pitch.node_count > 0) env_flags |= (1 << 2);
    if (ins->env_vol.enabled)          env_flags |= (1 << 3);

    /* Fixed fields (12 bytes) */
    wb_w8(wb, ins->global_volume);
    wb_w8(wb, ins->fadeout);
    wb_w8(wb, ins->random_volume);
    wb_w8(wb, ins->dct);
    wb_w8(wb, ins->nna);
    wb_w8(wb, env_flags);
    wb_w8(wb, ins->panning);
    wb_w8(wb, ins->dca);

    /* note_map_offset: engine reads the notemap at instrument_base + offset.
     * Layout: [12-byte header] [envelopes...] [notemap if present]
     * So offset = 12 + total envelope sizes. */
    u16 notemap_flag;
    if (!ins->has_full_notemap) {
        notemap_flag = 0x8000 | (ins->sample & 0xFF);
    } else {
        u16 offset = 12; /* sizeof(mm_mas_instrument) fixed header */
        if (ins->env_vol.node_count > 0)
            offset += 8 + ins->env_vol.node_count * 4;
        if (ins->env_pan.node_count > 0)
            offset += 8 + ins->env_pan.node_count * 4;
        if (ins->env_pitch.node_count > 0)
            offset += 8 + ins->env_pitch.node_count * 4;
        notemap_flag = offset; /* is_note_map_invalid=0 (bit 15 clear) */
    }
    wb_w16(wb, notemap_flag);
    wb_w16(wb, 0);  /* reserved */

    /* Envelopes (only those that exist) */
    if (ins->env_vol.node_count > 0)
        write_envelope(wb, &ins->env_vol);
    if (ins->env_pan.node_count > 0)
        write_envelope(wb, &ins->env_pan);
    if (ins->env_pitch.node_count > 0)
        write_envelope(wb, &ins->env_pitch);

    /* Note map (only if full map) */
    if (ins->has_full_notemap) {
        for (int i = 0; i < MT_MAX_NOTEMAP; i++)
            wb_w16(wb, ins->notemap[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Write sample                                                       */
/* ------------------------------------------------------------------ */

static void write_sample(WBuf *wb, const MT_Sample *smp)
{
    u8 bytes_per_sample = (smp->format == 1) ? 2 : 1;

    /* Sample info (12 bytes) — field order matches mm_mas_sample_info */
    wb_w8(wb, smp->default_volume);
    wb_w8(wb, smp->panning);
    wb_w16(wb, (u16)(smp->base_freq / 4));
    wb_w8(wb, smp->vib_type);
    wb_w8(wb, smp->vib_depth);
    wb_w8(wb, smp->vib_speed);
    wb_w8(wb, smp->global_volume);
    wb_w16(wb, smp->vib_rate);
    wb_w16(wb, 0xFFFF);  /* msl_id = inline sample */

    /* NDS sample header (16 bytes).
     * Must match mmutil encoding:
     *   loop_start:  in words (byte_offset / word_div)
     *   length:      non-looping = total length in words
     *                looping     = loop REGION length in words
     *   default_frequency: (base_freq * 1024 + 16384) / 32768
     * Word unit: 4 bytes for 8-bit, 2 bytes for 16-bit. */
    /* DS hardware always uses 32-bit words for length/offset fields */
    u32 word_div = 4;
    u8  format_byte = smp->format;
    u8  loop_mode = (smp->loop_type == 1) ? MM_SREPEAT_FORWARD : MM_SREPEAT_OFF;
    u16 ds_freq = (u16)(((u32)smp->base_freq * 1024 + 16384) / 32768);

    if (smp->loop_type == 1) {
        u32 ls_bytes = smp->loop_start * bytes_per_sample;
        u32 ll_bytes = smp->loop_length * bytes_per_sample;
        wb_w32(wb, ls_bytes / word_div);    /* loop_start in words */
        wb_w32(wb, ll_bytes / word_div);    /* loop_length in words */
    } else {
        wb_w32(wb, 0);
        u32 total_bytes = smp->length * bytes_per_sample;
        wb_w32(wb, (total_bytes + (word_div - 1)) / word_div); /* total length in words */
    }
    wb_w8(wb, format_byte);
    wb_w8(wb, loop_mode);
    wb_w16(wb, ds_freq);
    wb_w32(wb, 0);  /* reserved (runtime pointer) */

    /* PCM data */
    if (smp->active && smp->pcm_data) {
        u32 data_bytes = smp->length * bytes_per_sample;

        /* Write PCM data */
        if (wb->pos + data_bytes <= wb->cap) {
            memcpy(wb->buf + wb->pos, smp->pcm_data, data_bytes);
            wb->pos += data_bytes;
        } else {
            wb->pos += data_bytes; /* overflow tracking */
        }

        /* Pad to 4-byte alignment */
        while (data_bytes & 3) {
            wb_w8(wb, 0);
            data_bytes++;
        }

        /* 4 bytes of wraparound padding.
         * For looping samples: copy from loop start for smooth interpolation.
         * For non-looping: zeros. */
        if (smp->loop_length > 0) {
            u32 ls_wrap = smp->loop_start * bytes_per_sample;
            u32 pcm_total = smp->length * bytes_per_sample;
            for (int j = 0; j < 4; j++) {
                if (ls_wrap + (u32)j < pcm_total)
                    wb_w8(wb, smp->pcm_data[ls_wrap + j]);
                else
                    wb_w8(wb, 0);
            }
        } else {
            wb_w32(wb, 0);
        }
    } else {
        /* No active sample data -- write 4 bytes of silence */
        wb_w32(wb, 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int mas_write(const char *path, const MT_Song *song)
{
    /* Estimate buffer size: base overhead + PCM data for all samples. */
    u32 buf_size = WRITE_BUF_SIZE;
    for (int i = 0; i < song->samp_count; i++) {
        const MT_Sample *s = &song->samples[i];
        if (s->active && s->pcm_data) {
            u32 bps = (s->format == 1) ? 2 : 1;
            buf_size += (s->length * bps + 7) & ~3u; /* data + align + wrap */
        }
    }

    u8 *buf = (u8 *)malloc(buf_size);
    if (!buf) return -2;

    WBuf wb;
    wb_init(&wb, buf, buf_size);

    /* Effective counts -- use the song's values.
     * If inst/samp are 0 the file still needs to be structurally valid. */
    u8 inst_count = song->inst_count;
    u8 samp_count = song->samp_count;
    u8 patt_count = song->patt_count;

    /* =============================================================
     * 1. MAS Prefix (8 bytes)
     * ============================================================= */
    u32 prefix_pos = wb.pos;        /* position of the size field */
    wb_w32(&wb, 0);                 /* size -- back-patched later */
    wb_w8(&wb, MAS_TYPE_SONG);      /* type  */
    wb_w8(&wb, MAS_VERSION);        /* version */
    wb_w8(&wb, BYTESMASHER);        /* reserved */
    wb_w8(&wb, BYTESMASHER);        /* reserved */

    u32 module_base = wb.pos;       /* offsets are relative to here */

    /* =============================================================
     * 2. Module Header (276 bytes)
     * ============================================================= */

    /* Fixed fields (12 bytes) */
    wb_w8(&wb, song->order_count);
    wb_w8(&wb, inst_count);
    wb_w8(&wb, samp_count);
    wb_w8(&wb, patt_count);

    /* Flags (must match MAS_HEADER_FLAG_* in mm_mas.h) */
    u8 flags = 0;
    if (song->link_gxx)     flags |= (1 << 0);
    if (song->old_effects)  flags |= (1 << 1);
    if (song->freq_linear)  flags |= (1 << 2);
    if (song->xm_mode)      flags |= (1 << 3);
    if (song->old_mode)     flags |= (1 << 5);
    wb_w8(&wb, flags);

    wb_w8(&wb, song->global_volume);
    wb_w8(&wb, song->initial_speed);
    wb_w8(&wb, song->initial_tempo);
    wb_w8(&wb, song->repeat_position);

    /* Reserved (3 bytes) */
    wb_w8(&wb, BYTESMASHER);
    wb_w8(&wb, BYTESMASHER);
    wb_w8(&wb, BYTESMASHER);

    /* Channel volumes (32 bytes) */
    for (int i = 0; i < MT_MAX_CHANNELS; i++)
        wb_w8(&wb, song->channel_volume[i]);

    /* Channel panning (32 bytes) */
    for (int i = 0; i < MT_MAX_CHANNELS; i++)
        wb_w8(&wb, song->channel_panning[i]);

    /* Pattern order table (200 bytes) */
    int z;
    for (z = 0; z < song->order_count; z++) {
        u8 ord = song->orders[z];
        if (ord < 254 && ord >= patt_count)
            ord = 254;      /* invalid pattern index -> skip marker */
        wb_w8(&wb, ord);
    }
    for (; z < MT_MAX_ORDERS; z++)
        wb_w8(&wb, 0xFF);

    /* =============================================================
     * 3. Reserve space for offset tables
     * ============================================================= */
    u32 offset_table_pos = wb.pos;
    u32 offset_table_bytes = (u32)(inst_count + samp_count + patt_count) * 4;
    for (u32 i = 0; i < offset_table_bytes; i++)
        wb_w8(&wb, BYTESMASHER);

    /* =============================================================
     * 4. Instruments
     * ============================================================= */
    u32 inst_offsets[256];
    for (int i = 0; i < inst_count; i++) {
        wb_align32(&wb);
        inst_offsets[i] = wb.pos - module_base;
        write_instrument(&wb, &song->instruments[i]);
    }

    /* =============================================================
     * 5. Samples
     * ============================================================= */
    u32 samp_offsets[256];
    for (int i = 0; i < samp_count; i++) {
        wb_align32(&wb);
        samp_offsets[i] = wb.pos - module_base;
        write_sample(&wb, &song->samples[i]);
    }

    /* =============================================================
     * 6. Patterns -- the real meat
     * ============================================================= */

    /* Allocate pattern marks (stack is fine for small count). */
    PatternMarks *pm = NULL;
    if (patt_count > 0) {
        pm = (PatternMarks *)calloc(patt_count, sizeof(PatternMarks));
        if (!pm) {
            free(buf);
            return -2;
        }
        mark_patterns(song, pm);
    }

    u32 patt_offsets[256];
    for (int i = 0; i < patt_count; i++) {
        patt_offsets[i] = wb.pos - module_base;

        const MT_Pattern *pat = song->patterns[i];
        if (pat) {
            write_pattern(&wb, pat, &pm[i], song->xm_mode);
        } else {
            /* NULL pattern: write a 1-row empty pattern. */
            wb_w8(&wb, 0);      /* row_count = 0 means 1 row */
            wb_w8(&wb, 0x00);   /* end-of-row (empty) */
        }
    }

    free(pm);

    /* Align end of file to 4-byte boundary. */
    wb_align32(&wb);

    /* =============================================================
     * 7. Back-patch prefix size and offset tables
     * ============================================================= */
    u32 module_size = wb.pos - module_base;
    wb_patch32(&wb, prefix_pos, module_size);

    /* Write offset tables at reserved position. */
    u32 tpos = offset_table_pos;
    for (int i = 0; i < inst_count; i++) {
        wb_patch32(&wb, tpos, inst_offsets[i]);
        tpos += 4;
    }
    for (int i = 0; i < samp_count; i++) {
        wb_patch32(&wb, tpos, samp_offsets[i]);
        tpos += 4;
    }
    for (int i = 0; i < patt_count; i++) {
        wb_patch32(&wb, tpos, patt_offsets[i]);
        tpos += 4;
    }

    /* =============================================================
     * 8. Flush to disk
     * ============================================================= */
    u32 total = wb.pos;
    if (total > wb.cap) {
        /* Buffer overflow -- song too large for 256 KB buffer. */
        free(buf);
        return -2;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return -1;
    }

    size_t written = fwrite(buf, 1, total, f);
    fclose(f);
    free(buf);

    if (written != total)
        return -3;

    return 0;
}
