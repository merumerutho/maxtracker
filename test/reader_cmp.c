/*
 * reader_cmp.c — Compare original vs patched mmReadPattern flag output.
 *
 * Loads a .mas file, then for each row of each pattern, simulates both
 * the original RLE reader and our patched flat-cell reader, comparing
 * the flags and channel state they produce.
 */

#include "song.h"
#include "mas_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mm_mas.h>

/* Simulate what the ORIGINAL mmReadPattern produces for one row.
 * Reads from the raw RLE stream. */
typedef struct {
    u8 flags;    /* pattern_flags | (compr_flags >> 4) */
    u8 pnoter;
    u8 inst;
    u8 volcmd;
    u8 effect;
    u8 param;
} ChannelState;

/* MF flag bits (upper nibble of mask) */
#define MF_START    0x01
#define MF_DVOL     0x02
#define MF_HASVCMD  0x04
#define MF_HASFX    0x08
#define MF_NOTECUT  0x10
#define MF_NOTEOFF  0x20
#define MF_NEWINSTR 0x40

#define COMPR_FLAG_NOTE  (1 << 0)
#define COMPR_FLAG_INSTR (1 << 1)
#define COMPR_FLAG_VOLC  (1 << 2)
#define COMPR_FLAG_EFFC  (1 << 3)

#define MAS_HEADER_FLAG_OLD_MODE (1 << 5)
#define MAS_HEADER_FLAG_XM_MODE  (1 << 3)

static void sim_original_row(const u8 *rle_data, u32 rle_size, u32 *rle_pos,
                             ChannelState *ch_state, int nch,
                             u8 *last_cflags, u8 mas_flags, u8 instr_count)
{
    /* Clear all flags for this row */
    for (int i = 0; i < nch; i++)
        ch_state[i].flags = 0;

    while (*rle_pos < rle_size) {
        u8 byte = rle_data[(*rle_pos)++];
        u8 chan = byte & 0x7F;
        if (chan == 0) break;
        chan--;
        if (chan >= nch) break;

        u8 pattern_flags = 0;

        if (byte & 0x80)
            last_cflags[chan] = rle_data[(*rle_pos)++];

        u8 compr = last_cflags[chan];

        if (compr & COMPR_FLAG_NOTE) {
            u8 note = rle_data[(*rle_pos)++];
            if (note == 254)
                pattern_flags |= MF_NOTECUT;
            else if (note == 255)
                pattern_flags |= MF_NOTEOFF;
            else
                ch_state[chan].pnoter = note;
        }

        if (compr & COMPR_FLAG_INSTR) {
            u8 inst = rle_data[(*rle_pos)++];
            if (!(pattern_flags & (MF_NOTECUT | MF_NOTEOFF))) {
                if (inst > instr_count) inst = 0;
                if (ch_state[chan].inst != inst) {
                    if (mas_flags & MAS_HEADER_FLAG_OLD_MODE)
                        pattern_flags |= MF_START;
                    pattern_flags |= MF_NEWINSTR;
                }
                ch_state[chan].inst = inst;
            }
        }

        if (compr & COMPR_FLAG_VOLC)
            ch_state[chan].volcmd = rle_data[(*rle_pos)++];

        if (compr & COMPR_FLAG_EFFC) {
            ch_state[chan].effect = rle_data[(*rle_pos)++];
            ch_state[chan].param  = rle_data[(*rle_pos)++];
        }

        /* KEY: flags = pattern_flags | MF flags from upper nibble */
        ch_state[chan].flags = pattern_flags | (compr >> 4);
    }
}

static void sim_patched_row(const MT_Pattern *pat, int row,
                            ChannelState *ch_state, int nch,
                            u8 mas_flags, u8 instr_count)
{
    u8 vol_empty = (mas_flags & MAS_HEADER_FLAG_XM_MODE) ? 0 : 0xFF;

    for (int ch = 0; ch < nch; ch++) {
        ch_state[ch].flags = 0;

        const MT_Cell *cell = MT_CELL(pat, row, ch);
        u8 c_note  = cell->note;
        u8 c_inst  = cell->inst;
        u8 c_vol   = cell->vol;
        u8 c_fx    = cell->fx;
        u8 c_param = cell->param;

        if (c_note == 250 && c_inst == 0 && c_vol == vol_empty &&
            c_fx == 0 && c_param == 0)
            continue;

        u8 pattern_flags = 0;

        if (c_note != 250) {
            if (c_note == 254)
                pattern_flags |= MF_NOTECUT;
            else if (c_note == 255)
                pattern_flags |= MF_NOTEOFF;
            else {
                ch_state[ch].pnoter = c_note;
                pattern_flags |= MF_START;
            }
        }

        if (c_inst != 0) {
            if (!(pattern_flags & (MF_NOTECUT | MF_NOTEOFF))) {
                u8 inst = c_inst;
                if (inst > instr_count) inst = 0;
                if (ch_state[ch].inst != inst) {
                    if (mas_flags & MAS_HEADER_FLAG_OLD_MODE)
                        pattern_flags |= MF_START;
                    pattern_flags |= MF_NEWINSTR;
                }
                ch_state[ch].inst = inst;
                pattern_flags |= MF_DVOL;
            }
        }

        if (c_vol != vol_empty) {
            ch_state[ch].volcmd = c_vol;
            pattern_flags |= MF_HASVCMD;
        }

        if (c_fx != 0 || c_param != 0) {
            ch_state[ch].effect = c_fx;
            ch_state[ch].param  = c_param;
            pattern_flags |= MF_HASFX;
        }

        ch_state[ch].flags = pattern_flags;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <input.mas> [pattern_index]\n", argv[0]);
        return 1;
    }

    /* Read raw file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *raw = (u8 *)malloc(sz);
    fread(raw, 1, sz, f);
    fclose(f);

    /* Parse into MT_Song */
    song_init();
    int rc = mas_load(argv[1], &song);
    if (rc < 0) { printf("Load failed: %d\n", rc); free(raw); return 1; }

    /* Get header info */
    mm_mas_head *hdr = (mm_mas_head *)(raw + 8);
    u8 ic = hdr->instr_count, sc = hdr->sampl_count, pc = hdr->pattn_count;
    u8 mas_flags = hdr->flags;

    int patt_to_check = (argc > 2) ? strtol(argv[2], NULL, 0) : -1;

    int total_diffs = 0;

    for (int pi = 0; pi < pc; pi++) {
        if (patt_to_check >= 0 && pi != patt_to_check) continue;

        MT_Pattern *pat = song.patterns[pi];
        if (!pat) continue;

        /* Get RLE data for this pattern */
        u32 patt_off = 8 + hdr->tables[ic + sc + pi];
        u8 nrows = raw[patt_off] + 1;
        u32 rle_pos = patt_off + 1; /* skip row_count byte */

        int nch = song.channel_count;
        if (nch > 32) nch = 32;

        ChannelState orig_state[32], patch_state[32];
        u8 orig_cflags[32];
        memset(orig_state, 0, sizeof(orig_state));
        memset(patch_state, 0, sizeof(patch_state));
        memset(orig_cflags, 0, sizeof(orig_cflags));

        int pat_diffs = 0;

        for (int row = 0; row < nrows; row++) {
            sim_original_row(raw, sz, &rle_pos, orig_state, nch,
                             orig_cflags, mas_flags, ic);
            sim_patched_row(pat, row, patch_state, nch, mas_flags, ic);

            for (int ch = 0; ch < nch; ch++) {
                if (orig_state[ch].flags != patch_state[ch].flags) {
                    if (pat_diffs < 10) {
                        printf("PATT %02X ROW %02X CH %02d FLAGS: orig=%02X patch=%02X",
                               pi, row, ch, orig_state[ch].flags, patch_state[ch].flags);
                        u8 diff = orig_state[ch].flags ^ patch_state[ch].flags;
                        if (diff & MF_START)    printf(" +START");
                        if (diff & MF_DVOL)     printf(" +DVOL");
                        if (diff & MF_HASVCMD)  printf(" +VCMD");
                        if (diff & MF_HASFX)    printf(" +FX");
                        if (diff & MF_NOTECUT)  printf(" +CUT");
                        if (diff & MF_NOTEOFF)  printf(" +OFF");
                        if (diff & MF_NEWINSTR) printf(" +NEWINST");
                        printf("\n");
                    }
                    pat_diffs++;
                }
            }
        }

        if (pat_diffs > 0) {
            printf("  Pattern %02X: %d flag differences\n", pi, pat_diffs);
            total_diffs += pat_diffs;
        }
    }

    printf("\nTotal flag differences: %d\n", total_diffs);

    free(raw);
    song_free();
    return total_diffs > 0 ? 1 : 0;
}
