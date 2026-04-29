/*
 * effects.h — lookup API for tracker effects.
 *
 * Backed by effects.def (X-macro). To add an effect: edit effects.def,
 * then regenerate doc/effects.md from the same list.
 */

#ifndef MT_EFFECTS_H
#define MT_EFFECTS_H

#include <nds.h>

typedef struct {
    u8          code;         /* 1-26; matches MT_Cell.fx */
    char        letter;       /* 'A'-'Z' */
    const char *name;         /* short label, e.g. "Set Speed" */
    const char *description;  /* one-line param semantics */
} MT_EffectInfo;

/* Returns NULL when fx == 0 (no effect) or the code is unknown. */
const MT_EffectInfo *effect_info(u8 fx);

/* '\0' when fx == 0; '?' when fx is unknown; otherwise the letter. */
char        effect_letter(u8 fx);
const char *effect_name(u8 fx);
const char *effect_description(u8 fx);

/* Inverse lookup. Accepts lowercase. Returns 0 when no match. */
u8 effect_from_letter(char c);

#endif /* MT_EFFECTS_H */
