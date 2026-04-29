/*
 * filebrowser_test.c — Host-native tests for the disk filebrowser.
 *
 * Exercises the public API (filebrowser_init / _input / _selected_path /
 * _at_root) against a scratch directory tree. No NDS hardware.
 *
 * The filebrowser module has some architectural surface area that
 * matters for v1 correctness:
 *   - Alphabetical sort with directories first
 *   - at_root detection after descending / ascending
 *   - B-at-root clamp (regression test for the "empty root lockout" bug)
 *   - selected_path composition
 *
 * Build: make -C test filebrowser_test
 * Run:   ./filebrowser_test
 */

#include "filebrowser.h"
#include "nds_shim.h"

#include <dirent.h>
#include <errno.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- font stubs ----
 *
 * filebrowser.c compiles filebrowser_draw() unconditionally; it
 * references the font API and FONT_COLS/ROWS. This test only
 * exercises filebrowser_init / _input / _selected_path / _at_root,
 * so the draw path is never called — but the symbols still need to
 * resolve at link time. Stubs are no-ops. If filebrowser_draw grows
 * new font_* calls, add matching stubs here. */
int FONT_COLS = 64;
int FONT_ROWS = 32;

void font_putc(u8 *fb, int col, int row, char ch, u8 color)
{ (void)fb; (void)col; (void)row; (void)ch; (void)color; }

int font_puts(u8 *fb, int col, int row, const char *s, u8 color)
{ (void)fb; (void)col; (void)row; (void)s; (void)color; return 0; }

void font_fill_row(u8 *fb, int row, int col_start, int col_end, u8 bg)
{ (void)fb; (void)row; (void)col_start; (void)col_end; (void)bg; }

void font_clear(u8 *fb, u8 bg) { (void)fb; (void)bg; }

int font_printf(u8 *fb, int col, int row, u8 color, const char *fmt, ...)
{ (void)fb; (void)col; (void)row; (void)color; (void)fmt; return 0; }

/* ---- minimal test harness ---- */

static int g_total  = 0;
static int g_failed = 0;

#define CHECK(cond, ...) do { \
    g_total++; \
    if (!(cond)) { \
        g_failed++; \
        fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* ---- scratch-tree helpers ---- */

static const char *ROOT = "fb_test_root";

/* mkdir that works on Windows (mingw) — mode is ignored there. */
static int mkdir_portable(const char *path)
{
#ifdef _WIN32
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static void touch(const char *path, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (size_t i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}

/* rm -rf equivalent for our test tree (limited depth; we control it). */
static void rm_rf(const char *path)
{
    DIR *dp = opendir(path);
    if (!dp) {
        unlink(path);
        return;
    }
    struct dirent *de;
    char buf[512];
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        snprintf(buf, sizeof(buf), "%s/%s", path, de->d_name);
        rm_rf(buf);
    }
    closedir(dp);
    rmdir(path);
}

static void build_populated_tree(void)
{
    rm_rf(ROOT);
    mkdir_portable(ROOT);

    /* Directories first so we can check the dir-before-file sort rule. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/songs", ROOT);
    mkdir_portable(dir);
    snprintf(dir, sizeof(dir), "%s/samples", ROOT);
    mkdir_portable(dir);

    /* Files at root — intentionally out of alpha order on disk so we
     * exercise sort_entries. */
    char file[256];
    snprintf(file, sizeof(file), "%s/zeta.mas", ROOT);  touch(file, 10);
    snprintf(file, sizeof(file), "%s/alpha.mas", ROOT); touch(file, 10);
    snprintf(file, sizeof(file), "%s/MiXeD.wav", ROOT); touch(file, 10);

    /* One file inside a subdir so we can descend + reascend. */
    snprintf(file, sizeof(file), "%s/songs/inside.mas", ROOT);
    touch(file, 10);
}

static void build_empty_tree(void)
{
    rm_rf(ROOT);
    mkdir_portable(ROOT);
}

/* ---- tests ---- */

static void test_sort_alpha_dirs_first(void)
{
    build_populated_tree();

    FileBrowser fb;
    filebrowser_init(&fb, ROOT);

    CHECK(fb.entry_count == 5,
          "expected 5 entries, got %d", fb.entry_count);

    /* First two must be the directories. */
    CHECK(fb.is_dir[0] && fb.is_dir[1],
          "first two entries are directories");
    /* Directories alpha-sorted: samples < songs */
    CHECK(strcmp(fb.entries[0], "samples") == 0,
          "entry[0] = samples, got %s", fb.entries[0]);
    CHECK(strcmp(fb.entries[1], "songs") == 0,
          "entry[1] = songs, got %s", fb.entries[1]);

    /* Files alpha-sorted (case-insensitive): MiXeD then alpha then zeta
     * — no, alpha < MiXeD case-insensitively (a < m). Order:
     * alpha.mas, MiXeD.wav, zeta.mas. */
    CHECK(!fb.is_dir[2] && !fb.is_dir[3] && !fb.is_dir[4],
          "entries 2..4 are files");
    CHECK(strcmp(fb.entries[2], "alpha.mas") == 0,
          "entry[2] = alpha.mas, got %s", fb.entries[2]);
    CHECK(strcmp(fb.entries[3], "MiXeD.wav") == 0,
          "entry[3] = MiXeD.wav (case-insensitive sort), got %s",
          fb.entries[3]);
    CHECK(strcmp(fb.entries[4], "zeta.mas") == 0,
          "entry[4] = zeta.mas, got %s", fb.entries[4]);
}

static void test_at_root_after_init_and_descent(void)
{
    build_populated_tree();

    FileBrowser fb;
    filebrowser_init(&fb, ROOT);
    CHECK(filebrowser_at_root(&fb), "at_root after init");

    /* Cursor on 'songs' (entry 1) → A to descend. */
    fb.cursor = 1;
    bool file_selected = filebrowser_input(&fb, KEY_A);
    CHECK(!file_selected, "descending dir returns false, not file-select");
    CHECK(!filebrowser_at_root(&fb), "not at_root after descent");

    /* B climbs back up → at_root again. */
    filebrowser_input(&fb, KEY_B);
    CHECK(filebrowser_at_root(&fb), "at_root after climb back up");
}

static void test_selected_path_composition(void)
{
    build_populated_tree();

    FileBrowser fb;
    filebrowser_init(&fb, ROOT);

    /* Cursor on 'alpha.mas' (entry 2). */
    fb.cursor = 2;
    const char *sel = filebrowser_selected_path(&fb);
    CHECK(sel != NULL, "selected_path not NULL");
    /* Path should end with /alpha.mas. */
    size_t slen = strlen(sel);
    CHECK(slen >= strlen("alpha.mas"),
          "selected_path long enough");
    CHECK(strcmp(sel + slen - strlen("alpha.mas"), "alpha.mas") == 0,
          "selected_path ends with alpha.mas (got %s)", sel);
}

/*
 * Regression: empty-root B-lockout.
 *
 * Before the fix, pressing B inside an empty root directory let
 * filebrowser_input walk above the configured root. After that,
 * filebrowser_at_root() never returned true again, and the caller's
 * B-exit shortcut was permanently broken. This test boots with an
 * empty root, presses B, and confirms at_root is still true.
 */
static void test_empty_root_b_lockout(void)
{
    build_empty_tree();

    FileBrowser fb;
    filebrowser_init(&fb, ROOT);
    CHECK(fb.entry_count == 0, "empty root has 0 entries");
    CHECK(filebrowser_at_root(&fb), "empty root reports at_root");

    /* Press B. */
    filebrowser_input(&fb, KEY_B);

    CHECK(filebrowser_at_root(&fb),
          "still at_root after B in empty root (lockout regression)");
}

int main(void)
{
    printf("=== filebrowser host tests ===\n");

    test_sort_alpha_dirs_first();
    test_at_root_after_init_and_descent();
    test_selected_path_composition();
    test_empty_root_b_lockout();

    rm_rf(ROOT);

    printf("--- %d/%d passed", g_total - g_failed, g_total);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf(" ---\n");
    return g_failed == 0 ? 0 : 1;
}
