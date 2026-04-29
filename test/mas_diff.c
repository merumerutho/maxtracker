/*
 * mas_diff.c — MAS roundtrip comparison tool.
 *
 * Loads a .mas file (produced by mmutil), parses it into MT_Song via
 * mas_load, re-serializes via mas_write, then compares the original
 * and re-serialized bytes section by section.
 *
 * This reveals any parsing or serialization mismatches between our code
 * and mmutil's format.
 *
 * Usage: mas_diff <input.mas>
 *
 * Compile: gcc -Wall -g -O0 -Inds -I. -I../arm9/source/core
 *          -I../arm9/source/io -I../arm9/source/test -I../include
 *          -o mas_diff.exe mas_diff.c ../arm9/source/core/song.c
 *          ../arm9/source/core/scale.c ../arm9/source/io/mas_write.c
 *          ../arm9/source/io/mas_load.c
 */

#include "song.h"
#include "mas_write.h"
#include "mas_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mm_mas.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static u8 *read_file(const char *path, u32 *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    fclose(f);
    *out_size = (u32)sz;
    return buf;
}

static void dump_hex(const u8 *data, u32 offset, u32 len, u32 max)
{
    if (len > max) len = max;
    for (u32 i = 0; i < len; i++) {
        if (i > 0 && (i % 16) == 0) printf("\n       ");
        printf("%02X ", data[offset + i]);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Compare sections                                                    */
/* ------------------------------------------------------------------ */

static int compare_header(const u8 *orig, u32 orig_size,
                          const u8 *ours, u32 ours_size)
{
    int errors = 0;

    if (orig_size < 8 || ours_size < 8) {
        printf("ERROR: file too small for prefix\n");
        return 1;
    }

    /* Prefix (8 bytes) */
    printf("\n=== PREFIX (8 bytes) ===\n");
    mm_mas_prefix *op = (mm_mas_prefix *)orig;
    mm_mas_prefix *mp = (mm_mas_prefix *)ours;
    if (op->type != mp->type) {
        printf("  MISMATCH type: orig=%d ours=%d\n", op->type, mp->type);
        errors++;
    }
    if (op->version != mp->version) {
        printf("  MISMATCH version: orig=%02X ours=%02X\n", op->version, mp->version);
        errors++;
    }
    printf("  orig size=%u  ours size=%u\n", op->size, mp->size);

    /* Header (276 bytes starting at offset 8) */
    printf("\n=== HEADER (276 bytes at offset 8) ===\n");
    u32 hdr_off = 8;
    if (orig_size < hdr_off + 12 || ours_size < hdr_off + 12) {
        printf("ERROR: file too small for header\n");
        return errors + 1;
    }

    mm_mas_head *oh = (mm_mas_head *)(orig + hdr_off);
    mm_mas_head *mh = (mm_mas_head *)(ours + hdr_off);

    #define CMP_FIELD(field) \
        if (oh->field != mh->field) { \
            printf("  MISMATCH %-20s: orig=%d ours=%d\n", #field, oh->field, mh->field); \
            errors++; \
        } else { \
            printf("  OK       %-20s: %d\n", #field, oh->field); \
        }

    CMP_FIELD(order_count);
    CMP_FIELD(instr_count);
    CMP_FIELD(sampl_count);
    CMP_FIELD(pattn_count);
    CMP_FIELD(flags);
    CMP_FIELD(global_volume);
    CMP_FIELD(initial_speed);
    CMP_FIELD(initial_tempo);
    CMP_FIELD(repeat_position);

    /* Check sequence */
    int seq_diffs = 0;
    for (int i = 0; i < 200; i++) {
        if (oh->sequence[i] != mh->sequence[i]) seq_diffs++;
    }
    if (seq_diffs > 0) {
        printf("  MISMATCH sequence: %d positions differ\n", seq_diffs);
        printf("  orig seq: ");
        for (int i = 0; i < 20 && oh->sequence[i] != 0xFF; i++)
            printf("%02X ", oh->sequence[i]);
        printf("\n  ours seq: ");
        for (int i = 0; i < 20 && mh->sequence[i] != 0xFF; i++)
            printf("%02X ", mh->sequence[i]);
        printf("\n");
        errors++;
    } else {
        int count = 0;
        for (int i = 0; i < 200 && oh->sequence[i] != 0xFF; i++) count++;
        printf("  OK       sequence: %d entries match\n", count);
    }

    /* Channel volumes */
    int vol_diffs = 0, pan_diffs = 0;
    for (int i = 0; i < 32; i++) {
        if (oh->channel_volume[i] != mh->channel_volume[i]) vol_diffs++;
        if (oh->channel_panning[i] != mh->channel_panning[i]) pan_diffs++;
    }
    if (vol_diffs) { printf("  MISMATCH channel_volume: %d diffs\n", vol_diffs); errors++; }
    else printf("  OK       channel_volume\n");
    if (pan_diffs) { printf("  MISMATCH channel_panning: %d diffs\n", pan_diffs); errors++; }
    else printf("  OK       channel_panning\n");

    #undef CMP_FIELD
    return errors;
}

static int compare_instruments(const u8 *orig, u32 orig_size,
                               const u8 *ours, u32 ours_size,
                               u32 inst_count)
{
    int errors = 0;
    u32 hdr_off = 8;
    mm_mas_head *oh = (mm_mas_head *)(orig + hdr_off);
    mm_mas_head *mh = (mm_mas_head *)(ours + hdr_off);

    printf("\n=== INSTRUMENTS (%u) ===\n", inst_count);

    for (u32 i = 0; i < inst_count; i++) {
        u32 o_off = hdr_off + oh->tables[i];
        u32 m_off = hdr_off + mh->tables[i];

        if (o_off + 12 > orig_size || m_off + 12 > ours_size) {
            printf("  INST %02X: offset out of bounds (orig=%u ours=%u)\n", i, o_off, m_off);
            errors++;
            continue;
        }

        mm_mas_instrument *oi = (mm_mas_instrument *)(orig + o_off);
        mm_mas_instrument *mi = (mm_mas_instrument *)(ours + m_off);

        /* Compare all 12 fixed-header bytes */
        int inst_err = 0;
        #define ICMP(f) if (oi->f != mi->f) { \
            printf("    MISMATCH %-16s: orig=%d ours=%d\n", #f, oi->f, mi->f); \
            inst_err++; }
        ICMP(global_volume) ICMP(fadeout) ICMP(random_volume)
        ICMP(dct) ICMP(nna) ICMP(env_flags) ICMP(panning) ICMP(dca)
        #undef ICMP

        /* Compare notemap field (u16 bitfield) */
        u16 o_nm = *(u16 *)((u8 *)oi + 8);
        u16 m_nm = *(u16 *)((u8 *)mi + 8);
        if (o_nm != m_nm) {
            printf("    MISMATCH notemap_field: orig=%04X ours=%04X\n", o_nm, m_nm);
            inst_err++;
        }

        /* Compare envelope data byte-by-byte */
        u8 *o_env_start = (u8 *)oi + sizeof(mm_mas_instrument);
        u8 *m_env_start = (u8 *)mi + sizeof(mm_mas_instrument);

        u8 efl = oi->env_flags;
        u8 *oep = o_env_start, *mep = m_env_start;
        const char *env_names[] = {"vol", "pan", "pitch"};
        for (int e = 0; e < 3; e++) {
            if (!(efl & (1 << e))) continue; /* envelope doesn't exist */
            /* Each envelope: 8-byte header + node_count * 4 bytes */
            u8 o_size = *oep;       /* byte 0 = size */
            u8 m_size = *mep;
            u8 o_ncnt = *(oep + 5); /* byte 5 = node_count */
            u8 m_ncnt = *(mep + 5);
            u32 o_total = (o_size > 0) ? o_size : (8 + o_ncnt * 4);
            u32 m_total = (m_size > 0) ? m_size : (8 + m_ncnt * 4);

            if (o_total != m_total) {
                printf("    ENV %s SIZE MISMATCH: orig=%u ours=%u\n",
                       env_names[e], o_total, m_total);
                inst_err++;
            } else {
                /* Compare envelope bytes */
                u32 cmp_len = (o_total < m_total) ? o_total : m_total;
                int env_byte_diffs = 0;
                for (u32 b = 0; b < cmp_len; b++) {
                    if (oep[b] != mep[b]) env_byte_diffs++;
                }
                if (env_byte_diffs > 0) {
                    printf("    ENV %s: %d/%u bytes differ (nc=%d)\n",
                           env_names[e], env_byte_diffs, cmp_len, o_ncnt);
                    /* Show header bytes */
                    printf("      orig hdr: ");
                    dump_hex(oep, 0, 8 < cmp_len ? 8 : cmp_len, 8);
                    printf("      ours hdr: ");
                    dump_hex(mep, 0, 8 < cmp_len ? 8 : cmp_len, 8);
                    /* Show first differing node */
                    if (cmp_len > 8) {
                        for (u32 n = 0; n < o_ncnt && n < 4; n++) {
                            u32 noff = 8 + n * 4;
                            if (noff + 4 <= cmp_len &&
                                memcmp(oep + noff, mep + noff, 4) != 0) {
                                printf("      node %d: orig=", n);
                                dump_hex(oep, noff, 4, 4);
                                printf("              ours=");
                                dump_hex(mep, noff, 4, 4);
                            }
                        }
                    }
                    inst_err += env_byte_diffs;
                }
            }
            oep += o_total;
            mep += m_total;
        }

        if (inst_err > 0) {
            printf("  INST %02X: %d total mismatches\n", i, inst_err);
            errors += inst_err;
        } else {
            printf("  INST %02X: OK\n", i);
        }
    }

    return errors;
}

static int compare_samples(const u8 *orig, u32 orig_size,
                           const u8 *ours, u32 ours_size,
                           u32 inst_count, u32 samp_count)
{
    int errors = 0;
    u32 hdr_off = 8;
    mm_mas_head *oh = (mm_mas_head *)(orig + hdr_off);
    mm_mas_head *mh = (mm_mas_head *)(ours + hdr_off);

    printf("\n=== SAMPLES (%u) ===\n", samp_count);

    for (u32 i = 0; i < samp_count; i++) {
        u32 o_off = hdr_off + oh->tables[inst_count + i];
        u32 m_off = hdr_off + mh->tables[inst_count + i];

        if (o_off + 12 > orig_size || m_off + 12 > ours_size) {
            printf("  SAMP %02X: offset out of bounds\n", i);
            errors++;
            continue;
        }

        mm_mas_sample_info *os = (mm_mas_sample_info *)(orig + o_off);
        mm_mas_sample_info *ms = (mm_mas_sample_info *)(ours + m_off);

        int samp_err = 0;
        if (os->default_volume != ms->default_volume) samp_err++;
        if (os->panning != ms->panning) samp_err++;
        if (os->frequency != ms->frequency) samp_err++;
        if (os->global_volume != ms->global_volume) samp_err++;
        if (os->msl_id != ms->msl_id) samp_err++;

        if (samp_err > 0) {
            printf("  SAMP %02X: %d field mismatches\n", i, samp_err);
            printf("    orig: dv=%d pan=%d freq=%d gv=%d msl=%04X\n",
                   os->default_volume, os->panning, os->frequency,
                   os->global_volume, os->msl_id);
            printf("    ours: dv=%d pan=%d freq=%d gv=%d msl=%04X\n",
                   ms->default_volume, ms->panning, ms->frequency,
                   ms->global_volume, ms->msl_id);
            errors += samp_err;
        } else {
            printf("  SAMP %02X: OK (dv=%d pan=%d freq=%d gv=%d)\n",
                   i, os->default_volume, os->panning, os->frequency, os->global_volume);
        }

        /* Compare NDS sample header */
        if (os->msl_id == 0xFFFF && ms->msl_id == 0xFFFF) {
            mm_mas_ds_sample *od = (mm_mas_ds_sample *)(orig + o_off + sizeof(mm_mas_sample_info));
            mm_mas_ds_sample *md = (mm_mas_ds_sample *)(ours + m_off + sizeof(mm_mas_sample_info));

            if (od->loop_start != md->loop_start ||
                od->length != md->length ||
                od->format != md->format ||
                od->repeat_mode != md->repeat_mode) {
                printf("    NDS HDR MISMATCH:\n");
                printf("      orig: ls=%u len=%u fmt=%d rep=%d freq=%d\n",
                       od->loop_start, od->length, od->format, od->repeat_mode, od->default_frequency);
                printf("      ours: ls=%u len=%u fmt=%d rep=%d freq=%d\n",
                       md->loop_start, md->length, md->format, md->repeat_mode, md->default_frequency);
                errors++;
            }

            /* Compare ALL PCM data */
            u8 *opcm = (u8 *)od + sizeof(mm_mas_ds_sample);
            u8 *mpcm = (u8 *)md + sizeof(mm_mas_ds_sample);
            u32 pcm_bytes = od->length * 4;

            /* Bounds check */
            u32 opcm_end = (u32)(opcm - orig) + pcm_bytes;
            u32 mpcm_end = (u32)(mpcm - ours) + pcm_bytes;
            if (opcm_end > orig_size || mpcm_end > ours_size) {
                printf("    PCM out of bounds (len=%u words)\n", od->length);
                errors++;
            } else if (pcm_bytes > 0 && memcmp(opcm, mpcm, pcm_bytes) != 0) {
                /* Count differing bytes and find first diff */
                u32 diff_count = 0;
                u32 first_diff = 0;
                for (u32 b = 0; b < pcm_bytes; b++) {
                    if (opcm[b] != mpcm[b]) {
                        if (diff_count == 0) first_diff = b;
                        diff_count++;
                    }
                }
                printf("    PCM MISMATCH: %u/%u bytes differ (first at offset %u)\n",
                       diff_count, pcm_bytes, first_diff);
                printf("      orig @%u: ", first_diff);
                dump_hex(opcm, first_diff, 16, 16);
                printf("      ours @%u: ", first_diff);
                dump_hex(mpcm, first_diff, 16, 16);
                errors++;
            }

            /* Compare 4-byte wraparound padding after PCM */
            u8 *owrap = opcm + pcm_bytes;
            u8 *mwrap = mpcm + pcm_bytes;
            u32 owrap_end = (u32)(owrap - orig) + 4;
            u32 mwrap_end = (u32)(mwrap - ours) + 4;
            if (owrap_end <= orig_size && mwrap_end <= ours_size &&
                memcmp(owrap, mwrap, 4) != 0) {
                printf("    WRAPAROUND MISMATCH:\n");
                printf("      orig: "); dump_hex(owrap, 0, 4, 4);
                printf("      ours: "); dump_hex(mwrap, 0, 4, 4);
                errors++;
            }
        }
    }

    return errors;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <input.mas> [output.mas]\n", argv[0]);
        printf("Loads a .mas, re-serializes, and compares byte-by-byte.\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argc > 2 ? argv[2] : "_roundtrip.mas";

    /* Read original file */
    u32 orig_size;
    u8 *orig = read_file(input_path, &orig_size);
    if (!orig) {
        printf("ERROR: Cannot read '%s'\n", input_path);
        return 1;
    }
    printf("Loaded: %s (%u bytes)\n", input_path, orig_size);

    /* Parse into MT_Song */
    song_init();
    int rc = mas_load(input_path, &song);
    if (rc != 0) {
        if (rc < 0) {
            printf("ERROR: mas_load failed with code %d\n", rc);
            free(orig);
            return 1;
        }
        if (rc == 1)
            printf("WARNING: loaded with memory warnings\n");
    }

    printf("Parsed: inst=%d samp=%d patt=%d orders=%d ch=%d\n",
           song.inst_count, song.samp_count, song.patt_count,
           song.order_count, song.channel_count);
    printf("        tempo=%d speed=%d gvol=%d\n",
           song.initial_tempo, song.initial_speed, song.global_volume);

    /* Dump instrument info */
    for (int i = 0; i < song.inst_count; i++) {
        MT_Instrument *inst = &song.instruments[i];
        if (inst->active)
            printf("  Inst %02X: gv=%d pan=%d smp=%d nna=%d env=%02X\n",
                   i, inst->global_volume, inst->panning, inst->sample,
                   inst->nna,
                   (inst->env_vol.node_count > 0 ? 1 : 0) |
                   (inst->env_pan.node_count > 0 ? 2 : 0) |
                   (inst->env_pitch.node_count > 0 ? 4 : 0));
    }

    /* Dump sample info */
    for (int i = 0; i < song.samp_count; i++) {
        MT_Sample *s = &song.samples[i];
        if (s->active)
            printf("  Samp %02X: len=%lu freq=%lu bits=%d pan=%d dv=%d gv=%d loop=%lu+%lu\n",
                   i, (unsigned long)s->length, (unsigned long)s->base_freq,
                   s->bits, s->panning, s->default_volume, s->global_volume,
                   (unsigned long)s->loop_start, (unsigned long)s->loop_length);
    }

    /* Dump pattern info */
    for (int i = 0; i < song.patt_count; i++) {
        if (song.patterns[i]) {
            MT_Pattern *p = song.patterns[i];
            int note_count = 0;
            for (int r = 0; r < p->nrows; r++)
                for (int c = 0; c < song.channel_count; c++)
                    if (MT_CELL(p, r, c)->note != 250) note_count++;
            printf("  Patt %02X: %d rows, %d notes\n", i, p->nrows, note_count);
        }
    }

    /* Re-serialize */
    rc = mas_write(output_path, &song);
    if (rc != 0) {
        printf("ERROR: mas_write failed with code %d\n", rc);
        free(orig);
        song_free();
        return 1;
    }

    /* Read back re-serialized file */
    u32 ours_size;
    u8 *ours = read_file(output_path, &ours_size);
    if (!ours) {
        printf("ERROR: Cannot read back '%s'\n", output_path);
        free(orig);
        song_free();
        return 1;
    }
    printf("\nRe-serialized: %u bytes (orig %u bytes, diff %+d)\n",
           ours_size, orig_size, (int)ours_size - (int)orig_size);

    /* Compare sections */
    int total_errors = 0;

    mm_mas_head *oh = (mm_mas_head *)(orig + 8);
    total_errors += compare_header(orig, orig_size, ours, ours_size);
    total_errors += compare_instruments(orig, orig_size, ours, ours_size,
                                        oh->instr_count);
    total_errors += compare_samples(orig, orig_size, ours, ours_size,
                                     oh->instr_count, oh->sampl_count);

    /* Compare patterns (RLE compressed data) */
    {
        u32 hdr_off = 8;
        mm_mas_head *ohdr = (mm_mas_head *)(orig + hdr_off);
        mm_mas_head *mhdr = (mm_mas_head *)(ours + hdr_off);
        u32 patt_count = ohdr->pattn_count;
        u32 ic = ohdr->instr_count, sc = ohdr->sampl_count;

        printf("\n=== PATTERNS (%u) ===\n", patt_count);

        for (u32 i = 0; i < patt_count; i++) {
            u32 o_off = hdr_off + ohdr->tables[ic + sc + i];
            u32 m_off = hdr_off + mhdr->tables[ic + sc + i];

            if (o_off >= orig_size || m_off >= ours_size) {
                printf("  PATT %02X: offset out of bounds\n", i);
                total_errors++;
                continue;
            }

            /* Row count byte */
            u8 o_rows = orig[o_off];
            u8 m_rows = ours[m_off];
            if (o_rows != m_rows) {
                printf("  PATT %02X: row count MISMATCH orig=%d ours=%d\n",
                       i, o_rows + 1, m_rows + 1);
                total_errors++;
                continue;
            }

            /* Determine pattern data size by scanning for the next offset or EOF */
            u32 o_next, m_next;
            if (i + 1 < patt_count) {
                o_next = hdr_off + ohdr->tables[ic + sc + i + 1];
                m_next = hdr_off + mhdr->tables[ic + sc + i + 1];
            } else {
                /* Last pattern: use prefix size field for orig, file size for ours */
                mm_mas_prefix *pfx = (mm_mas_prefix *)orig;
                o_next = 8 + pfx->size;
                if (o_next > orig_size) o_next = orig_size;
                m_next = ours_size;
            }

            u32 o_patt_size = (o_next > o_off) ? o_next - o_off : 0;
            u32 m_patt_size = (m_next > m_off) ? m_next - m_off : 0;

            if (o_patt_size != m_patt_size) {
                printf("  PATT %02X: RLE size MISMATCH orig=%u ours=%u (%d rows)\n",
                       i, o_patt_size, m_patt_size, o_rows + 1);
                total_errors++;
            } else if (o_patt_size > 0) {
                u32 cmp_len = (o_patt_size < m_patt_size) ? o_patt_size : m_patt_size;
                if (memcmp(orig + o_off, ours + m_off, cmp_len) != 0) {
                    u32 diff_count = 0;
                    u32 first_diff = 0;
                    for (u32 b = 0; b < cmp_len; b++) {
                        if (orig[o_off + b] != ours[m_off + b]) {
                            if (diff_count == 0) first_diff = b;
                            diff_count++;
                        }
                    }
                    printf("  PATT %02X: RLE DATA MISMATCH %u/%u bytes differ "
                           "(first at +%u, %d rows)\n",
                           i, diff_count, cmp_len, first_diff, o_rows + 1);
                    total_errors++;
                } else {
                    printf("  PATT %02X: OK (%u bytes, %d rows)\n",
                           i, o_patt_size, o_rows + 1);
                }
            }
        }
    }

    printf("\n=== TOTAL: %d mismatches ===\n", total_errors);

    /* Cleanup */
    free(orig);
    free(ours);
    song_free();
    remove(output_path);

    return total_errors > 0 ? 1 : 0;
}
