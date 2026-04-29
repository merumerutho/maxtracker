/*
 * disk_view.h — Disk-screen flow (file browser, WAV/MAS load, sample save).
 *
 * Houses the state shared with the views that open the browser
 * (sample_view for load/save, project_view for module load/save). Before
 * switching to SCREEN_DISK, openers set sample_load_target or
 * sample_save_target and disk_return_screen to control routing.
 */

#ifndef MT_DISK_VIEW_H
#define MT_DISK_VIEW_H

#include <nds.h>
#include <stdbool.h>

#include "screen.h"
#include "filebrowser.h"

/* Shared browser state. Persists across screen switches so opening the
 * browser again from elsewhere resumes at the last directory. */
extern FileBrowser disk_browser;
extern bool        disk_browser_inited;

/* When > 0, the next .wav loaded is routed into
 * song.samples[sample_load_target-1] (set by SAMPLE's LOAD action).
 * 0 means use the legacy cursor.instrument path. */
extern u8 sample_load_target;

/* When > 0, the disk browser is in SAVE mode for this sample slot.
 * Mutually exclusive with sample_load_target. */
extern u8 sample_save_target;

/* Screen to return to when the user exits the browser with B at root. */
extern ScreenMode disk_return_screen;

/* Reset transient disk-view state (overwrite modal, save target).
 * Called from navigation.c on every DISK exit path so stale state
 * doesn't survive across screen transitions. */
void disk_view_cleanup(void);

/* Run one frame of disk-screen input. May transition to another screen. */
void disk_view_input(u32 kd, u32 kh);

/* Draw the disk screen (file list on top, hints/modal on bottom). */
void disk_view_draw(u8 *top, u8 *bot);

#endif /* MT_DISK_VIEW_H */
