/*
 * navigation_test.c — Host-native tests for navigation.c's SHIFT-chord
 * dispatcher.
 *
 * `navigation_handle_shift(down, held)` is the gatekeeper every view
 * calls first. Its public contract is:
 *
 *   - returns true  → it consumed the chord; the view must NOT run its
 *                     own input handler for this frame.
 *   - returns false → nothing was consumed; the view runs as normal.
 *
 * The class of bug we've been bitten by is "returns true for a SHIFT
 * chord that isn't actually bound on this screen" — SELECT+B on a
 * non-pattern screen used to be silently eaten, breaking per-view
 * SELECT+B bindings (the "restore original" gesture in LFE, for
 * example). This test drives a representative table of
 * `(from_screen, chord, expected_handled, expected_to_screen)` tuples
 * against the real navigation.c, with the UI side stubbed out.
 *
 * What's intentionally NOT tested here:
 *   - Actual rendering (the stubs are no-ops).
 *   - Playback-state mutations — playback.h is shimmed.
 *   - LFE transitions — MAXTRACKER_LFE is intentionally not defined
 *     so we don't pull the LFE chain into the host build.
 *
 * Build: make -C test navigation_test
 * Run:   ./navigation_test
 */

#include "nds_shim.h"
#include "navigation.h"
#include "screen.h"
#include "editor_state.h"
#include "song.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- UI stubs needed by navigation.c ----
 *
 * On-device these live in screen.c / font.c / view files. For the host
 * test we provide the minimum surface area navigation_handle_shift
 * actually touches: the screen-mode variable, framebuffer pointers
 * (dummies — never dereferenced here because font_clear is a no-op),
 * and the sample-view helpers the horizontal-navigation branch calls.
 *
 * The real versions live outside the host build; keeping these stubs
 * here (rather than in a shared file) makes the contract between this
 * test and navigation.c explicit and local. */

static u8 dummy_top_fb[1];
static u8 dummy_bot_fb[1];
u8 *top_fb = dummy_top_fb;
u8 *bot_fb = dummy_bot_fb;

ScreenMode current_screen = SCREEN_PATTERN;

void screen_set_mode(ScreenMode m) { current_screen = m; }

/* Font / screen stubs — navigation.c only ever calls font_clear. */
int FONT_COLS = 64;
int FONT_ROWS = 32;
void font_clear(u8 *fb, u8 bg) { (void)fb; (void)bg; }

/* sample_view helpers: navigation.c uses _set_selected when transitioning
 * Instrument→Sample, and _get_selected when transitioning Sample→LFE
 * (the latter is behind MAXTRACKER_LFE, so _get_selected may be unused). */
static u8 sv_selected;
void sample_view_set_selected(u8 i) { sv_selected = i; }
u8   sample_view_get_selected(void) { return sv_selected; }

/* Disk browser routing globals — navigation.c references them on
 * SHIFT+DOWN from the disk screen. No behavior needed beyond storage. */
ScreenMode disk_return_screen = SCREEN_PROJECT;
u8         sample_load_target = 0;

/* Dirty flags — defined in main.c on-device; editor_state.c's
 * mt_mark_song_modified() writes them, so the host build needs real
 * symbols for the linker. navigation_test doesn't read them. */
bool song_modified;
bool autosave_dirty;

/* Clipboard + undo — navigation.c's SHIFT+A paste path references these.
 * Stub away the real behavior; the tests don't drive the paste path. */
#include "clipboard.h"
MT_Clipboard clipboard;
bool clipboard_has_block(void) { return false; }
void clipboard_paste(MT_Pattern *p, u8 r, u8 c) { (void)p; (void)r; (void)c; }

#include "undo.h"
/* The real undo.c is linked for other tests; for this binary we don't
 * use it, but the symbols need to resolve. Provide trivial definitions
 * that navigation.c's SHIFT+A paste might touch. */
void undo_push_block(u8 p, u16 rs, u16 re, u8 cs, u8 ce)
{ (void)p; (void)rs; (void)re; (void)cs; (void)ce; }

/* ---- test scaffolding ---- */

/* Reset global state between cases so one test's fallout doesn't
 * contaminate the next. */
static void reset_world(ScreenMode from_screen)
{
    current_screen = from_screen;
    memset(&cursor, 0, sizeof(cursor));
    cursor.instrument = 1;
    cursor.octave = 4;
    play_mode = PLAY_STOPPED;
    disk_return_screen = SCREEN_PROJECT;
    sample_load_target = 0;

    /* Pattern data for the contextual-instrument scan (Inside→Instrument
     * transition reads the current pattern). */
    song_init();
}

static void teardown(void)
{
    song_free();
}

/* ---- named cases ---- */

/*
 * SHIFT+RIGHT from Song → Pattern overview. Classic horizontal nav.
 * `handled` must be true and the screen must transition.
 */
static void test_shift_right_song_to_pattern(void)
{
    reset_world(SCREEN_SONG);
    bool h = navigation_handle_shift(KEY_RIGHT, KEY_SELECT | KEY_RIGHT);
    CHECK(h == true, "SHIFT+RIGHT on SONG: handled");
    CHECK(current_screen == SCREEN_PATTERN,
          "SHIFT+RIGHT on SONG: landed on PATTERN (got %d)", current_screen);
    teardown();
}

/*
 * SHIFT+LEFT from Pattern Overview → Song. Reverses the above.
 * cursor.inside must be false (we're in overview) for this route.
 */
static void test_shift_left_pattern_to_song(void)
{
    reset_world(SCREEN_PATTERN);
    cursor.inside = false;
    bool h = navigation_handle_shift(KEY_LEFT, KEY_SELECT | KEY_LEFT);
    CHECK(h == true, "SHIFT+LEFT on PATTERN overview: handled");
    CHECK(current_screen == SCREEN_SONG,
          "SHIFT+LEFT on PATTERN: landed on SONG (got %d)", current_screen);
    teardown();
}

/*
 * SHIFT+UP from Song → Project. The vertical-nav branch.
 */
static void test_shift_up_song_to_project(void)
{
    reset_world(SCREEN_SONG);
    bool h = navigation_handle_shift(KEY_UP, KEY_SELECT | KEY_UP);
    CHECK(h == true, "SHIFT+UP on SONG: handled");
    CHECK(current_screen == SCREEN_PROJECT,
          "SHIFT+UP on SONG: landed on PROJECT (got %d)", current_screen);
    teardown();
}

/*
 * SHIFT+DOWN from Project → Song. Reverses the above via DOWN-from-project.
 */
static void test_shift_down_project_to_song(void)
{
    reset_world(SCREEN_PROJECT);
    bool h = navigation_handle_shift(KEY_DOWN, KEY_SELECT | KEY_DOWN);
    CHECK(h == true, "SHIFT+DOWN on PROJECT: handled");
    CHECK(current_screen == SCREEN_SONG,
          "SHIFT+DOWN on PROJECT: landed on SONG (got %d)", current_screen);
    teardown();
}

/*
 * SHIFT+DOWN from Disk → returns to disk_return_screen (not nav_return),
 * per the routing-globals contract (see architecture.md § 9b).
 */
static void test_shift_down_disk_returns_to_opener(void)
{
    reset_world(SCREEN_DISK);
    disk_return_screen = SCREEN_SAMPLE;
    sample_load_target = 3;

    bool h = navigation_handle_shift(KEY_DOWN, KEY_SELECT | KEY_DOWN);
    CHECK(h == true, "SHIFT+DOWN on DISK: handled");
    CHECK(current_screen == SCREEN_SAMPLE,
          "SHIFT+DOWN on DISK: returned to disk_return_screen (got %d)",
          current_screen);
    CHECK(sample_load_target == 0,
          "SHIFT+DOWN on DISK: sample_load_target cleared (got %d)",
          sample_load_target);
    teardown();
}

/*
 * **Regression coverage for the "SELECT eats SHIFT+B" bug.**
 *
 * SHIFT+B is only bound on Pattern view (enters selection mode), and
 * only when selection isn't already active. On any other screen,
 * SHIFT+B must return false so the per-view handler can own the chord
 * (LFE binds SHIFT+B as "restore pristine original"). The previous
 * bug had handle_shift returning true unconditionally whenever SHIFT
 * was held, eating the per-view chord.
 */
static void test_shift_b_on_lfe_falls_through(void)
{
    /* Use SCREEN_SAMPLE as proxy for "any non-PATTERN screen" since
     * SCREEN_LFE is behind MAXTRACKER_LFE and not in this build. The
     * contract is the same. */
    reset_world(SCREEN_SAMPLE);
    bool h = navigation_handle_shift(KEY_B, KEY_SELECT | KEY_B);
    CHECK(h == false,
          "SHIFT+B on non-PATTERN screen: fell through (got handled=%d)", h);
    CHECK(current_screen == SCREEN_SAMPLE,
          "SHIFT+B on non-PATTERN: screen unchanged");
    teardown();
}

/*
 * SHIFT+B on Pattern view (not yet selecting) enters selection mode
 * AND returns handled so the per-view B-handling doesn't also run.
 */
static void test_shift_b_on_pattern_enters_selection(void)
{
    reset_world(SCREEN_PATTERN);
    cursor.selecting = false;
    bool h = navigation_handle_shift(KEY_B, KEY_SELECT | KEY_B);
    CHECK(h == true, "SHIFT+B on PATTERN: handled");
    CHECK(cursor.selecting == true,
          "SHIFT+B on PATTERN: entered selection mode");
    teardown();
}

/*
 * No SHIFT held: handle_shift must be inert — no handled, no screen
 * change, no cursor mutation.
 */
static void test_no_shift_is_inert(void)
{
    reset_world(SCREEN_PATTERN);
    ScreenMode before = current_screen;
    bool h = navigation_handle_shift(KEY_A, KEY_A);    /* A only, no SELECT */
    CHECK(h == false, "A without SHIFT: not handled");
    CHECK(current_screen == before, "A without SHIFT: no screen change");
    teardown();
}

/*
 * Unmapped SHIFT chord: SHIFT+L on any screen isn't bound anywhere.
 * Must return false so the per-view handler can use SHIFT+L if it
 * wants (not currently used, but reserved).
 */
static void test_unmapped_shift_chord_falls_through(void)
{
    reset_world(SCREEN_PATTERN);
    bool h = navigation_handle_shift(KEY_L, KEY_SELECT | KEY_L);
    CHECK(h == false,
          "SHIFT+L: unmapped chord falls through (got handled=%d)", h);
    teardown();
}

int main(void)
{
    printf("=== navigation host tests ===\n");

    test_shift_right_song_to_pattern();
    test_shift_left_pattern_to_song();
    test_shift_up_song_to_project();
    test_shift_down_project_to_song();
    test_shift_down_disk_returns_to_opener();
    test_shift_b_on_lfe_falls_through();
    test_shift_b_on_pattern_enters_selection();
    test_no_shift_is_inert();
    test_unmapped_shift_chord_falls_through();

    printf("--- %d/%d passed", g_total - g_failed, g_total);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf(" ---\n");
    return g_failed == 0 ? 0 : 1;
}
