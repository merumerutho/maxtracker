#ifndef MT_SCALE_H
#define MT_SCALE_H

#include <nds.h>
#include <stdbool.h>

#define MT_SCALE_CHROMATIC  0
#define MT_SCALE_MAJOR      1
#define MT_SCALE_MINOR      2
#define MT_SCALE_PENTATONIC 3
#define MT_SCALE_BLUES      4
#define MT_SCALE_DORIAN     5
#define MT_SCALE_COUNT      6

bool mt_scale_contains(u8 scale_id, u8 semitone);
u8   mt_scale_snap(u8 scale_id, u8 root, u8 note);
u8   mt_scale_next(u8 scale_id, u8 root, u8 note);
u8   mt_scale_prev(u8 scale_id, u8 root, u8 note);

#endif
