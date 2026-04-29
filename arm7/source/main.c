/*
 * maxtracker ARM7 — Audio engine and IPC command handler.
 *
 * This runs on ARM7 alongside maxmod. It handles IPC commands from ARM9
 * (play, stop, set pattern, preview note, mute) and sends position updates
 * back to ARM9 via FIFO.
 *
 * maxmod is compiled with MAXTRACKER_MODE, which patches mmReadPattern()
 * to read flat MT_Cell arrays from shared RAM instead of compressed MAS data.
 */

#include <nds.h>
#include <maxmod7.h>
#include <mm_mas.h>
#include <mm_types.h>
#include "mt_ipc.h"
#include "mt_shared.h"

/* mt_shared is defined in mas_arm.c (MAXTRACKER_MODE build) */

/*
 * mmSetEventHandler is defined in mas.c (core) but not declared in maxmod7.h.
 * We declare it here so the ARM7 can register a tick callback.
 */
extern void mmSetEventHandler(mm_callback handler);

volatile bool exitflag = false;

/* --- Pending command state (set in IRQ context, processed in main loop) --- */

static volatile u32  pending_mas_addr = 0;
static volatile u8   pending_play_pos = 0;
static volatile u8   pending_play_row = 0;
static volatile bool pending_play = false;
static volatile bool pending_stop = false;

/* Preview note state */
static volatile u16  pending_preview = 0;   /* note | (inst << 8), 0 = none */
static volatile bool pending_stop_preview = false;
static mm_sfxhand    preview_handle = 0;

/*
 * Tracks what kind of address to expect next in the address handler.
 * Set by the value handler when it sees MT_CMD_SET_SHARED or MT_CMD_SET_MAS.
 */
static volatile u8 pending_addr_type = 0;

/* --- maxmod event callback (runs in ARM7 tick context) --- */

/*
 * Called by maxmod every tick via mmSetEventHandler. We use MMCB_SONGTICK
 * to relay position information to ARM9, and update the shared state
 * position fields for low-latency reads.
 *
 * Callback param encoding for MMCB_SONGTICK:
 *   bits [7:0]  = layer (0=main, 1=jingle)
 *   bits [15:8] = tick
 *   bits [23:16]= row
 *   bits [31:24]= position (order)
 */
/* Track last reported order position to detect pattern boundaries */
static u8 last_reported_pos = 0xFF;

static mm_word mt_EventCallback(mm_word msg, mm_word param)
{
    if (msg == MMCB_SONGTICK)
    {
        u8 layer = param & 0xFF;

        /* Only report main layer, not jingle */
        if (layer != 0)
            return 0;

        u8 tick = (param >> 8) & 0xFF;
        u8 row  = (param >> 16) & 0xFF;
        u8 pos  = (param >> 24) & 0xFF;

        /* Update shared state in a single 32-bit store so ARM9 can never
         * read a torn (tick,row,position) combination during the callback. */
        if (mt_shared)
        {
            mt_shared->pos_state = MT_POS_PACK(pos, row, tick);
        }

        /* Detect pattern boundary: order position changed */
        if (pos != last_reported_pos)
        {
            /* Tell ARM9 to update shared_state.cells to the new pattern */
            fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_PATTERN_END, (u32)pos));
            last_reported_pos = pos;
        }

        /* Send tick message to ARM9 on tick 0 of each row (reduce FIFO traffic) */
        if (tick == 0)
        {
            u32 msg_val = MT_MKCMD(MT_CMD_TICK, (u32)pos | ((u32)row << 8));
            fifoSendValue32(FIFO_MT, msg_val);
        }
    }
    else if (msg == MMCB_SONGFINISHED)
    {
        if (mt_shared)
            mt_shared->playing = 0;

        fifoSendValue32(FIFO_MT, MT_MKCMD(MT_CMD_SONG_END, 0));
    }

    return 0;
}

/* --- FIFO Handlers (IRQ context) --- */

static void mt_ValueHandler(u32 value, void *userdata)
{
    (void)userdata;
    u8 cmd   = MT_CMD_TYPE(value);
    u32 param = MT_CMD_PARAM(value);

    switch (cmd) {
    case MT_CMD_PLAY:
        pending_play_pos = (u8)(param & 0xFF);
        pending_play_row = (u8)((param >> 8) & 0xFF);
        pending_play = true;
        break;

    case MT_CMD_STOP:
        pending_stop = true;
        break;

    case MT_CMD_SET_SHARED:
        /* Next address message will be the MT_SharedPatternState pointer */
        pending_addr_type = MT_CMD_SET_SHARED;
        break;

    case MT_CMD_SET_MAS:
        /* Next address message will be the MAS buffer pointer */
        pending_addr_type = MT_CMD_SET_MAS;
        break;

    case MT_CMD_PREVIEW_NOTE:
        pending_preview = (u16)(param & 0xFFFF);
        break;

    case MT_CMD_STOP_PREVIEW:
        pending_stop_preview = true;
        break;

    case MT_CMD_SET_TEMPO:
        /* Set BPM directly. Q10 format: param * 1024 / param = 1024 for 1x.
         * mmSetModuleTempo expects Q10 where 0x400 = 1.0x.
         * We receive raw BPM and need to scale relative to current tempo.
         * Simpler: just write directly. But mmSetModuleTempo is the safe API. */
        if (param > 0)
            mmSetModuleTempo(param * 1024 / 125);  /* scale relative to 125 BPM base */
        break;

    case MT_CMD_SET_SPEED:
        /* Speed is ticks per row — no direct API, but we can use effect Axx.
         * For now, leave as TODO — speed changes come via pattern effects. */
        break;

    case MT_CMD_SET_MUTE:
        /* Muting is handled via shared_state.mute_mask read in mmReadPattern */
        break;

    default:
        break;
    }
}

static void mt_AddressHandler(void *address, void *userdata)
{
    (void)userdata;

    u8 addr_type = pending_addr_type;
    pending_addr_type = 0;

    if (addr_type == MT_CMD_SET_SHARED)
    {
        mt_shared = (MT_SharedPatternState *)address;
    }
    else
    {
        /* Default: treat as MAS address */
        pending_mas_addr = (u32)address;
    }
}

/* --- IRQ handlers --- */

static void VblankHandler(void) {}

static void VcountHandler(void)
{
    inputGetAndSend();
}

static void powerButtonCB(void)
{
    exitflag = true;
}

/* --- Main loop --- */

int main(void)
{
    /* Clear sound registers */
    dmaFillWords(0, (void *)0x04000400, 0x100);

    /* Load NDS firmware personal data (touchscreen calibration, user
     * name, birthdate, etc.) into shared memory. Without this call,
     * touchReadXY() has no calibration reference points and returns
     * (0, 0) for every touch regardless of where the stylus actually
     * lands — buttons and KEY_TOUCH still fire correctly because those
     * come from REG_KEYXY, which doesn't need calibration. Every libnds
     * ARM7 example calls this as the very first thing after entry. */
    readUserSettings();

    irqInit();
    fifoInit();

    SetYtrigger(80);

    installSoundFIFO();

    /* Install MaxMod on ARM7 */
    mmInstall(FIFO_MAXMOD);

    /*
     * NOTE: Don't call mmSetEventHandler here — mmInit7() hasn't run yet.
     * It will be called asynchronously when ARM9 sends mmInit(), and
     * mmInit7() overwrites the event handler with mmEventForwarder.
     * We set our callback AFTER mmInit7 completes, in the main loop below.
     */

    /* Install maxtracker FIFO handlers */
    fifoSetValue32Handler(FIFO_MT, mt_ValueHandler, NULL);
    fifoSetAddressHandler(FIFO_MT, mt_AddressHandler, NULL);

    installSystemFIFO();

    irqSet(IRQ_VCOUNT, VcountHandler);
    irqSet(IRQ_VBLANK, VblankHandler);
    irqEnable(IRQ_VBLANK | IRQ_VCOUNT);

    setPowerButtonCB(powerButtonCB);

    /*
     * Wait for mmInit7 to complete (triggered asynchronously by ARM9's mmInit).
     * Then install our event callback, replacing mmEventForwarder.
     */
    while (!mmIsInitialized())
        swiWaitForVBlank();
    mmSetEventHandler(mt_EventCallback);

    while (!exitflag) {
        swiWaitForVBlank();

        /* --- Process pending commands (deferred from IRQ context) --- */

        if (pending_stop) {
            pending_stop = false;
            mmStop();
            if (mt_shared)
                mt_shared->playing = 0;
        }

        if (pending_mas_addr && !pending_play) {
            /*
             * MAS address received without explicit play — load and start.
             * This handles the case where SET_MAS + PLAY are sent together;
             * the MAS load happens first, PLAY happens next frame.
             */
            u32 addr = pending_mas_addr;
            pending_mas_addr = 0;
            mmStop();
            mmPlayMAS(addr, MM_PLAY_LOOP, 0);
            if (mt_shared)
                mt_shared->playing = 1;
        }

        if (pending_play) {
            pending_play = false;
            last_reported_pos = 0xFF;  /* reset so first tick triggers PATTERN_END */

            /* Reset the pattern position tracker in mmReadPattern so it
             * resolves the cells pointer on the very first tick. */
            extern mm_byte mt_last_position;
            mt_last_position = 0xFF;

            /* If there's a pending MAS address, load it first */
            if (pending_mas_addr) {
                u32 addr = pending_mas_addr;
                pending_mas_addr = 0;
                mmStop();
                mmPlayMAS(addr, MM_PLAY_LOOP, 0);
            }

            /* Jump to the requested order position + row */
            mmSetPositionEx(pending_play_pos, pending_play_row);
            pending_play_row = 0;

            if (mt_shared)
                mt_shared->playing = 1;
        }

        /* --- Preview note --- */

        if (pending_stop_preview) {
            pending_stop_preview = false;
            if (preview_handle) {
                mmEffectCancel(preview_handle);
                preview_handle = 0;
            }
        }

        if (pending_preview) {
            u16 pv = pending_preview;
            pending_preview = 0;

            u8 note = pv & 0xFF;
            u8 inst = (pv >> 8) & 0xFF;

            if (inst > 0 && note < 120) {
                /* Cancel any existing preview */
                if (preview_handle) {
                    mmEffectCancel(preview_handle);
                    preview_handle = 0;
                }

                /*
                 * Play a sample using mmEffectEx.
                 *
                 * Rate calculation: 1024 = base rate at C-4 (note 48).
                 * Each octave doubles the rate. Within an octave, use a
                 * semitone lookup table (Q10 fixed point).
                 */
                mm_sound_effect sfx;
                sfx.id      = inst - 1;  /* 0-based sample ID */
                sfx.rate    = 1024;
                sfx.handle  = 0;
                sfx.volume  = 255;
                sfx.panning = 128;

                int offset = (int)note - 48;
                int octave = offset / 12;
                int semi   = offset % 12;
                if (semi < 0) { semi += 12; octave--; }

                /* Q10 semitone multipliers: 1024 * 2^(n/12) for n=0..11 */
                static const u16 semi_rate[12] = {
                    1024, 1085, 1149, 1218, 1290, 1367,
                    1448, 1534, 1625, 1722, 1824, 1933
                };

                u32 rate = semi_rate[semi];

                if (octave > 0)
                    rate <<= octave;
                else if (octave < 0)
                    rate >>= (-octave);

                if (rate < 1) rate = 1;
                if (rate > 0xFFFF) rate = 0xFFFF;

                sfx.rate = (u16)rate;

                preview_handle = mmEffectEx(&sfx);
            }
        }
    }

    return 0;
}
