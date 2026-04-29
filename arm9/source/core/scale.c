#include "scale.h"

/*
 * Each scale is a 12-bit mask where bit N = 1 means semitone N is in the scale.
 * Semitone 0 = C (root), 1 = C#, 2 = D, etc.
 */
static const u16 scale_masks[MT_SCALE_COUNT] = {
    0xFFF,  /* Chromatic:   all 12 semitones                              */
    0xAB5,  /* Major:       C D E F G A B       = {0,2,4,5,7,9,11}       */
    0x5AD,  /* Minor (nat): C D Eb F G Ab Bb    = {0,2,3,5,7,8,10}       */
    0x295,  /* Pentatonic:  C D E G A           = {0,2,4,7,9}            */
    0x4E9,  /* Blues:        C Eb F F# G Bb      = {0,3,5,6,7,10}         */
    0x6AD,  /* Dorian:      C D Eb F G A Bb     = {0,2,3,5,7,9,10}       */
};

/*
 * Check whether a raw semitone (0-11) belongs to the given scale
 * (always relative to C, i.e. root = 0).
 */
bool mt_scale_contains(u8 scale_id, u8 semitone)
{
    if (scale_id >= MT_SCALE_COUNT) return false;
    semitone %= 12;
    return (scale_masks[scale_id] >> semitone) & 1;
}

/*
 * Helper: check if a MIDI note belongs to the scale with the given root.
 */
static bool note_in_scale(u8 scale_id, u8 root, u8 note)
{
    u8 rel = ((u8)(note - root + 12)) % 12;
    return (scale_masks[scale_id] >> rel) & 1;
}

/*
 * Snap a note to the nearest note in the scale (with given root).
 * On a tie, snaps downward.  Clamps result to 0-119.
 */
u8 mt_scale_snap(u8 scale_id, u8 root, u8 note)
{
    if (scale_id >= MT_SCALE_COUNT) return note;
    if (note_in_scale(scale_id, root, note)) return note;

    for (u8 offset = 1; offset <= 6; offset++) {
        bool lo = (note >= offset) && note_in_scale(scale_id, root, note - offset);
        bool hi = (note + offset <= 119) && note_in_scale(scale_id, root, note + offset);

        if (lo && hi) return note - offset;   /* tie: snap down */
        if (lo) return note - offset;
        if (hi) return note + offset;
    }
    return note;  /* fallback (shouldn't happen for valid scales) */
}

/*
 * Return the next note in the scale above `note`.  Clamps to 119.
 */
u8 mt_scale_next(u8 scale_id, u8 root, u8 note)
{
    if (scale_id >= MT_SCALE_COUNT) return (note < 119) ? note + 1 : 119;

    for (u8 n = note + 1; n <= 119; n++) {
        if (note_in_scale(scale_id, root, n)) return n;
    }
    return note;  /* already at top or no higher note in scale */
}

/*
 * Return the previous note in the scale below `note`.  Clamps to 0.
 */
u8 mt_scale_prev(u8 scale_id, u8 root, u8 note)
{
    if (scale_id >= MT_SCALE_COUNT) return (note > 0) ? note - 1 : 0;
    if (note == 0) return 0;

    for (u8 n = note - 1; ; n--) {
        if (note_in_scale(scale_id, root, n)) return n;
        if (n == 0) break;
    }
    return 0;  /* no lower note found */
}
