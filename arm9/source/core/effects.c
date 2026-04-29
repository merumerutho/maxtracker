#include "effects.h"
#include <stddef.h>

static const MT_EffectInfo fx_table[] = {
#define FX(code, letter, name, desc) { (code), (letter), (name), (desc) },
#include "effects.def"
#undef FX
};

#define FX_COUNT ((size_t)(sizeof(fx_table) / sizeof(fx_table[0])))

const MT_EffectInfo *effect_info(u8 fx)
{
    if (fx == 0) return NULL;
    for (size_t i = 0; i < FX_COUNT; i++)
        if (fx_table[i].code == fx) return &fx_table[i];
    return NULL;
}

char effect_letter(u8 fx)
{
    if (fx == 0) return '\0';
    const MT_EffectInfo *e = effect_info(fx);
    return e ? e->letter : '?';
}

const char *effect_name(u8 fx)
{
    const MT_EffectInfo *e = effect_info(fx);
    return e ? e->name : NULL;
}

const char *effect_description(u8 fx)
{
    const MT_EffectInfo *e = effect_info(fx);
    return e ? e->description : NULL;
}

u8 effect_from_letter(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < FX_COUNT; i++)
        if (fx_table[i].letter == c) return fx_table[i].code;
    return 0;
}
