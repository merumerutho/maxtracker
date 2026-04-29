/*
 * playback.h — ARM9 playback controller for maxtracker.
 *
 * Manages communication with ARM7 for module playback, position tracking,
 * note preview, and channel muting. Builds the minimal MAS header that
 * maxmod needs for instrument/sample data while patterns are read live
 * from shared RAM by the patched mmReadPattern on ARM7.
 */

#ifndef MT_PLAYBACK_H
#define MT_PLAYBACK_H

#include <nds.h>
#include <stdbool.h>

/*
 * Initialize the playback subsystem.
 *
 * - Allocates and zeroes the MT_SharedPatternState.
 * - Sends its address to ARM7 via FIFO.
 * - Initializes maxmod on the ARM9 side (mmInitNoSoundbank).
 *
 * Call once at startup, after fifoInit() and maxmod ARM9 init.
 */
void playback_init(void);

/*
 * Start playback from the given order position.
 *
 * Before calling: the song must have valid patterns, instruments, and samples.
 * This function:
 * 1. Builds a minimal MAS header from the current song state.
 * 2. Points mt_shared->cells at the first pattern's cell data.
 * 3. Flushes cache.
 * 4. Sends the MAS address and play command to ARM7.
 */
void playback_play(u8 order_pos);

/*
 * Like playback_play() but starts at `row` within the target pattern.
 * Uses maxmod's mmSetPositionEx fast-forward internally.
 */
void playback_play_at(u8 order_pos, u8 row);

/*
 * Stop playback.
 */
void playback_stop(void);

/*
 * Per-frame update. Call from the main loop each VBlank.
 *
 * Processes incoming FIFO messages from ARM7 (tick position updates,
 * pattern end notifications). Updates internal position state.
 */
void playback_update(void);

/*
 * Query playback state.
 */
bool playback_is_playing(void);
u8   playback_get_row(void);
u8   playback_get_order(void);
u8   playback_get_tick(void);

/*
 * Preview a single note (plays on a free channel, independent of sequencer).
 *
 * @param note  Note number (0-119), or NOTE_CUT/NOTE_OFF.
 * @param inst  Instrument number (1-based).
 */
void playback_preview_note(u8 note, u8 inst);

/*
 * Stop any currently playing preview note.
 */
void playback_stop_preview(void);

/*
 * Mute or unmute a channel. Muted channels are skipped by mmReadPattern.
 *
 * @param channel  Channel number (0-based).
 * @param mute     true = mute, false = unmute.
 */
void playback_set_mute(u8 channel, bool mute);

/*
 * Get the current mute bitmask (bit N = 1 means channel N is muted).
 */
u32 playback_get_mute_mask(void);

/*
 * Set the playback tempo (BPM) on ARM7 without rebuilding the MAS header.
 * Only effective while playback is active.
 *
 * @param bpm  Tempo in BPM (32-255).
 */
void playback_set_tempo(u8 bpm);

/*
 * Rebuild the MAS header after song data changes (instruments, samples,
 * order table, pattern count). Call this after modifying instrument or
 * sample data. Not needed for pattern cell edits (those are read live).
 *
 * If playback is active, it will be briefly interrupted while the header
 * is rebuilt.
 */
void playback_rebuild_mas(void);

/*
 * Start playback of a single pattern in a loop (for LSDJ-style
 * pattern preview). Temporarily overrides the order table to contain
 * only `pattern_idx`, builds the MAS buffer, starts playback, then
 * restores the original arrangement. The resulting MAS loops the
 * pattern via repeat_position = 0.
 */
void playback_play_pattern(u8 pattern_idx);

/*
 * Like playback_play_pattern() but starts at `row` within the pattern.
 */
void playback_play_pattern_at(u8 pattern_idx, u8 row);

/* Mark pattern cells as modified (triggers cache flush on next update) */
void playback_mark_cells_dirty(void);

/* Temporarily detach the shared cells pointer (ARM7 will read nothing).
 * Call before freeing or reallocating any pattern that might be active.
 * Call playback_reattach_pattern() after to restore it. */
void playback_detach_pattern(void);

/* Reattach the shared cells pointer to the current order's pattern. */
void playback_reattach_pattern(void);

/* Refresh the shared order + pattern lookup tables.
 * Call after editing orders, adding/removing/reallocating patterns,
 * or any change that affects which cells pointer a pattern index maps to.
 * Safe to call during playback. */
void playback_refresh_shared_tables(void);

#endif /* MT_PLAYBACK_H */
