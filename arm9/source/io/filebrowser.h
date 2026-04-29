/*
 * filebrowser.h — SD card file browser for maxtracker.
 *
 * POSIX directory browsing via libfat. Directories listed first,
 * then files, both sorted alphabetically. Supports .mas, .wav, .raw
 * file types for loading.
 */

#ifndef MT_FILEBROWSER_H
#define MT_FILEBROWSER_H

#include <nds.h>
#include <stdbool.h>

#include "../ui/scroll_view.h"

#define FB_MAX_ENTRIES  64
#define FB_NAME_LEN     32
#define FB_PATH_LEN     256

/* Visible rows on screen (FONT_ROWS minus header/status lines).
 * Computed as FONT_ROWS - 3: one header row, one path row, one status
 * row. Runtime value so the browser tracks SMALL/BIG font modes. */
#include "../ui/font.h"
static inline int fb_visible_rows(void) { return FONT_ROWS - 3; }
#define FB_VISIBLE_ROWS fb_visible_rows()

/*
 * Browser mode.
 *
 *   FB_MODE_LOAD — default. A on a file returns "file selected"; the
 *                  caller reads filebrowser_selected_path() to get the
 *                  full path, then loads it.
 *   FB_MODE_SAVE — picking a save destination. A on a file does
 *                  nothing (you can't save onto an existing file by
 *                  highlighting it — you'd compose a filename from
 *                  elsewhere, e.g. the current sample name, and write
 *                  into the current directory). START signals
 *                  "save here"; the caller polls
 *                  filebrowser_take_save_request() to detect it.
 *
 * Mode only affects input semantics and the footer hint — navigation,
 * sorting, and scroll are identical.
 */
typedef enum {
    FB_MODE_LOAD = 0,
    FB_MODE_SAVE = 1,
} FileBrowserMode;

typedef struct {
    char path[FB_PATH_LEN];             /* Current directory path */
    char root_path[FB_PATH_LEN];        /* Path the browser was initialized at */
    char entries[FB_MAX_ENTRIES][FB_NAME_LEN]; /* Entry names */
    u32  entry_size[FB_MAX_ENTRIES];    /* File size (0 for dirs) */
    int  entry_count;                   /* Number of entries */
    int  cursor;                        /* Currently highlighted entry */
    ScrollView sv;                      /* Viewport + scroll state (margin, row_y, row_height set per draw) */
    bool is_dir[FB_MAX_ENTRIES];        /* true if entry is a directory */
    FileBrowserMode mode;               /* LOAD (default) or SAVE */
    bool save_requested;                /* set when START pressed in SAVE mode;
                                           consumed by filebrowser_take_save_request */
} FileBrowser;

/*
 * filebrowser_init — Open a directory and populate the entry list.
 *
 * @param fb          Browser state to initialize.
 * @param start_path  Directory path to open (e.g., "fat:/maxtracker").
 */
void filebrowser_init(FileBrowser *fb, const char *start_path);

/*
 * filebrowser_draw — Render the file list onto a framebuffer.
 *
 * @param fb       Browser state.
 * @param framebuf  8-bit indexed framebuffer (256x192).
 */
void filebrowser_draw(FileBrowser *fb, u8 *framebuf);

/*
 * filebrowser_input — Handle d-pad and button input.
 *
 * @param fb    Browser state.
 * @param down  Button mask from keysDown() (KEY_UP, KEY_DOWN, KEY_A, KEY_B, etc.)
 *
 * @return  true if user selected a file (A on a non-directory entry).
 */
bool filebrowser_input(FileBrowser *fb, u32 down);

/*
 * filebrowser_selected_path — Get the full path of the selected entry.
 *
 * Returns a pointer to a static buffer containing the full path.
 * Valid until the next call to this function or filebrowser_init.
 */
const char *filebrowser_selected_path(FileBrowser *fb);

/*
 * filebrowser_at_root — Is the current directory the initial/root path?
 * Used so callers can map B to "exit the browser" only at the root
 * level, leaving B = "go up one dir" inside subdirectories.
 */
bool filebrowser_at_root(const FileBrowser *fb);

/*
 * filebrowser_set_mode — Switch the browser between LOAD and SAVE mode.
 *
 * Called right after filebrowser_init by openers that want save
 * semantics. Clears any pending save_requested flag. Safe to call
 * repeatedly.
 */
void filebrowser_set_mode(FileBrowser *fb, FileBrowserMode mode);

/*
 * filebrowser_take_save_request — SAVE mode only. Returns true if
 * START was pressed since the last call, and clears the flag.
 *
 * The caller is expected to poll this each frame while the browser
 * is up. On true, compose a target path from fb->path plus whatever
 * filename your flow chose and perform the write (with overwrite
 * confirmation if needed — filebrowser itself doesn't do that).
 */
bool filebrowser_take_save_request(FileBrowser *fb);

/* Root path for filesystem browsing. Defaults to "./data/" (FAT on
 * SD card). main.c updates this to "./" when NitroFS is in use.
 * Used by the disk screen's file browser and LFE's save-as-WAV dialog
 * to know where to start navigating. */
extern const char *fs_browse_root;

#endif /* MT_FILEBROWSER_H */
