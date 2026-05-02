/*
 * keybind.h — Configurable key binding system for maxtracker.
 *
 * Maps 8 semantic actions to physical NDS keys via runtime-switchable
 * presets.  The active mapping lives in mt_keymap[], a flat u32[8]
 * array that fits in a single ARM9 cache line.  Views read it through
 * the MT_KEY_* convenience macros defined below.
 *
 * Presets are defined in presets/<stem>.def, one file per preset.
 * Each .def defines a PRESET_MAP_<stem>(X) macro.  Adding a new
 * preset requires a new .def file, an entry in KEYBIND_PRESETS
 * below, and a corresponding #include in keybind.c.
 */

#ifndef MT_KEYBIND_H
#define MT_KEYBIND_H

#include <nds.h>

/* ---- Semantic actions ------------------------------------------------
 *
 * KEYBIND_ACTIONS(X)   X(enum_suffix, display_label)
 */
#define KEYBIND_ACTIONS(X)              \
    X(CONFIRM,       "Confirm")         \
    X(BACK,          "Back")            \
    X(MOD_PRIMARY,   "Primary")         \
    X(MOD_SECONDARY, "Secondary")       \
    X(SHOULDER_L,    "Left Shldr")      \
    X(SHOULDER_R,    "Right Shldr")     \
    X(START,         "Start")           \
    X(SHIFT,         "Shift")

typedef enum {
#define X(name, label) MT_ACT_##name,
    KEYBIND_ACTIONS(X)
#undef X
    MT_ACT_COUNT
} MT_Action;

/* ---- Presets ---------------------------------------------------------
 *
 * KEYBIND_PRESETS(X)   X(enum_suffix, display_label, def_stem)
 *
 * def_stem must match the PRESET_MAP_<stem> macro defined in the
 * corresponding presets/<stem>.def file.
 */
#define KEYBIND_PRESETS(X)                      \
    X(DEFAULT,  "Default",  default)            \
    X(NDS_FAT,  "NDS Fat",  nds_fat)           \
    X(NDS_LITE, "NDS Lite", nds_lite)

typedef enum {
#define X(name, label, stem) MT_PRESET_##name,
    KEYBIND_PRESETS(X)
#undef X
    MT_PRESET_COUNT
} MT_KeyPreset;

/* ---- Runtime keymap --------------------------------------------------
 *
 * Populated by keybind_set_preset().  Each slot holds the KEY_xxx
 * bitmask for the corresponding MT_ACT_* action.
 */
extern u32 mt_keymap[MT_ACT_COUNT];

/* ---- Convenience macros ----------------------------------------------
 *
 * Use these in input handlers instead of raw KEY_A / KEY_B / etc.
 * They resolve to an array load — one extra cycle per check on ARM9.
 */
#define MT_KEY_CONFIRM       mt_keymap[MT_ACT_CONFIRM]
#define MT_KEY_BACK          mt_keymap[MT_ACT_BACK]
#define MT_KEY_MOD_PRIMARY   mt_keymap[MT_ACT_MOD_PRIMARY]
#define MT_KEY_MOD_SECONDARY mt_keymap[MT_ACT_MOD_SECONDARY]
#define MT_KEY_SHOULDER_L    mt_keymap[MT_ACT_SHOULDER_L]
#define MT_KEY_SHOULDER_R    mt_keymap[MT_ACT_SHOULDER_R]
#define MT_KEY_START         mt_keymap[MT_ACT_START]
#define MT_KEY_SHIFT         mt_keymap[MT_ACT_SHIFT]

/* ---- API ------------------------------------------------------------- */

void         keybind_set_preset(MT_KeyPreset preset);
MT_KeyPreset keybind_get_preset(void);
const char  *keybind_preset_name(MT_KeyPreset preset);

#endif /* MT_KEYBIND_H */
