/*
 * keybind.c — Runtime key binding preset management.
 *
 * Preset definitions live in presets/<name>.def — one file per preset.
 * Each .def file contains X(action, key) lines that are expanded here
 * into static const arrays via the X_INIT macro.
 */

#include "keybind.h"
#include <string.h>

/* ---- Preset tables --------------------------------------------------- */

#define X_INIT(action, key) [MT_ACT_##action] = (key),

static const u32 map_default[MT_ACT_COUNT] = {
#define X X_INIT
#include "presets/default.def"
#undef X
};

static const u32 map_nds_fat[MT_ACT_COUNT] = {
#define X X_INIT
#include "presets/nds_fat.def"
#undef X
};

static const u32 map_nds_lite[MT_ACT_COUNT] = {
#define X X_INIT
#include "presets/nds_lite.def"
#undef X
};

#undef X_INIT

static const u32 *preset_maps[MT_PRESET_COUNT] = {
    [MT_PRESET_DEFAULT]  = map_default,
    [MT_PRESET_NDS_FAT]  = map_nds_fat,
    [MT_PRESET_NDS_LITE] = map_nds_lite,
};

/* Preset display labels. */
static const char *preset_labels[MT_PRESET_COUNT] = {
#define X(name, label) [MT_PRESET_##name] = label,
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
