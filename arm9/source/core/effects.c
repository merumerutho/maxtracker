#include "effects.h"
#include <stddef.h>

static const MT_EffectInfo fx_table[] = {
#define FX(code, letter, mnem, supported, name, desc) \
    { (code), (letter), (mnem), (supported), (name), (desc) },
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

const char *effect_mnem(u8 fx)
{
    if (fx == 0) return "---";
    const MT_EffectInfo *e = effect_info(fx);
    return e ? e->mnem : "???";
}

const char *effect_name(u8 fx)
{
    const MT_EffectInfo *e = effect_info(fx);
    return e ? e->name : NULL;
}

bool effect_supported(u8 fx)
{
    const MT_EffectInfo *e = effect_info(fx);
    return e ? (e->supported != 0) : false;
}

u8 effect_code_min(void)
{
    u8 lo = 0xFF;
    for (size_t i = 0; i < FX_COUNT; i++)
        if (fx_table[i].code < lo) lo = fx_table[i].code;
    return (lo == 0xFF) ? 0 : lo;
}

u8 effect_code_max(void)
{
    u8 hi = 0;
    for (size_t i = 0; i < FX_COUNT; i++)
        if (fx_table[i].code > hi) hi = fx_table[i].code;
    return hi;
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
