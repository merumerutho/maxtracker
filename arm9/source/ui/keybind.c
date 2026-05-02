/*
 * keybind.c — Runtime key binding preset management.
 *
 * Preset definitions live in presets/<stem>.def — one file per preset.
 * Each .def defines a PRESET_MAP_<stem>(X) macro that is expanded here
 * into static const arrays via KEYBIND_PRESETS.
 */

#include "keybind.h"
#include <string.h>

#include "presets/default.def"
#include "presets/nds_fat.def"
#include "presets/nds_lite.def"

/* ---- Preset tables --------------------------------------------------- */

#define X_SLOT(action, key) [MT_ACT_##action] = (key),
#define X_MAP(name, label, stem) \
    static const u32 map_##stem[MT_ACT_COUNT] = { PRESET_MAP_##stem(X_SLOT) };
KEYBIND_PRESETS(X_MAP)
#undef X_MAP
#undef X_SLOT

static const u32 *preset_maps[MT_PRESET_COUNT] = {
#define X(name, label, stem) [MT_PRESET_##name] = map_##stem,
    KEYBIND_PRESETS(X)
#undef X
};

static const char *preset_labels[MT_PRESET_COUNT] = {
#define X(name, label, stem) [MT_PRESET_##name] = label,
    KEYBIND_PRESETS(X)
#undef X
};

/* ---- State ----------------------------------------------------------- */

u32 mt_keymap[MT_ACT_COUNT];
static MT_KeyPreset current_preset = MT_PRESET_DEFAULT;

/* ---- API ------------------------------------------------------------- */

void keybind_set_preset(MT_KeyPreset preset)
{
    if (preset >= MT_PRESET_COUNT) preset = MT_PRESET_DEFAULT;
    current_preset = preset;
    memcpy(mt_keymap, preset_maps[preset], sizeof(mt_keymap));
}

MT_KeyPreset keybind_get_preset(void)
{
    return current_preset;
}

const char *keybind_preset_name(MT_KeyPreset preset)
{
    if (preset >= MT_PRESET_COUNT) return "?";
    return preset_labels[preset];
}
