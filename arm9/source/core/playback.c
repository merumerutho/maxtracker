/*
 * playback.c — ARM9 playback controller for maxtracker.
 *
 * Builds a minimal MAS header for maxmod, manages the shared pattern state,
 * and communicates with ARM7 via FIFO for playback control.
 *
 * Key design:
 *   - Actual pattern data is read live from MT_Pattern cells[][] by ARM7's
 *     patched mmReadPattern (via mt_shared->cells pointer).
 *   - This file only builds the MAS "scaffolding": header, instrument info,
 *     sample info + PCM, stub patterns (just row counts), and order table.
 *   - ARM7 uses the MAS for instrument/sample resolution; patterns come from
 *     the shared state.
 */

#include <nds.h>
#include <maxmod9.h>
#include <mm_mas.h>
#include <string.h>
#include <stdlib.h>

#include "playback.h"
#include "song.h"
#include "undo.h"
#include "util.h"
#include "mt_ipc.h"
#include "mt_shared.h"

/* ====================================================================
 * Static state
 * ==================================================================== */

/* Shared pattern state — allocated here, pointer sent to ARM7 */
static MT_SharedPatternState shared_state __attribute__((aligned(32)));

/* MAS buffer built in RAM (dynamically allocated) */
static u8 *mas_buffer = NULL;
static u32 mas_buffer_size = 0;

/* Playback tracking state (updated from ARM7 tick messages) */
static volatile bool pb_playing = false;
static volatile u8   pb_row = 0;
static volatile u8   pb_order = 0;
static volatile u8   pb_tick = 0;

/* Flag: pattern cells were edited and need cache flush */
static bool pb_cells_dirty = false;


/* ====================================================================
 * FIFO handler — receives messages from ARM7
 * ==================================================================== */

static void pb_fifo_value_handler(u32 value, void *userdata)
{
    (void)userdata;
    u8 cmd   = MT_CMD_TYPE(value);
    u32 param = MT_CMD_PARAM(value);

    switch (cmd) {
    case MT_CMD_TICK:
    {
        /* param encoding: pos | (row << 8) */
        pb_order = param & 0xFF;
        pb_row   = (param >> 8) & 0xFF;
        pb_playing = true;
        break;
    }

    case MT_CMD_PATTERN_END:
    {
        /*
         * ARM7 is about to advance to the next pattern.
         * Update mt_shared->cells to point to the new pattern's data.
         */
        u8 next_pos = param & 0xFF;

        if (next_pos < song.order_count) {
            u8 patt_idx = song.orders[next_pos];

            MT_Pattern *pat = song.patterns[patt_idx];
            if (pat) {
                shared_state.cells = (volatile MT_Cell *)pat->cells;
                shared_state.nrows = pat->nrows;
            } else {
                /* Pattern not allocated — point to NULL, ARM7 will get empty */
                shared_state.cells = NULL;
                shared_state.nrows = 64;
            }

            shared_state.channel_count = song.channel_count;
            DC_FlushRange((void *)&shared_state, sizeof(shared_state));
        }
        break;
    }

    case MT_CMD_SONG_END:
        pb_playing = false;
        break;

    default:
        break;
    }
}

/* ====================================================================
 * MAS Header Builder
 *
 * Constructs a minimal MAS file in RAM that maxmod can parse for
 * instrument/sample metadata, while actual pattern data is read from
 * shared state by the patched mmReadPattern.
 *
 * MAS layout (as expected by mmPlayMAS):
 *   [mm_mas_prefix]  — 8 bytes
 *   [mm_mas_head]    — header with orders, flags, channel info
 *   [tables[]]       — instr_count offsets + sampl_count offsets + pattn_count offsets
 *   [instrument data]
 *   [sample info + PCM data]
 *   [stub pattern data] — just row_count byte per pattern
 * ==================================================================== */

/*
 * Calculate the size needed for the MAS buffer and build it.
 *
 * Serializes real instrument envelopes/notemaps and sample PCM data
 * from song.instruments[] and song.samples[] into the MAS format that
 * maxmod expects.
 */

/* Helper: calculate the serialized size of one envelope (0 if not present) */
static u32 envelope_data_size(const MT_Envelope *env)
{
    if (!env || env->node_count == 0)
        return 0;
    /* 8-byte header + 4 bytes per node */
    return sizeof(mm_mas_envelope) + (u32)env->node_count * sizeof(mm_mas_envelope_node);
}

/* Helper: calculate the serialized size of one instrument */
static u32 instrument_data_size(const MT_Instrument *inst)
{
    u32 size = sizeof(mm_mas_instrument); /* 12 bytes fixed */

    if (inst->active) {
        /* Envelopes: only present if env_flags bits 0-2 are set */
        if (inst->env_vol.node_count > 0)
            size += envelope_data_size(&inst->env_vol);
        if (inst->env_pan.node_count > 0)
            size += envelope_data_size(&inst->env_pan);
        if (inst->env_pitch.node_count > 0)
            size += envelope_data_size(&inst->env_pitch);

        /* Notemap: 120 * u16 if full, otherwise encoded in the fixed header */
        if (inst->has_full_notemap)
            size += MT_MAX_NOTEMAP * sizeof(u16);
    }

    return size;
}

/* Helper: calculate the serialized size of one sample (info + DS header + PCM) */
static u32 sample_data_size(const MT_Sample *samp)
{
    u32 size = sizeof(mm_mas_sample_info) + sizeof(mm_mas_ds_sample);

    if (samp->active && samp->pcm_data && samp->length > 0) {
        u32 bps = (samp->format == 1) ? 2 : 1; /* 16-bit or 8-bit */
        u32 pcm_bytes = samp->length * bps;
        pcm_bytes = (pcm_bytes + 3) & ~3u; /* 4-byte align */
        pcm_bytes += 4; /* wraparound bytes */
        size += pcm_bytes;
    } else {
        size += 4; /* 4 bytes silence */
    }

    return size;
}

/* Helper: serialize one envelope into the buffer, return bytes written */
static u32 write_envelope(u8 *dest, const MT_Envelope *env)
{
    if (!env || env->node_count == 0)
        return 0;

    mm_mas_envelope *me = (mm_mas_envelope *)dest;
    me->size       = sizeof(mm_mas_envelope) + env->node_count * sizeof(mm_mas_envelope_node);
    me->loop_start = env->loop_start;
    me->loop_end   = env->loop_end;
    me->sus_start  = env->sus_start;
    me->sus_end    = env->sus_end;
    me->node_count = env->node_count;
    me->is_filter  = 0;
    me->wasted     = 0;

    u8 *node_dest = dest + sizeof(mm_mas_envelope);
    for (int n = 0; n < env->node_count; n++) {
        s32 base_y = env->nodes[n].y;
        s32 range, delta;

        if (n < env->node_count - 1) {
            range = (s32)env->nodes[n + 1].x - (s32)env->nodes[n].x;
            range = clamp_s32(range, 1, 511);
            s32 dy = (s32)env->nodes[n + 1].y - base_y;
            delta = (dy * 512 + range / 2) / range;
            delta = clamp_s32(delta, -32768, 32767);

            /* Overflow correction (match MAS playback's >> 9 shift) */
            while ((base_y + ((delta * range) >> 9)) > 64) delta--;
            while ((base_y + ((delta * range) >> 9)) < 0)  delta++;
        } else {
            range = 0;
            delta = 0;
        }

        /* Write as raw s16 delta + u16 packed (base | range<<7) */
        s16 delta16 = (s16)delta;
        u16 packed = (u16)((base_y & 0x7F) | ((range & 0x1FF) << 7));
        memcpy(node_dest, &delta16, 2);
        memcpy(node_dest + 2, &packed, 2);
        node_dest += 4;
    }

    return sizeof(mm_mas_envelope) + (u32)env->node_count * sizeof(mm_mas_envelope_node);
}

static void build_mas_buffer(void)
{
    u32 inst_count = song.inst_count;
    u32 samp_count = song.samp_count;
    u32 patt_count = song.patt_count;

    if (inst_count == 0) inst_count = 1;
    if (samp_count == 0) samp_count = 1;
    if (patt_count == 0) patt_count = 1;

    /* Size of the offset tables */
    u32 table_size = (inst_count + samp_count + patt_count) * sizeof(u32);

    /* Calculate exact instrument data size by iterating */
    u32 *inst_sizes = (u32 *)malloc(inst_count * sizeof(u32));
    if (!inst_sizes) return;
    u32 inst_data_size = 0;
    for (u32 i = 0; i < inst_count; i++) {
        if (i < (u32)song.inst_count && song.instruments[i].active)
            inst_sizes[i] = instrument_data_size(&song.instruments[i]);
        else
            inst_sizes[i] = sizeof(mm_mas_instrument); /* minimal stub */
        inst_data_size += inst_sizes[i];
    }

    /* Calculate exact sample data size by iterating */
    u32 *samp_sizes = (u32 *)malloc(samp_count * sizeof(u32));
    if (!samp_sizes) { free(inst_sizes); return; }
    u32 samp_data_size = 0;
    for (u32 i = 0; i < samp_count; i++) {
        if (i < (u32)song.samp_count && song.samples[i].active)
            samp_sizes[i] = sample_data_size(&song.samples[i]);
        else
            samp_sizes[i] = sizeof(mm_mas_sample_info) + sizeof(mm_mas_ds_sample) + 4;
        samp_data_size += samp_sizes[i];
    }

    /* Stub patterns: 1 byte row_count + 1 byte end marker each */
    u32 patt_data_size = patt_count * 2;

    u32 total = sizeof(mm_mas_prefix) + sizeof(mm_mas_head) + table_size
              + inst_data_size + samp_data_size + patt_data_size;

    /* Align up to 4 bytes */
    total = (total + 3) & ~3u;

    /* (Re)allocate if needed */
    if (mas_buffer == NULL || mas_buffer_size < total) {
        if (mas_buffer)
            free(mas_buffer);
        mas_buffer = (u8 *)malloc(total);
        mas_buffer_size = total;
    }

    if (!mas_buffer) {
        free(inst_sizes);
        free(samp_sizes);
        return;
    }

    memset(mas_buffer, 0, total);

    u8 *p = mas_buffer;

    /* --- Prefix --- */
    mm_mas_prefix *prefix = (mm_mas_prefix *)p;
    prefix->size    = total - sizeof(mm_mas_prefix);
    prefix->type    = MAS_TYPE_SONG;
    prefix->version = 0;
    p += sizeof(mm_mas_prefix);

    /* --- Header --- */
    mm_mas_head *hdr = (mm_mas_head *)p;
    hdr->order_count    = song.order_count;
    hdr->instr_count    = inst_count;
    hdr->sampl_count    = samp_count;
    hdr->pattn_count    = patt_count;
    hdr->global_volume  = song.global_volume;
    hdr->initial_speed  = song.initial_speed;
    hdr->initial_tempo  = song.initial_tempo;
    hdr->repeat_position = song.repeat_position;

    /* Flags */
    hdr->flags = 0;
    if (song.freq_linear)
        hdr->flags |= MAS_HEADER_FLAG_FREQ_MODE;
    if (song.xm_mode)
        hdr->flags |= MAS_HEADER_FLAG_XM_MODE;
    if (song.old_mode)
        hdr->flags |= MAS_HEADER_FLAG_OLD_MODE;
    if (song.link_gxx)
        hdr->flags |= MAS_HEADER_FLAG_LINK_GXX;

    /* Channel volumes and pannings */
    for (int i = 0; i < 32; i++) {
        hdr->channel_volume[i]  = song.channel_volume[i];
        hdr->channel_panning[i] = song.channel_panning[i];
    }

    /* Sequence / order table */
    memset(hdr->sequence, 255, 200);  /* 255 = end marker */
    for (int i = 0; i < song.order_count && i < 200; i++)
        hdr->sequence[i] = song.orders[i];

    /* --- Offset tables --- */
    /*
     * The tables are arrays of u32 offsets from the base of mm_mas_head.
     * Layout: inst_table[inst_count], samp_table[samp_count], patt_table[patt_count]
     */
    u32 *tables = hdr->tables;
    u8 *base = (u8 *)hdr;

    /* Calculate where each data section starts (offset from hdr base) */
    u32 tables_end_offset = sizeof(mm_mas_head) + table_size;

    /* Fill instrument offset table (variable sizes) */
    u32 offset = tables_end_offset;
    for (u32 i = 0; i < inst_count; i++) {
        tables[i] = offset;
        offset += inst_sizes[i];
    }

    /* Fill sample offset table (variable sizes) */
    for (u32 i = 0; i < samp_count; i++) {
        tables[inst_count + i] = offset;
        offset += samp_sizes[i];
    }

    /* Fill pattern offset table */
    u32 patt_start = offset;
    for (u32 i = 0; i < patt_count; i++)
        tables[inst_count + samp_count + i] = patt_start + i * 2;

    free(inst_sizes);
    free(samp_sizes);

    /* --- Instrument data --- */
    for (u32 i = 0; i < inst_count; i++)
    {
        u8 *inst_base = base + tables[i];
        mm_mas_instrument *mi = (mm_mas_instrument *)inst_base;

        const MT_Instrument *src = NULL;
        if (i < (u32)song.inst_count && song.instruments[i].active)
            src = &song.instruments[i];

        if (src) {
            mi->global_volume  = src->global_volume;
            mi->fadeout        = src->fadeout;
            mi->random_volume  = src->random_volume;
            mi->dct            = src->dct;
            mi->nna            = src->nna;
            mi->panning        = src->panning;
            mi->dca            = src->dca;

            /* Build env_flags. Matches mmutil's bit layout — see
             * mas_load.c for the rationale. Bits 4-7 are unused. */
            u8 eflags = 0;
            if (src->env_vol.node_count > 0)   eflags |= MAS_INSTR_FLAG_VOL_ENV_EXISTS;
            if (src->env_pan.node_count > 0)   eflags |= MAS_INSTR_FLAG_PAN_ENV_EXISTS;
            if (src->env_pitch.node_count > 0) eflags |= (1 << 2); /* pitch exists */
            if (src->env_vol.enabled)          eflags |= MAS_INSTR_FLAG_VOL_ENV_ENABLED;
            mi->env_flags = eflags;

            /* Write envelopes after fixed header */
            u8 *env_ptr = inst_base + sizeof(mm_mas_instrument);
            if (src->env_vol.node_count > 0)
                env_ptr += write_envelope(env_ptr, &src->env_vol);
            if (src->env_pan.node_count > 0)
                env_ptr += write_envelope(env_ptr, &src->env_pan);
            if (src->env_pitch.node_count > 0)
                env_ptr += write_envelope(env_ptr, &src->env_pitch);

            /* Notemap */
            if (src->has_full_notemap) {
                mi->is_note_map_invalid = 0;
                mi->note_map_offset = (u16)(env_ptr - inst_base);
                memcpy(env_ptr, src->notemap, MT_MAX_NOTEMAP * sizeof(u16));
            } else {
                mi->is_note_map_invalid = 1;
                mi->note_map_offset = (src->sample > 0) ? src->sample : 1;
            }
        } else {
            /* Inactive/stub instrument */
            mi->global_volume = 128;
            mi->fadeout       = 0;
            mi->random_volume = 0;
            mi->dct           = 0;
            mi->nna           = 0;
            mi->env_flags     = 0;
            mi->panning       = 0;
            mi->dca           = 0;
            mi->is_note_map_invalid = 1;
            mi->note_map_offset = (i < samp_count) ? (i + 1) : 1;
        }

        mi->reserved = 0;
    }

    /* --- Sample data --- */
    for (u32 i = 0; i < samp_count; i++)
    {
        u8 *samp_base = base + tables[inst_count + i];
        mm_mas_sample_info *si = (mm_mas_sample_info *)samp_base;

        const MT_Sample *src = NULL;
        if (i < (u32)song.samp_count && song.samples[i].active)
            src = &song.samples[i];

        if (src) {
            /* 12 bytes sample info */
            si->default_volume = src->default_volume;
            si->panning        = src->panning;
            si->frequency      = (u16)(src->base_freq / 4);
            si->av_type        = src->vib_type;
            si->av_depth       = src->vib_depth;
            si->av_speed       = src->vib_speed;
            si->global_volume  = src->global_volume;
            si->av_rate        = src->vib_rate;
            si->msl_id         = 0xFFFF; /* Inline sample follows */

            /* 16 bytes NDS DS sample header.
             *
             * Must match mmutil's encoding exactly — the mixer reads these
             * fields directly to program the DS hardware sound channels.
             *
             * Key encoding rules (from mmutil/source/mas.c):
             *   loop_start:  in words (sample_offset / 4 for 8-bit, / 2 for 16-bit)
             *   length:      for non-looping = total length in words
             *                for looping     = loop REGION length in words (union!)
             *   default_frequency: (base_freq * 1024 + 16384) / 32768
             */
            mm_mas_ds_sample *ds = (mm_mas_ds_sample *)(samp_base + sizeof(mm_mas_sample_info));
            u32 bps = (src->format == 1) ? 2 : 1;
            /* DS hardware always uses 32-bit words for length/offset */
            u32 word_div = 4;

            if (src->loop_type == 1 && src->pcm_data && src->length > 0) {
                /* Looping: loop_start + loop_length (both in words) */
                ds->loop_start  = (src->loop_start * bps) / word_div;
                u32 loop_end    = src->length;  /* samples */
                u32 loop_region = (loop_end > src->loop_start)
                                ? (loop_end - src->loop_start) : 0;
                ds->loop_length = (loop_region * bps + 3) / word_div;
            } else if (src->pcm_data && src->length > 0) {
                /* Non-looping: loop_start=0, length=total in words */
                ds->loop_start = 0;
                u32 pcm_bytes = src->length * bps;
                ds->length = ((pcm_bytes + 3) & ~3u) / 4;
            } else {
                ds->loop_start = 0;
                ds->length = 1; /* 4 bytes silence = 1 word */
            }

            ds->format = (src->format == 1) ? MM_SFORMAT_16BIT : MM_SFORMAT_8BIT;
            ds->repeat_mode = (src->loop_type == 1) ? MM_SREPEAT_FORWARD : MM_SREPEAT_OFF;
            /* Match mmutil: (freq * 1024 + 16384) / 32768 */
            ds->default_frequency = (u16)(((u32)src->base_freq * 1024 + 16384) / 32768);
            ds->point = 0;

            /* PCM data */
            u8 *pcm_dest = (u8 *)ds + sizeof(mm_mas_ds_sample);
            if (src->pcm_data && src->length > 0) {
                u32 pcm_bytes = src->length * bps;
                u32 pcm_aligned = (pcm_bytes + 3) & ~3u;
                memcpy(pcm_dest, src->pcm_data, pcm_bytes);
                /* Zero-pad to alignment */
                if (pcm_aligned > pcm_bytes)
                    memset(pcm_dest + pcm_bytes, 0, pcm_aligned - pcm_bytes);
                /* 4 bytes wraparound: copy from loop start if looping, zeros otherwise */
                if (src->loop_type == 1 && src->loop_start < src->length) {
                    u32 wrap_off = src->loop_start * bps;
                    for (int j = 0; j < 4; j++) {
                        if (wrap_off + (u32)j < pcm_bytes)
                            pcm_dest[pcm_aligned + j] = src->pcm_data[wrap_off + j];
                        else
                            pcm_dest[pcm_aligned + j] = 0;
                    }
                } else {
                    memset(pcm_dest + pcm_aligned, 0, 4);
                }
            } else {
                /* 4 bytes silence */
                memset(pcm_dest, 0, 4);
            }
        } else {
            /* Inactive/stub sample */
            si->default_volume = 64;
            si->panning        = 0xC0; /* 0x80 | 64 = center */
            si->frequency      = 8363;
            si->av_type        = 0;
            si->av_depth       = 0;
            si->av_speed       = 0;
            si->global_volume  = 64;
            si->av_rate        = 0;
            si->msl_id         = 0xFFFF;

            mm_mas_ds_sample *ds = (mm_mas_ds_sample *)(samp_base + sizeof(mm_mas_sample_info));
            ds->loop_start        = 0;
            ds->length            = 1; /* 1 word = 4 bytes silence */
            ds->format            = MM_SFORMAT_16BIT;
            ds->repeat_mode       = MM_SREPEAT_OFF;
            ds->default_frequency = 8363;
            ds->point             = 0;

            u8 *pcm = (u8 *)ds + sizeof(mm_mas_ds_sample);
            memset(pcm, 0, 4);
        }
    }

    /* --- Stub pattern data --- */
    for (u32 i = 0; i < patt_count; i++)
    {
        mm_mas_pattern *pat = (mm_mas_pattern *)(base + tables[inst_count + samp_count + i]);

        /* Get actual row count from the song's pattern, or default to 64 */
        u8 nrows = 64;
        if (i < MT_MAX_PATTERNS && song.patterns[i])
            nrows = (song.patterns[i]->nrows > 0) ? (song.patterns[i]->nrows - 1) : 0;
        else
            nrows = 63;  /* 64 rows, but maxmod stores nrows-1 */

        pat->row_count = nrows;
        /* End-of-row marker (0x00): the original mmReadPattern reads until it
         * sees a 0x00. In MAXTRACKER_MODE this is ignored, but we include it
         * for safety if fallback mode is ever used. */
        pat->pattern_data[0] = 0x00;
    }

    /* Flush the entire MAS buffer to main RAM so ARM7 can read it */
    DC_FlushRange(mas_buffer, total);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void playback_init(void)
{
    /* Initialize shared state */
    memset((void *)&shared_state, 0, sizeof(shared_state));
    shared_state.active = 1;
    shared_state.channel_count = song.channel_count;
    DC_FlushRange((void *)&shared_state, sizeof(shared_state));

    /* maxmod already initialized in main() via mmInit() — don't reinit here */

    /* Send shared state address to ARM7 */
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_SET_SHARED, 0));
    fifoSendAddress(FIFO_MT, (void *)&shared_state);

    /* Install our FIFO handler for ARM7 -> ARM9 messages */
    fifoSetValue32Handler(FIFO_MT, pb_fifo_value_handler, NULL);

    pb_playing = false;
    pb_row = 0;
    pb_order = 0;
    pb_tick = 0;

    /* Wire ourselves up to the song module's pattern lifecycle so the
     * shared cells pointer can be detached/reattached around frees. */
    song_set_pattern_lifecycle(playback_detach_pattern,
                               playback_reattach_pattern);

    /* Hook the undo push path so cell edits proactively flush the
     * pattern cache to RAM on the next playback_update — fixes the
     * "ARM7 sees stale cells for a few frames after a pattern edit"
     * bug documented in memory/project_latent_playback_bugs. Every
     * cell-modifying view goes through undo_push_cell or
     * undo_push_block before the edit, so this single hook covers
     * every cell-edit site. */
    undo_set_on_push(playback_mark_cells_dirty);
}

void playback_play(u8 order_pos)
{
    playback_play_at(order_pos, 0);
}

void playback_play_at(u8 order_pos, u8 row)
{
    /* Build the MAS header from current song state */
    build_mas_buffer();

    if (!mas_buffer)
        return;

    /* Populate pattern lookup table — ARM7 uses this to resolve the
     * correct cells pointer instantly on pattern transitions, without
     * waiting for an IPC round-trip to ARM9. */
    shared_state.order_count = song.order_count;
    memcpy((void *)shared_state.orders, song.orders,
           song.order_count < MT_MAX_ORDERS ? song.order_count : MT_MAX_ORDERS);

    shared_state.patt_count = song.patt_count;
    for (int i = 0; i < 256; i++) {
        MT_Pattern *p = (i < song.patt_count) ? song.patterns[i] : NULL;
        if (p) {
            shared_state.patterns[i].cells = (volatile MT_Cell *)p->cells;
            shared_state.patterns[i].nrows = p->nrows;
        } else {
            shared_state.patterns[i].cells = NULL;
            shared_state.patterns[i].nrows = 64;
        }
    }

    /* Point shared state at the first pattern to be played */
    u8 patt_idx = 0;
    if (order_pos < song.order_count)
        patt_idx = song.orders[order_pos];

    MT_Pattern *pat = song.patterns[patt_idx];
    if (!pat)
        pat = song_ensure_pattern(patt_idx);

    if (pat) {
        shared_state.cells = (volatile MT_Cell *)pat->cells;
        shared_state.nrows = pat->nrows;
    } else {
        shared_state.cells = NULL;
        shared_state.nrows = 64;
    }

    /* Clamp row to pattern bounds — cursor.row can outlive a target
     * pattern that's shorter than the one it was set on. */
    if (row >= shared_state.nrows)
        row = 0;

    shared_state.active = 1;
    shared_state.playing = 1;
    shared_state.channel_count = song.channel_count;
    shared_state.pos_state = MT_POS_PACK(order_pos, row, 0);
    DC_FlushRange((void *)&shared_state, sizeof(shared_state));

    /* Send MAS address to ARM7, then play command */
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_SET_MAS, 0));
    fifoSendAddress(FIFO_MT, (void *)mas_buffer);

    /* Small delay to ensure address is received before play command */
    swiDelay(100);

    fifoSendValue32(FIFO_MT,
                    MT_MKCMD(MT_CMD_PLAY,
                             ((u32)order_pos & 0xFF) | (((u32)row & 0xFF) << 8)));

    pb_playing = true;
    pb_order = order_pos;
    pb_row = row;
    pb_tick = 0;
}

void playback_stop(void)
{
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_STOP, 0));

    shared_state.playing = 0;
    DC_FlushRange((void *)&shared_state, sizeof(shared_state));

    pb_playing = false;
}

/*
 * Get the uncached mirror of an address.
 * ARM9 main RAM: 0x02000000 (cached) maps to 0x02400000 (uncached).
 * Reading from the uncached mirror bypasses the data cache entirely,
 * so ARM7's writes are always visible without DC_InvalidateRange.
 */
static inline volatile MT_SharedPatternState *get_uncached_shared(void)
{
    u32 addr = (u32)&shared_state;
    return (volatile MT_SharedPatternState *)(addr | 0x00400000);
}

void playback_update(void)
{
    /*
     * Read position data from the uncached mirror of shared_state.
     * ARM7 writes directly to main RAM (no cache). ARM9's cached view
     * may be stale, but the uncached mirror at +0x400000 always shows
     * the latest ARM7 writes.
     */
    volatile MT_SharedPatternState *uc = get_uncached_shared();

    if (uc->playing) {
        /* Single 32-bit load — atomic with respect to ARM7's tick callback. */
        u32 ps = uc->pos_state;
        pb_playing = true;
        pb_order   = MT_POS_POSITION(ps);
        pb_row     = MT_POS_ROW(ps);
        pb_tick    = MT_POS_TICK(ps);
    } else if (pb_playing) {
        pb_playing = false;
    }

    /* Flush pattern cells only if they were modified this frame.
     * Flushing 10KB+ of cache every frame can cause timing jitter
     * when ARM7 is reading the same cache lines mid-tick. */
    if (shared_state.cells && pb_cells_dirty) {
        u16 nrows = shared_state.nrows;
        if (nrows > MT_MAX_ROWS) nrows = MT_MAX_ROWS;
        DC_FlushRange((void *)shared_state.cells,
                      nrows * shared_state.channel_count * sizeof(MT_Cell));
        pb_cells_dirty = false;
    }
}

bool playback_is_playing(void)
{
    return pb_playing;
}

u8 playback_get_row(void)
{
    return pb_row;
}

u8 playback_get_order(void)
{
    return pb_order;
}

u8 playback_get_tick(void)
{
    return pb_tick;
}

void playback_preview_note(u8 note, u8 inst)
{
    /* Suppress preview while the song is playing — otherwise the previewed
     * sample would layer on top of the scheduled playback audio and clash
     * with whatever the song is producing. The user can still insert and
     * edit notes during playback; they just won't get an extra preview ping. */
    if (pb_playing) return;

    u32 param = (u32)note | ((u32)inst << 8);
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_PREVIEW_NOTE, param));
}

void playback_stop_preview(void)
{
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_STOP_PREVIEW, 0));
}

void playback_set_mute(u8 channel, bool mute)
{
    if (channel >= MT_MAX_CHANNELS)
        return;

    if (mute)
        shared_state.mute_mask |= (1u << channel);
    else
        shared_state.mute_mask &= ~(1u << channel);

    DC_FlushRange((void *)&shared_state, sizeof(shared_state));
}

u32 playback_get_mute_mask(void)
{
    return shared_state.mute_mask;
}

void playback_set_tempo(u8 bpm)
{
    fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_SET_TEMPO, (u32)bpm));
}

void playback_mark_cells_dirty(void)
{
    pb_cells_dirty = true;
}

void playback_refresh_shared_tables(void)
{
    /* Refresh the order + pattern lookup tables in shared state.
     * Call this after editing orders, adding/removing patterns, or
     * reallocating pattern cells during playback.
     *
     * No-op when not playing — the tables will be rebuilt fresh
     * by playback_play() the next time the user hits START. This
     * lets edit-site code call us unconditionally. */
    if (!pb_playing) return;

    shared_state.order_count = song.order_count;
    memcpy((void *)shared_state.orders, song.orders,
           song.order_count < MT_MAX_ORDERS ? song.order_count : MT_MAX_ORDERS);

    shared_state.patt_count = song.patt_count;
    for (int i = 0; i < 256; i++) {
        MT_Pattern *p = (i < song.patt_count) ? song.patterns[i] : NULL;
        if (p) {
            shared_state.patterns[i].cells = (volatile MT_Cell *)p->cells;
            shared_state.patterns[i].nrows = p->nrows;
        } else {
            shared_state.patterns[i].cells = NULL;
            shared_state.patterns[i].nrows = 64;
        }
    }

    /* Also refresh the current cells pointer */
    u8 pos = pb_order;
    if (pos < song.order_count) {
        u8 patt_idx = song.orders[pos];
        MT_Pattern *pat = (patt_idx < song.patt_count) ? song.patterns[patt_idx] : NULL;
        if (pat) {
            shared_state.cells = (volatile MT_Cell *)pat->cells;
            shared_state.nrows = pat->nrows;
        }
    }

    DC_FlushRange((void *)&shared_state, sizeof(shared_state));
}

void playback_detach_pattern(void)
{
    shared_state.cells = NULL;
    DC_FlushRange((void *)&shared_state, sizeof(shared_state));
}

void playback_reattach_pattern(void)
{
    /* No-op when nothing is playing — the cells pointer will be set
     * fresh next time playback_play() runs. This lets song.c call us
     * unconditionally without needing to consult playback state. */
    if (!pb_playing) return;

    /* Pattern allocation/free changed the pattern table layout —
     * refresh shared.orders[] / shared.patterns[] so ARM7's pattern
     * transition logic sees the new layout on the next loop. */
    playback_refresh_shared_tables();

    /* Find the current pattern from the playing order position */
    u8 pos = pb_order;
    if (pos < song.order_count) {
        u8 patt_idx = song.orders[pos];
        MT_Pattern *pat = song.patterns[patt_idx];
        if (pat) {
            shared_state.cells = (volatile MT_Cell *)pat->cells;
            shared_state.nrows = pat->nrows;
            shared_state.channel_count = song.channel_count;
            DC_FlushRange((void *)&shared_state, sizeof(shared_state));
            return;
        }
    }
    shared_state.cells = NULL;
    DC_FlushRange((void *)&shared_state, sizeof(shared_state));
}

void playback_play_pattern(u8 pattern_idx)
{
    playback_play_pattern_at(pattern_idx, 0);
}

void playback_play_pattern_at(u8 pattern_idx, u8 row)
{
    /* Save the real arrangement so we can restore after the MAS is
     * built with the single-pattern order table. */
    u8  saved_count    = song.order_count;
    u8  saved_repeat   = song.repeat_position;
    u8  saved_orders[MT_MAX_ORDERS];
    memcpy(saved_orders, song.orders,
           saved_count < MT_MAX_ORDERS ? saved_count : MT_MAX_ORDERS);

    /* Temporarily set a one-entry order table so maxmod loops this
     * single pattern. repeat_position = 0 makes it loop. */
    song.order_count     = 1;
    song.orders[0]       = pattern_idx;
    song.repeat_position = 0;

    /* playback_play builds the MAS buffer from the (now single-entry)
     * order table, sets up shared_state, and sends to ARM7. */
    playback_play_at(0, row);

    /* Restore the real arrangement. The MAS buffer keeps the single-
     * pattern order table baked in — that's intentional; maxmod reads
     * from the MAS, not from song.orders. If playback_rebuild_mas is
     * called later (e.g. a LFE commit), it will rebuild from the
     * restored full arrangement, which is correct. */
    song.order_count     = saved_count;
    song.repeat_position = saved_repeat;
    memcpy(song.orders, saved_orders,
           saved_count < MT_MAX_ORDERS ? saved_count : MT_MAX_ORDERS);
}

void playback_rebuild_mas(void)
{
    bool was_playing = pb_playing;
    u8 saved_order = pb_order;

    if (was_playing)
        playback_stop();

    build_mas_buffer();

    if (was_playing && mas_buffer)
        playback_play(saved_order);
}

