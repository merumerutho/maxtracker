/*
 * filebrowser.c — SD card file browser for maxtracker.
 */

#include "filebrowser.h"
#include "font.h"
#include "screen.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Default filesystem browse root — see filebrowser.h for docs. */
const char *fs_browse_root = "./data/";

/* Static buffer for building full paths */
static char path_buf[FB_PATH_LEN + FB_NAME_LEN];

/* ---- Sorting helpers ---- */

/*
 * Case-insensitive character comparison.
 */
static int char_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/*
 * Case-insensitive string comparison.
 */
static int stricmp_local(const char *a, const char *b)
{
    while (*a && *b) {
        int diff = char_lower((unsigned char)*a) - char_lower((unsigned char)*b);
        if (diff != 0) return diff;
        a++;
        b++;
    }
    return char_lower((unsigned char)*a) - char_lower((unsigned char)*b);
}

/*
 * Simple insertion sort for the entry list.
 * Directories come first, then files. Within each group, sorted alphabetically.
 */
static void sort_entries(FileBrowser *fb)
{
    for (int i = 1; i < fb->entry_count; i++) {
        char   tmp_name[FB_NAME_LEN];
        bool   tmp_dir;
        u32    tmp_size;

        memcpy(tmp_name, fb->entries[i], FB_NAME_LEN);
        tmp_dir  = fb->is_dir[i];
        tmp_size = fb->entry_size[i];

        int j = i - 1;
        while (j >= 0) {
            /* Directories before files */
            if (tmp_dir && !fb->is_dir[j]) {
                /* tmp is dir, j is file -> tmp goes before j */
            } else if (!tmp_dir && fb->is_dir[j]) {
                /* tmp is file, j is dir -> j stays before tmp */
                break;
            } else {
                /* Same type: compare alphabetically */
                if (stricmp_local(tmp_name, fb->entries[j]) >= 0)
                    break;
            }

            /* Shift j forward */
            memcpy(fb->entries[j + 1], fb->entries[j], FB_NAME_LEN);
            fb->is_dir[j + 1]     = fb->is_dir[j];
            fb->entry_size[j + 1] = fb->entry_size[j];
            j--;
        }

        memcpy(fb->entries[j + 1], tmp_name, FB_NAME_LEN);
        fb->is_dir[j + 1]     = tmp_dir;
        fb->entry_size[j + 1] = tmp_size;
    }
}

/*
 * Check if a filename has one of the recognized extensions.
 */
static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return stricmp_local(dot, ext) == 0;
}

/*
 * Build full path from browser path + entry name.
 */
static const char *build_path(const char *dir, const char *name)
{
    int dlen = strlen(dir);
    /* Ensure trailing slash */
    if (dlen > 0 && dir[dlen - 1] == '/') {
        snprintf(path_buf, sizeof(path_buf), "%s%s", dir, name);
    } else {
        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, name);
    }
    return path_buf;
}

/* ---- Public API ---- */

/* Read the current fb->path into the entry list. Called both from the
 * public init (first entry) and from the subdirectory-navigation code
 * inside filebrowser_input. Preserves root_path so at-root detection
 * still works after the user has descended into subdirectories. */
static void filebrowser_load_dir(FileBrowser *fb)
{
    DIR *dp;
    struct dirent *de;
    struct stat st;

    /* Reset entry list but keep path + root_path intact. */
    memset(fb->entries, 0, sizeof(fb->entries));
    memset(fb->is_dir, 0, sizeof(fb->is_dir));
    memset(fb->entry_size, 0, sizeof(fb->entry_size));
    fb->entry_count = 0;
    fb->cursor = 0;
    fb->sv.scroll = 0;
    fb->sv.margin = 0;   /* edge-follow: cursor lands at top/bottom, not centered */

    /* Remove trailing slash unless it's the root */
    int plen = strlen(fb->path);
    if (plen > 1 && fb->path[plen - 1] == '/') {
        fb->path[plen - 1] = '\0';
    }

    dp = opendir(fb->path);
    if (!dp) return;

    while ((de = readdir(dp)) != NULL && fb->entry_count < FB_MAX_ENTRIES) {
        /* Skip . and hidden files */
        if (de->d_name[0] == '.')
            continue;

        int idx = fb->entry_count;
        strncpy(fb->entries[idx], de->d_name, FB_NAME_LEN - 1);
        fb->entries[idx][FB_NAME_LEN - 1] = '\0';

        /* Stat the entry to determine type and size */
        build_path(fb->path, de->d_name);
        if (stat(path_buf, &st) == 0) {
            fb->is_dir[idx] = S_ISDIR(st.st_mode);
            fb->entry_size[idx] = fb->is_dir[idx] ? 0 : (u32)st.st_size;
        } else {
            fb->is_dir[idx] = false;
            fb->entry_size[idx] = 0;
        }

        fb->entry_count++;
    }

    closedir(dp);

    sort_entries(fb);
}

void filebrowser_init(FileBrowser *fb, const char *start_path)
{
    memset(fb, 0, sizeof(FileBrowser));
    strncpy(fb->path, start_path, FB_PATH_LEN - 1);
    fb->path[FB_PATH_LEN - 1] = '\0';
    strncpy(fb->root_path, start_path, FB_PATH_LEN - 1);
    fb->root_path[FB_PATH_LEN - 1] = '\0';
    fb->mode = FB_MODE_LOAD;   /* explicit — memset already zeroed it */
    fb->save_requested = false;
    filebrowser_load_dir(fb);
}

void filebrowser_set_mode(FileBrowser *fb, FileBrowserMode mode)
{
    fb->mode = mode;
    fb->save_requested = false;
}

bool filebrowser_take_save_request(FileBrowser *fb)
{
    if (!fb->save_requested) return false;
    fb->save_requested = false;
    return true;
}

void filebrowser_draw(FileBrowser *fb, u8 *framebuf)
{
    font_clear(framebuf, PAL_BG);

    /* ---- Header row (row 0) ---- */
    font_fill_row(framebuf, 0, 0, FONT_COLS, PAL_HEADER_BG);
    if (fb->mode == FB_MODE_SAVE) {
        font_puts(framebuf, 1, 0, "FILE BROWSER — SAVE MODE", PAL_ORANGE);
        /* Footer hint on the right — "START: save here". Keep it on
         * the same header row since the disk-screen bottom screen
         * owns the footer row itself. */
        int hint_col = FONT_COLS - 16;
        if (hint_col > 0)
            font_puts(framebuf, hint_col, 0, "START: save here", PAL_WHITE);
    } else {
        font_puts(framebuf, 1, 0, "FILE BROWSER", PAL_WHITE);
    }

    /* ---- Current path (row 1) ---- */
    font_fill_row(framebuf, 1, 0, FONT_COLS, PAL_BG);

    /* Truncate path display to fit screen */
    const char *disp_path = fb->path;
    int pathlen = strlen(fb->path);
    if (pathlen > FONT_COLS - 2) {
        disp_path = fb->path + pathlen - (FONT_COLS - 5);
        font_puts(framebuf, 1, 1, "...", PAL_DIM);
        font_puts(framebuf, 4, 1, disp_path, PAL_GRAY);
    } else {
        font_puts(framebuf, 1, 1, disp_path, PAL_GRAY);
    }

    /* ---- File list (rows 2..30) ---- */
    int list_start_row = 2;
    fb->sv.row_y       = list_start_row;
    fb->sv.row_height  = FB_VISIBLE_ROWS;
    fb->sv.total       = fb->entry_count;
    fb->sv.cursor      = fb->cursor;
    scroll_view_follow(&fb->sv);

    if (fb->entry_count == 0) {
        font_puts(framebuf, 2, list_start_row, "(empty)", PAL_DIM);
        return;
    }

    int fb_first, fb_last;
    scroll_view_visible(&fb->sv, &fb_first, &fb_last);

    for (int idx = fb_first; idx < fb_last; idx++) {
        int row = fb->sv.row_y + (idx - fb->sv.scroll);

        /* Highlight cursor row */
        if (idx == fb->cursor) {
            font_fill_row(framebuf, row, 0, FONT_COLS, PAL_ROW_CURSOR);
        }

        u8 name_color;
        if (fb->is_dir[idx]) {
            name_color = PAL_NOTE;   /* Directories in a distinct color */
        } else if (has_ext(fb->entries[idx], ".mas")) {
            name_color = PAL_INST;
        } else if (has_ext(fb->entries[idx], ".wav") ||
                   has_ext(fb->entries[idx], ".raw")) {
            name_color = PAL_EFFECT;
        } else {
            name_color = PAL_TEXT;
        }

        /* Directory indicator */
        if (fb->is_dir[idx]) {
            font_putc(framebuf, 1, row, '/', name_color);
        } else {
            font_putc(framebuf, 1, row, ' ', PAL_BG);
        }

        /* Entry name -- truncate to 28 chars with "..." */
        int name_len = strlen(fb->entries[idx]);
        if (name_len > 28) {
            char trunc[32];
            memcpy(trunc, fb->entries[idx], 25);
            trunc[25] = '.';
            trunc[26] = '.';
            trunc[27] = '.';
            trunc[28] = '\0';
            font_puts(framebuf, 2, row, trunc, name_color);
        } else {
            font_puts(framebuf, 2, row, fb->entries[idx], name_color);
        }

        /* File size for .mas and .wav files */
        if (!fb->is_dir[idx] &&
            (has_ext(fb->entries[idx], ".mas") ||
             has_ext(fb->entries[idx], ".wav") ||
             has_ext(fb->entries[idx], ".raw"))) {
            u32 sz = fb->entry_size[idx];
            font_printf(framebuf, FONT_COLS - 10, row, PAL_DIM,
                        "%4luK", (unsigned long)((sz + 512) / 1024));
        }
    }

    /* Scrollbar on the rightmost grid column — only drawn when the
     * content overflows the viewport. Replaces the old ^/v arrow hints. */
    scroll_view_draw_scrollbar(&fb->sv, framebuf, FONT_COLS - 1);

    /* Status row: N/M position count (rightmost column reserved for
     * the scrollbar). */
    int status_row = list_start_row + FB_VISIBLE_ROWS;
    if (status_row < FONT_ROWS) {
        font_fill_row(framebuf, status_row, 0, FONT_COLS, PAL_BG);
        if (fb->entry_count > FB_VISIBLE_ROWS) {
            font_printf(framebuf, 1, status_row, PAL_DIM,
                        "%d/%d", fb->cursor + 1, fb->entry_count);
        }
    }
}

bool filebrowser_input(FileBrowser *fb, u32 down)
{
    if (fb->entry_count == 0) {
        /* Only allow B (go up) when empty — and only when we're not
         * already at the configured root. Without this guard, an
         * empty root directory lets B walk above root_path; once
         * that happens, filebrowser_at_root() never returns true
         * again and the caller's B-exit shortcut is permanently
         * broken. */
        if ((down & KEY_B) && !filebrowser_at_root(fb)) {
            char *slash = strrchr(fb->path, '/');
            if (slash && slash != fb->path) {
                *slash = '\0';
                filebrowser_load_dir(fb);
            }
        }
        return false;
    }

    /* Cursor nav — scroll is reconciled in filebrowser_draw via
     * scroll_view_follow, using fb->sv.margin == 0 for edge-follow
     * (cursor lands at the top or bottom of the visible window when
     * it crosses the edge). */
    u32 rep = keysDownRepeat();
    if (rep & KEY_UP) {
        if (fb->cursor > 0) fb->cursor--;
    }
    if (rep & KEY_DOWN) {
        if (fb->cursor < fb->entry_count - 1) fb->cursor++;
    }
    if (rep & KEY_LEFT) {
        /* Page up */
        fb->cursor -= FB_VISIBLE_ROWS;
        if (fb->cursor < 0) fb->cursor = 0;
    }
    if (rep & KEY_RIGHT) {
        /* Page down */
        fb->cursor += FB_VISIBLE_ROWS;
        if (fb->cursor >= fb->entry_count)
            fb->cursor = fb->entry_count - 1;
    }

    if (down & KEY_A) {
        if (fb->is_dir[fb->cursor]) {
            /* Enter directory. Update fb->path in place and reload the
             * listing. root_path stays intact so at-root detection
             * still works after descending. */
            build_path(fb->path, fb->entries[fb->cursor]);
            strncpy(fb->path, path_buf, FB_PATH_LEN - 1);
            fb->path[FB_PATH_LEN - 1] = '\0';
            filebrowser_load_dir(fb);
            return false;
        } else {
            /* File selected. In SAVE mode, A on a file is a no-op —
             * save destinations are composed from fb->path + caller's
             * filename, not picked from the listing. The caller uses
             * START (via filebrowser_take_save_request) to commit. */
            if (fb->mode == FB_MODE_SAVE)
                return false;
            return true;
        }
    }

    /* START in SAVE mode is "save into current directory". The caller
     * picks this up via filebrowser_take_save_request() next frame. */
    if ((down & KEY_START) && fb->mode == FB_MODE_SAVE) {
        fb->save_requested = true;
    }

    if (down & KEY_B) {
        /* Go up one directory level */
        char *slash = strrchr(fb->path, '/');
        if (slash && slash != fb->path) {
            *slash = '\0';
            filebrowser_load_dir(fb);
        }
    }

    return false;
}

const char *filebrowser_selected_path(FileBrowser *fb)
{
    if (fb->entry_count == 0 || fb->cursor < 0 || fb->cursor >= fb->entry_count)
        return NULL;

    build_path(fb->path, fb->entries[fb->cursor]);
    return path_buf;
}

bool filebrowser_at_root(const FileBrowser *fb)
{
    /* Compare current path against the path the browser was init'd at,
     * with any trailing slash stripped from root_path for a stable
     * comparison (load_dir strips trailing slash from fb->path too). */
    char rp[FB_PATH_LEN];
    strncpy(rp, fb->root_path, sizeof(rp) - 1);
    rp[sizeof(rp) - 1] = '\0';
    int n = (int)strlen(rp);
    if (n > 1 && rp[n - 1] == '/') rp[n - 1] = '\0';
    return strcmp(fb->path, rp) == 0;
}
