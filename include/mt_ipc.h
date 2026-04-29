/*
 * mt_ipc.h — Shared IPC definitions for maxtracker ARM7 <-> ARM9.
 * Included by both processors. No ARM-specific code.
 *
 * Commands are driven by two X-lists (MT_IPC_A9_CMDS, MT_IPC_A7_CMDS)
 * so the numeric code, the `MT_CMD_*` enum entry, and the debug name
 * stay in sync from a single edit site. The numeric codes are the wire
 * protocol between CPUs — NEVER change an existing value, only append.
 */

#ifndef MT_IPC_H
#define MT_IPC_H

#include <nds.h>

#define FIFO_MT  (FIFO_USER_08)

/* Value32 command encoding: bits[31:24]=type, bits[23:0]=param */
#define MT_MKCMD(cmd, param)   ((((u32)(cmd)) << 24) | ((u32)(param) & 0x00FFFFFFu))
#define MT_CMD_TYPE(val)       (((val) >> 24) & 0xFFu)
#define MT_CMD_PARAM(val)      ((val) & 0x00FFFFFFu)

/* ---- X-lists ----
 * Columns: NAME, CODE, DESCRIPTION
 * Numeric codes are locked by the wire protocol — append only.
 */

/* ARM9 -> ARM7 */
#define MT_IPC_A9_CMDS(X) \
    X(PLAY,         0x01, "param = order pos | (row << 8)")            \
    X(STOP,         0x02, "stop playback")                              \
    X(SET_SHARED,   0x03, "followed by fifoSendAddress(MT_SharedPatternState*)") \
    X(SET_MAS,      0x04, "followed by fifoSendAddress(MAS buffer*)")   \
    X(SET_PATTERN,  0x05, "param = pattern index")                      \
    X(PREVIEW_NOTE, 0x06, "param = note | (inst << 8)")                 \
    X(STOP_PREVIEW, 0x07, "cancel the active preview note")             \
    X(SET_MUTE,     0x08, "param = ch | (mute << 8)")                   \
    X(SET_TEMPO,    0x09, "param = BPM")                                \
    X(SET_SPEED,    0x0A, "param = ticks per row")                      \
    X(REBUILD_DONE, 0x0B, "MAS header rebuilt; ARM7 re-resolve ptrs")

/* ARM7 -> ARM9 */
#define MT_IPC_A7_CMDS(X) \
    X(TICK,         0x80, "param = pos | (row << 8)")                   \
    X(PATTERN_END,  0x81, "param = next order position")                \
    X(SONG_END,     0x82, "song reached end (order 0xFF)")

/* Opcode constants. Enum rather than #define so -Wswitch can flag
 * unhandled opcodes in dispatch tables. Values are fixed by the wire
 * protocol — see the X-lists above. */
enum {
#define X(name, code, desc) MT_CMD_##name = (code),
    MT_IPC_A9_CMDS(X)
    MT_IPC_A7_CMDS(X)
#undef X
    MT_CMD__SENTINEL = 0  /* keep the trailing comma well-formed */
};

/* Debug name lookup. static inline so both CPUs can use it without a
 * separate .c — the bodies are tiny and only pulled in where called. */
static inline const char *mt_cmd_name(u8 code)
{
    switch (code) {
#define X(name, c, desc) case (c): return #name;
    MT_IPC_A9_CMDS(X)
    MT_IPC_A7_CMDS(X)
#undef X
    default: return "?";
    }
}

static inline const char *mt_cmd_description(u8 code)
{
    switch (code) {
#define X(name, c, desc) case (c): return (desc);
    MT_IPC_A9_CMDS(X)
    MT_IPC_A7_CMDS(X)
#undef X
    default: return "";
    }
}

/* MAS prefix size (mmutil -d prepends 8 bytes before MAS header) */
#define MAS_PREFIX_SIZE     8

#endif /* MT_IPC_H */
