/*
 * test.c — Unit tests for maxtracker core modules (NDS ARM9).
 *
 * Tests are grouped by module: song model, clipboard, undo, MAS roundtrip.
 * Run on-device by holding SELECT during boot.
 */

#include "test.h"
#include "song.h"
#include "clipboard.h"
#include "undo.h"
#include "mas_write.h"
#include "mas_load.h"
#include "scale.h"

#include <stdlib.h>
#include <string.h>

MT_TestResults test_results;

/* ------------------------------------------------------------------ */
/* T1.1: Song Model Tests                                              */
/* ------------------------------------------------------------------ */

static void test_song_init_defaults(void)
{
    song_init();

    MT_ASSERT_EQ(song.initial_speed, 6, "speed==6");
    MT_ASSERT_EQ(song.initial_tempo, 125, "tempo==125");
    MT_ASSERT_EQ(song.global_volume, 64, "global_volume==64");
    MT_ASSERT_EQ(song.repeat_position, 0, "repeat_position==0");
    MT_ASSERT_EQ(song.channel_count, 32, "channel_count==32");
    MT_ASSERT(song.freq_linear == true, "freq_linear==true");
    MT_ASSERT(song.xm_mode == true, "xm_mode==true");
    MT_ASSERT_EQ(song.order_count, 1, "order_count==1");
    MT_ASSERT_EQ(song.orders[0], 0, "orders[0]==0");
    MT_ASSERT_EQ(song.patt_count, 1, "patt_count==1");
    MT_ASSERT_EQ(song.inst_count, 1, "inst_count==1");
    MT_ASSERT(strcmp(song.name, "untitled") == 0, "name==untitled");

    for (int i = 0; i < MT_MAX_CHANNELS; i++) {
        MT_ASSERT_EQ(song.channel_volume[i], 64, "ch_vol==64");
        MT_ASSERT_EQ(song.channel_panning[i], 128, "ch_pan==128");
    }

    MT_ASSERT(song.patterns[0] != NULL, "pat[0] allocated");
    MT_ASSERT_EQ(song.patterns[0]->nrows, 64, "pat[0] nrows==64");

    /* Verify pattern 0 cells are empty */
    MT_Pattern *pat = song.patterns[0];
    for (int r = 0; r < (int)pat->nrows; r++) {
        for (int c = 0; c < MT_MAX_CHANNELS; c++) {
            if (MT_CELL(pat, r, c)->note != NOTE_EMPTY ||
                MT_CELL(pat, r, c)->inst != 0 ||
                MT_CELL(pat, r, c)->vol  != 0 ||
                MT_CELL(pat, r, c)->fx   != 0 ||
                MT_CELL(pat, r, c)->param != 0) {
                MT_ASSERT(0, "pat[0] cell not empty");
                goto init_done;
            }
        }
    }
    test_results.total++;
    test_results.passed++;  /* all cells empty */

init_done:
    song_free();
}

static void test_song_ensure_pattern_alloc(void)
{
    song_init();

    MT_ASSERT(song.patterns[5] == NULL, "pat[5] starts NULL");

    MT_Pattern *pat = song_ensure_pattern(5);
    MT_ASSERT(pat != NULL, "pat[5] allocated");
    MT_ASSERT_EQ(pat->nrows, 64, "pat[5] nrows==64");
    MT_ASSERT(song.patt_count >= 6, "patt_count>=6");

    /* All cells must be empty */
    MT_ASSERT_EQ(MT_CELL(pat, 0, 0)->note, NOTE_EMPTY, "pat[5] cell empty");

    song_free();
}

static void test_song_ensure_pattern_idempotent(void)
{
    song_init();

    MT_Pattern *pat1 = song_ensure_pattern(5);
    MT_Pattern *pat2 = song_ensure_pattern(5);
    MT_ASSERT(pat1 == pat2, "ensure_pattern idempotent");

    /* Verify data survives second call */
    MT_CELL(pat1, 0, 0)->note = 60;
    MT_CELL(pat1, 0, 0)->inst = 1;
    MT_Pattern *pat3 = song_ensure_pattern(5);
    MT_ASSERT(pat3 == pat1, "same pointer on third call");
    MT_ASSERT_EQ(MT_CELL(pat3, 0, 0)->note, 60, "data preserved");
    MT_ASSERT_EQ(MT_CELL(pat3, 0, 0)->inst, 1, "inst preserved");

    song_free();
}

static void test_song_free_cleanup(void)
{
    song_init();

    song_ensure_pattern(0);
    song_ensure_pattern(3);
    song_ensure_pattern(10);

    song_free();

    for (int i = 0; i < MT_MAX_PATTERNS; i++) {
        if (song.patterns[i] != NULL) {
            MT_ASSERT(0, "pattern not freed");
            return;
        }
    }
    test_results.total++;
    test_results.passed++;
}

static void test_cell_size(void)
{
    MT_ASSERT_EQ((int)sizeof(MT_Cell), 5, "sizeof(MT_Cell)==5");
}

/* ------------------------------------------------------------------ */
/* T1.2: Clipboard Tests                                               */
/* ------------------------------------------------------------------ */

static void test_clipboard_copy_paste_single(void)
{
    song_init();
    MT_Pattern *pat = song.patterns[0];

    /* Set a cell */
    MT_CELL(pat, 5, 2)->note = 48;
    MT_CELL(pat, 5, 2)->inst = 3;
    MT_CELL(pat, 5, 2)->vol  = 40;

    /* Copy it */
    clipboard_copy_cell(pat, 5, 2);
    MT_ASSERT(clipboard.type == CLIP_CELLS == true, "cb valid after copy");
    MT_ASSERT_EQ(clipboard.cell_rows, 1, "cb rows==1");
    MT_ASSERT_EQ(clipboard.cell_channels, 1, "cb channels==1");
    MT_ASSERT_EQ(clipboard.cell_data[0].note, 48, "cb note==48");
    MT_ASSERT_EQ(clipboard.cell_data[0].inst, 3, "cb inst==3");

    /* Paste to another location */
    clipboard_paste(pat, 20, 3);
    MT_ASSERT_EQ(MT_CELL(pat, 20, 3)->note, 48, "paste note==48");
    MT_ASSERT_EQ(MT_CELL(pat, 20, 3)->inst, 3, "paste inst==3");
    MT_ASSERT_EQ(MT_CELL(pat, 20, 3)->vol, 40, "paste vol==40");

    clipboard_free();
    song_free();
}

static void test_clipboard_copy_paste_block(void)
{
    song_init();
    MT_Pattern *pat = song.patterns[0];

    /* Fill a 4-row x 2-channel block with known data */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 2; c++) {
            MT_CELL(pat, 10 + r, 4 + c)->note = (u8)(60 + r * 2 + c);
            MT_CELL(pat, 10 + r, 4 + c)->inst = (u8)(r * 2 + c + 1);
        }
    }

    /* Copy rows 10-13, channels 4-5 */
    clipboard_copy(pat, 10, 13, 4, 5);
    MT_ASSERT(clipboard.type == CLIP_CELLS == true, "block cb valid");
    MT_ASSERT_EQ(clipboard.cell_rows, 4, "block cb rows==4");
    MT_ASSERT_EQ(clipboard.cell_channels, 2, "block cb channels==2");

    /* Verify clipboard data */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 2; c++) {
            int idx = r * 2 + c;
            MT_ASSERT_EQ(clipboard.cell_data[idx].note,
                         (u8)(60 + r * 2 + c), "block cb note");
            MT_ASSERT_EQ(clipboard.cell_data[idx].inst,
                         (u8)(r * 2 + c + 1), "block cb inst");
        }
    }

    /* Paste to a different location and verify */
    clipboard_paste(pat, 30, 0);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 2; c++) {
            MT_ASSERT_EQ(MT_CELL(pat, 30 + r, 0 + c)->note,
                         (u8)(60 + r * 2 + c), "block paste note");
            MT_ASSERT_EQ(MT_CELL(pat, 30 + r, 0 + c)->inst,
                         (u8)(r * 2 + c + 1), "block paste inst");
        }
    }

    clipboard_free();
    song_free();
}

/* ------------------------------------------------------------------ */
/* T1.3: Undo Tests                                                    */
/* ------------------------------------------------------------------ */

static void test_undo_push_pop_single(void)
{
    song_init();
    undo_init();
    MT_Pattern *pat = song.patterns[0];

    /* Set initial value */
    MT_CELL(pat, 10, 3)->note = 60;
    MT_CELL(pat, 10, 3)->inst = 1;

    /* Push state (captures note=60, inst=1) */
    undo_push_cell(0, 10, 3);

    /* Modify the cell */
    MT_CELL(pat, 10, 3)->note = 72;
    MT_CELL(pat, 10, 3)->inst = 5;

    /* Pop should restore original */
    bool result = undo_pop(&song);
    MT_ASSERT(result == true, "undo_pop returns true");
    MT_ASSERT_EQ(MT_CELL(pat, 10, 3)->note, 60, "undo restores note");
    MT_ASSERT_EQ(MT_CELL(pat, 10, 3)->inst, 1, "undo restores inst");

    undo_free();
    song_free();
}

static void test_undo_ring_overflow(void)
{
    song_init();
    undo_init();
    MT_Pattern *pat = song.patterns[0];

    /* Push 80 edits (exceeds ring size of 64) */
    for (int i = 0; i < 80; i++) {
        u8 row = (u8)(i % 64);
        MT_CELL(pat, row, 0)->note = NOTE_EMPTY;
        undo_push_cell(0, row, 0);
        MT_CELL(pat, row, 0)->note = (u8)(i + 1);
    }

    /* Should be able to pop the most recent 64 */
    int count = 0;
    while (undo_pop(&song)) {
        count++;
    }
    MT_ASSERT_EQ(count, 64, "ring overflow: popped 64");

    /* Further pops should return false */
    MT_ASSERT(undo_pop(&song) == false, "ring empty after drain");

    undo_free();
    song_free();
}

/* Block push + block pop: snapshot a 3×3 area, scramble it, undo,
 * verify every cell was restored. Catches row/channel arithmetic
 * bugs in undo_push_block / undo_pop that single-cell coverage misses. */
static void test_undo_block_push_pop(void)
{
    song_init();
    undo_init();
    MT_Pattern *pat = song.patterns[0];

    /* Seed a 3×3 block at rows 4..6, channels 2..4 */
    for (u16 r = 4; r <= 6; r++) {
        for (u8 c = 2; c <= 4; c++) {
            MT_CELL(pat, r, c)->note = (u8)(r * 10 + c);
            MT_CELL(pat, r, c)->inst = (u8)(r + c);
        }
    }

    undo_push_block(0, 4, 6, 2, 4);

    /* Scramble the block. */
    for (u16 r = 4; r <= 6; r++)
        for (u8 c = 2; c <= 4; c++)
            MT_CELL(pat, r, c)->note = 0xFF;

    MT_ASSERT(undo_pop(&song) == true, "block undo_pop returns true");

    /* Every cell must match the pre-scramble state. */
    bool all_ok = true;
    for (u16 r = 4; r <= 6 && all_ok; r++) {
        for (u8 c = 2; c <= 4 && all_ok; c++) {
            if (MT_CELL(pat, r, c)->note != (u8)(r * 10 + c) ||
                MT_CELL(pat, r, c)->inst != (u8)(r + c))
                all_ok = false;
        }
    }
    MT_ASSERT(all_ok, "block undo restored every cell");

    /* Cells outside the block should be unaffected. */
    MT_ASSERT_EQ(MT_CELL(pat, 0, 0)->note, NOTE_EMPTY,
                 "undo didn't touch cells outside the block");

    undo_free();
    song_free();
}

/* A push followed by the pattern slot being nulled out (e.g. the user
 * deleted the pattern via compact). The pop must not crash and must
 * signal failure rather than corrupt memory by writing to a NULL
 * pattern pointer. */
static void test_undo_pop_with_null_pattern(void)
{
    song_init();
    undo_init();

    MT_CELL(song.patterns[0], 5, 1)->note = 60;
    undo_push_cell(0, 5, 1);

    /* Remove the pattern under the undo entry's feet. */
    free(song.patterns[0]);
    song.patterns[0] = NULL;

    /* Pop must return false, not crash. */
    bool result = undo_pop(&song);
    MT_ASSERT(result == false, "undo_pop on NULL pattern returns false");

    undo_free();
    song_free();
}

/* Exercise the row-variant push (undo_push_block collapsing to 1 row
 * across the full channel count). Catches channel-count bounds bugs
 * in the block path when width == MT_MAX_CHANNELS. */
static void test_undo_full_row_block(void)
{
    song_init();
    undo_init();
    MT_Pattern *pat = song.patterns[0];
    u8 ncols = pat->ncols;

    for (u8 c = 0; c < ncols; c++)
        MT_CELL(pat, 0, c)->note = (u8)(100 + c);

    undo_push_block(0, 0, 0, 0, (u8)(ncols - 1));

    for (u8 c = 0; c < ncols; c++)
        MT_CELL(pat, 0, c)->note = 0;

    MT_ASSERT(undo_pop(&song) == true, "full-row undo_pop returns true");

    bool all_ok = true;
    for (u8 c = 0; c < ncols && all_ok; c++) {
        if (MT_CELL(pat, 0, c)->note != (u8)(100 + c))
            all_ok = false;
    }
    MT_ASSERT(all_ok, "full-row undo restored every channel");

    undo_free();
    song_free();
}

/* ------------------------------------------------------------------ */
/* T1.4: MAS Roundtrip Tests                                           */
/* ------------------------------------------------------------------ */

#ifdef ARM9
#define TEST_MAS_PATH_EMPTY "fat:/maxtracker/_test_empty.mas"
#define TEST_MAS_PATH_DATA  "fat:/maxtracker/_test_data.mas"
#else
#define TEST_MAS_PATH_EMPTY "_test_empty.mas"
#define TEST_MAS_PATH_DATA  "_test_data.mas"
#endif

static void test_mas_roundtrip_empty(void)
{
    song_init();

    /* Save expected values before writing */
    u8 exp_speed  = song.initial_speed;
    u8 exp_tempo  = song.initial_tempo;
    u8 exp_gvol   = song.global_volume;
    u8 exp_repeat = song.repeat_position;
    u8 exp_orders = song.order_count;
    u8 exp_patt   = song.patt_count;
    bool exp_freq = song.freq_linear;
    bool exp_xm   = song.xm_mode;
    u8 exp_chvol[MT_MAX_CHANNELS];
    u8 exp_chpan[MT_MAX_CHANNELS];
    for (int i = 0; i < MT_MAX_CHANNELS; i++) {
        exp_chvol[i] = song.channel_volume[i];
        exp_chpan[i] = song.channel_panning[i];
    }

    int rc = mas_write(TEST_MAS_PATH_EMPTY, &song);
    if (rc != 0) {
        iprintf("SKIP: mas_write failed (%d), no SD?\n", rc);
        song_free();
        return;
    }

    /* Load into the global song (mas_load calls song_free internally) */
    rc = mas_load(TEST_MAS_PATH_EMPTY, &song);
    MT_ASSERT_EQ(rc, 0, "mas_load succeeds");
    if (rc != 0) {
        song_free();
        return;
    }

    MT_ASSERT_EQ(song.initial_speed, exp_speed, "rt speed");
    MT_ASSERT_EQ(song.initial_tempo, exp_tempo, "rt tempo");
    MT_ASSERT_EQ(song.global_volume, exp_gvol, "rt gvol");
    MT_ASSERT_EQ(song.repeat_position, exp_repeat, "rt repeat");
    MT_ASSERT_EQ(song.order_count, exp_orders, "rt order_count");
    MT_ASSERT_EQ(song.orders[0], 0, "rt orders[0]");
    MT_ASSERT_EQ(song.patt_count, exp_patt, "rt patt_count");
    MT_ASSERT(song.freq_linear == exp_freq, "rt freq_linear");
    MT_ASSERT(song.xm_mode == exp_xm, "rt xm_mode");

    /* Channel volumes and panning */
    for (int i = 0; i < MT_MAX_CHANNELS; i++) {
        MT_ASSERT_EQ(song.channel_volume[i], exp_chvol[i], "rt ch_vol");
        MT_ASSERT_EQ(song.channel_panning[i], exp_chpan[i], "rt ch_pan");
    }

    /* Pattern 0: all cells should be empty */
    MT_ASSERT(song.patterns[0] != NULL, "rt pat[0] exists");
    if (song.patterns[0]) {
        MT_Pattern *lpat = song.patterns[0];
        bool all_empty = true;
        for (int r = 0; r < (int)lpat->nrows && all_empty; r++) {
            for (int c = 0; c < MT_MAX_CHANNELS && all_empty; c++) {
                if (MT_CELL(lpat, r, c)->note != NOTE_EMPTY)
                    all_empty = false;
            }
        }
        MT_ASSERT(all_empty, "rt pat[0] all cells empty");
    }

    song_free();
}

static void test_mas_roundtrip_pattern_data(void)
{
    song_init();
    MT_Pattern *pat = song.patterns[0];

    /* Write diverse data */
    MT_CELL(pat, 0, 0)->note  = 48;
    MT_CELL(pat, 0, 0)->inst  = 1;
    MT_CELL(pat, 0, 0)->vol   = 40;
    MT_CELL(pat, 0, 0)->fx    = 0;
    MT_CELL(pat, 0, 0)->param = 0;

    MT_CELL(pat, 1, 0)->note  = 60;
    MT_CELL(pat, 1, 0)->inst  = 2;
    MT_CELL(pat, 1, 0)->vol   = 0;
    MT_CELL(pat, 1, 0)->fx    = 15;
    MT_CELL(pat, 1, 0)->param = 0x20;

    MT_CELL(pat, 2, 0)->note  = NOTE_CUT;
    MT_CELL(pat, 3, 0)->note  = NOTE_OFF;

    MT_CELL(pat, 0, 1)->note  = 72;
    MT_CELL(pat, 0, 1)->inst  = 3;
    MT_CELL(pat, 0, 1)->vol   = 0;
    MT_CELL(pat, 0, 1)->fx    = 1;
    MT_CELL(pat, 0, 1)->param = 0x0A;

    /* Repeated values (test RLE) */
    MT_CELL(pat, 4, 0)->note  = 48;
    MT_CELL(pat, 4, 0)->inst  = 1;
    MT_CELL(pat, 4, 0)->vol   = 40;
    MT_CELL(pat, 5, 0)->note  = 48;
    MT_CELL(pat, 5, 0)->inst  = 1;
    MT_CELL(pat, 5, 0)->vol   = 40;

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: mas_write failed (%d), no SD?\n", rc);
        song_free();
        return;
    }

    /* Load into the global song (mas_load calls song_free internally) */
    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "data mas_load succeeds");
    if (rc != 0) {
        song_free();
        return;
    }

    MT_Pattern *lpat = song.patterns[0];
    MT_ASSERT(lpat != NULL, "data rt pat[0] exists");
    if (lpat) {
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 0)->note, 48, "data rt [0][0].note");
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 0)->inst, 1,  "data rt [0][0].inst");
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 0)->vol,  40, "data rt [0][0].vol");

        MT_ASSERT_EQ(MT_CELL(lpat, 1, 0)->note, 60,   "data rt [1][0].note");
        MT_ASSERT_EQ(MT_CELL(lpat, 1, 0)->fx,   15,   "data rt [1][0].fx");
        MT_ASSERT_EQ(MT_CELL(lpat, 1, 0)->param, 0x20, "data rt [1][0].param");

        MT_ASSERT_EQ(MT_CELL(lpat, 2, 0)->note, NOTE_CUT, "data rt note_cut");
        MT_ASSERT_EQ(MT_CELL(lpat, 3, 0)->note, NOTE_OFF, "data rt note_off");

        MT_ASSERT_EQ(MT_CELL(lpat, 0, 1)->note, 72,   "data rt [0][1].note");
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 1)->inst, 3,    "data rt [0][1].inst");
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 1)->fx,   1,    "data rt [0][1].fx");
        MT_ASSERT_EQ(MT_CELL(lpat, 0, 1)->param, 0x0A, "data rt [0][1].param");

        /* RLE repeated values */
        MT_ASSERT_EQ(MT_CELL(lpat, 4, 0)->note, 48, "data rt rle[4]");
        MT_ASSERT_EQ(MT_CELL(lpat, 5, 0)->note, 48, "data rt rle[5]");
        MT_ASSERT_EQ(MT_CELL(lpat, 5, 0)->inst, 1,  "data rt rle[5] inst");
        MT_ASSERT_EQ(MT_CELL(lpat, 5, 0)->vol,  40, "data rt rle[5] vol");
    }

    song_free();
}

/* ------------------------------------------------------------------ */
/* T2.1: Pattern Clone                                                 */
/* ------------------------------------------------------------------ */

static void test_clone_pattern(void)
{
    song_init();

    /* Put some data in pattern 0 */
    MT_Pattern *src = song.patterns[0];
    MT_ASSERT(src != NULL, "pattern 0 allocated");
    MT_CELL(src, 0, 0)->note = 48;
    MT_CELL(src, 0, 0)->inst = 1;
    MT_CELL(src, 3, 0)->note = 52;
    MT_CELL(src, 3, 0)->inst = 2;

    /* Set order to use pattern 0 */
    song.orders[0] = 0;

    /* Find an unused pattern slot */
    u8 new_idx = 255;
    for (int i = 1; i < MT_MAX_PATTERNS; i++) {
        if (!song.patterns[i]) { new_idx = (u8)i; break; }
    }
    MT_ASSERT(new_idx != 255, "found unused pattern slot");

    /* Clone: allocate new pattern and copy */
    song_alloc_pattern(new_idx, src->nrows, src->ncols);
    MT_ASSERT(song.patterns[new_idx] != NULL, "new pattern allocated");
    memcpy(song.patterns[new_idx], src, MT_PATTERN_SIZE(src->nrows, src->ncols));

    /* Verify clone is independent copy with same data */
    MT_Pattern *dst = song.patterns[new_idx];
    MT_ASSERT(dst != src, "clone is different pointer");
    MT_ASSERT_EQ(MT_CELL(dst, 0, 0)->note, 48, "cloned note 0 matches");
    MT_ASSERT_EQ(MT_CELL(dst, 0, 0)->inst, 1, "cloned inst 0 matches");
    MT_ASSERT_EQ(MT_CELL(dst, 3, 0)->note, 52, "cloned note 3 matches");
    MT_ASSERT_EQ(dst->nrows, src->nrows, "cloned nrows matches");

    /* Modify clone, verify source unchanged */
    MT_CELL(dst, 0, 0)->note = 60;
    MT_ASSERT_EQ(MT_CELL(src, 0, 0)->note, 48, "source unchanged after clone edit");

    song_free();
}

/* ------------------------------------------------------------------ */
/* T2.2: Tempo Bounds                                                  */
/* ------------------------------------------------------------------ */

static void test_tempo_bounds(void)
{
    song_init();

    /* Default tempo */
    MT_ASSERT_EQ(song.initial_tempo, 125, "default tempo 125");

    /* Increase to max */
    song.initial_tempo = 254;
    song.initial_tempo++;
    MT_ASSERT_EQ(song.initial_tempo, 255, "tempo can reach 255");

    /* Should not exceed 255 (u8 wraps, but code should clamp) */
    /* The code in main.c clamps to 255, here we just verify the field */
    MT_ASSERT(song.initial_tempo <= 255, "tempo <= 255");

    /* Decrease to min */
    song.initial_tempo = 33;
    song.initial_tempo--;
    MT_ASSERT_EQ(song.initial_tempo, 32, "tempo can reach 32");

    song_free();
}

/* ------------------------------------------------------------------ */
/* T2.3: Note-Off Value                                                */
/* ------------------------------------------------------------------ */

static void test_note_off_value(void)
{
    song_init();

    MT_Pattern *pat = song.patterns[0];
    MT_ASSERT(pat != NULL, "pattern exists");

    /* Place a note-off */
    MT_CELL(pat, 5, 0)->note = NOTE_OFF;
    MT_ASSERT_EQ(MT_CELL(pat, 5, 0)->note, 255, "NOTE_OFF == 255");

    /* Verify note-off is distinct from empty and cut */
    MT_ASSERT(NOTE_OFF != NOTE_EMPTY, "OFF != EMPTY");
    MT_ASSERT(NOTE_OFF != NOTE_CUT, "OFF != CUT");
    MT_ASSERT_EQ(NOTE_EMPTY, 250, "EMPTY == 250");
    MT_ASSERT_EQ(NOTE_CUT, 254, "CUT == 254");

    song_free();
}

/* ------------------------------------------------------------------ */
/* T2.4: Save-As Filename Generation                                   */
/* ------------------------------------------------------------------ */

static void test_save_as_filename(void)
{
    /* Verify snprintf generates expected filenames */
    char path[128];
    snprintf(path, sizeof(path), "fat:/maxtracker/songs/song_%02d.mas", 1);
    MT_ASSERT(strcmp(path, "fat:/maxtracker/songs/song_01.mas") == 0,
              "save-as filename 01");
    snprintf(path, sizeof(path), "fat:/maxtracker/songs/song_%02d.mas", 99);
    MT_ASSERT(strcmp(path, "fat:/maxtracker/songs/song_99.mas") == 0,
              "save-as filename 99");
}

/* ------------------------------------------------------------------ */
/* T2.5: Default Sample Exists                                         */
/* ------------------------------------------------------------------ */

static void test_default_sample(void)
{
    song_init();

    /* Instrument 0 should reference sample 1 */
    MT_ASSERT_EQ(song.instruments[0].active, true, "inst 0 active");
    MT_ASSERT_EQ(song.instruments[0].sample, 1, "inst 0 -> sample 1");

    /* Sample 0 should have valid PCM data */
    MT_ASSERT_EQ(song.samples[0].active, true, "sample 0 active");
    MT_ASSERT(song.samples[0].pcm_data != NULL, "sample 0 has PCM");
    MT_ASSERT_EQ(song.samples[0].length, 256, "sample 0 length 256");
    MT_ASSERT_EQ(song.samples[0].bits, 8, "sample 0 is 8-bit");
    MT_ASSERT_EQ(song.samples[0].base_freq, 8363, "sample 0 freq 8363");
    MT_ASSERT_EQ(song.samples[0].loop_type, 1, "sample 0 loops");
    MT_ASSERT_EQ(song.samp_count, 1, "samp_count == 1");

    song_free();
}

/* ------------------------------------------------------------------ */
/* T2.6: MAS Sample Info Field Order (end-to-end)                      */
/*                                                                     */
/* Catches bugs where mas_write/mas_load use different field order      */
/* than the mm_mas_sample_info struct that maxmod expects.              */
/* ------------------------------------------------------------------ */

static void test_mas_sample_field_order(void)
{
    song_init();

    /* Set up a sample with distinct values in every field */
    MT_Sample *s = &song.samples[0];
    s->active         = true;
    s->default_volume = 42;
    s->panning        = 0xC0;  /* center stereo (0x80 | 64) */
    s->base_freq      = 22050;
    s->global_volume  = 50;
    s->vib_type       = 1;
    s->vib_depth      = 20;
    s->vib_speed      = 30;
    s->vib_rate        = 100;
    s->format         = 0; /* 8-bit */
    s->bits           = 8;
    s->loop_type      = 1;
    s->loop_start     = 0;
    s->loop_length    = 256;
    /* pcm_data already set by song_init */

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: mas_write failed (%d)\n", rc);
        song_free();
        return;
    }

    /* Reload */
    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "field order: load ok");
    if (rc != 0) { song_free(); return; }

    MT_Sample *ls = &song.samples[0];
    MT_ASSERT_EQ(ls->active, true, "field order: active");
    MT_ASSERT_EQ(ls->default_volume, 42, "field order: default_volume");
    MT_ASSERT_EQ(ls->panning, 0xC0, "field order: panning");
    /* base_freq goes through /4 then *4 roundtrip: 22050/4=5512, *4=22048 */
    MT_ASSERT(ls->base_freq >= 22048 && ls->base_freq <= 22052,
              "field order: base_freq roundtrip");
    MT_ASSERT_EQ(ls->global_volume, 50, "field order: global_volume");
    MT_ASSERT_EQ(ls->vib_type, 1, "field order: vib_type");
    MT_ASSERT_EQ(ls->vib_depth, 20, "field order: vib_depth");
    MT_ASSERT_EQ(ls->vib_speed, 30, "field order: vib_speed");
    MT_ASSERT_EQ(ls->vib_rate, 100, "field order: vib_rate");
    MT_ASSERT_EQ(ls->format, 0, "field order: format");
    MT_ASSERT_EQ(ls->loop_type, 1, "field order: loop_type");

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/* ------------------------------------------------------------------ */
/* T2.7: Panning Encoding Convention                                   */
/*                                                                     */
/* Maxmod uses bit 7 of sample panning as a "panning valid" flag.      */
/* Value 128 (0x80) = full left, NOT center! Center = 0xC0 (0x80|64). */
/* This test verifies our defaults and roundtrip preserve this.        */
/* ------------------------------------------------------------------ */

static void test_panning_encoding(void)
{
    /* Verify MSB flag convention */
    u8 center = 0xC0;  /* 0x80 | 64 */
    u8 left   = 0x80;  /* 0x80 | 0  */
    u8 right  = 0xFE;  /* 0x80 | 126 */

    /* MSB must be set for panning to be active */
    MT_ASSERT((center & 0x80) != 0, "center has MSB");
    MT_ASSERT((left & 0x80) != 0, "left has MSB");
    MT_ASSERT((right & 0x80) != 0, "right has MSB");

    /* Extract pan value (lower 7 bits) */
    MT_ASSERT_EQ(center & 0x7F, 64, "center pan value = 64");
    MT_ASSERT_EQ(left & 0x7F, 0, "left pan value = 0");
    MT_ASSERT_EQ(right & 0x7F, 126, "right pan value = 126");

    /* Verify song_init defaults */
    song_init();
    MT_ASSERT_EQ(song.instruments[0].panning, 0xC0, "inst default = center");
    MT_ASSERT_EQ(song.samples[0].panning, 0xC0, "sample default = center");

    /* Roundtrip through file */
    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: panning write failed\n");
        song_free();
        return;
    }
    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "panning roundtrip load ok");
    if (rc == 0) {
        MT_ASSERT_EQ(song.samples[0].panning, 0xC0,
                     "panning roundtrip = center");
    }

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/* ------------------------------------------------------------------ */
/* T2.8: Instrument-to-Sample Mapping Roundtrip                        */
/*                                                                     */
/* Verifies the notemap_flag (compact mode: 0x8000 | sample) survives  */
/* a write/load cycle, and that the instrument's sample reference       */
/* correctly points to the right sample after loading.                  */
/* ------------------------------------------------------------------ */

static void test_instrument_sample_mapping(void)
{
    song_init();

    /* Set up instrument 0 pointing to sample 1 (index 0) */
    song.instruments[0].active = true;
    song.instruments[0].sample = 1;
    song.instruments[0].has_full_notemap = false;
    song.inst_count = 1;

    /* Set up instrument 1 pointing to sample 5 */
    song.instruments[1].active = true;
    song.instruments[1].sample = 5;
    song.instruments[1].has_full_notemap = false;
    song.instruments[1].global_volume = 128;
    song.instruments[1].panning = 0xC0;
    song.inst_count = 2;

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: inst mapping write failed\n");
        song_free();
        return;
    }

    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "inst mapping load ok");
    if (rc == 0) {
        MT_ASSERT_EQ(song.instruments[0].sample, 1,
                     "inst 0 -> sample 1");
        MT_ASSERT_EQ(song.instruments[0].has_full_notemap, false,
                     "inst 0 compact notemap");
        MT_ASSERT_EQ(song.instruments[1].sample, 5,
                     "inst 1 -> sample 5");
        MT_ASSERT_EQ(song.instruments[1].has_full_notemap, false,
                     "inst 1 compact notemap");
    }

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/* ------------------------------------------------------------------ */
/* T2.9: Full MAS Roundtrip with Instrument+Sample+Pattern             */
/*                                                                     */
/* End-to-end: create a song with real instrument, sample, and notes.   */
/* Write to file, load back, verify everything matches.                */
/* ------------------------------------------------------------------ */

static void test_mas_full_roundtrip(void)
{
    song_init();

    /* Instrument 0 already set by song_init with sample 1 */
    /* Sample 0 already has square wave PCM from song_init */

    /* Add some notes */
    MT_Pattern *pat = song.patterns[0];
    MT_ASSERT(pat != NULL, "full rt: pat exists");
    MT_CELL(pat, 0, 0)->note = 48;
    MT_CELL(pat, 0, 0)->inst = 1;
    MT_CELL(pat, 0, 0)->vol  = 64;
    MT_CELL(pat, 1, 0)->note = 52;
    MT_CELL(pat, 1, 0)->inst = 1;
    MT_CELL(pat, 2, 0)->note = NOTE_OFF;
    MT_CELL(pat, 3, 0)->note = NOTE_CUT;

    /* Save the values we want to verify */
    u8 saved_tempo = song.initial_tempo;
    u8 saved_speed = song.initial_speed;
    u8 saved_samp_defvol = song.samples[0].default_volume;
    u8 saved_samp_panning = song.samples[0].panning;
    u32 saved_samp_freq = song.samples[0].base_freq;
    u32 saved_samp_len = song.samples[0].length;

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: full roundtrip write failed (%d)\n", rc);
        song_free();
        return;
    }

    /* Load back */
    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "full rt: load ok");
    if (rc != 0) { song_free(); return; }

    /* Verify header */
    MT_ASSERT_EQ(song.initial_tempo, saved_tempo, "full rt: tempo");
    MT_ASSERT_EQ(song.initial_speed, saved_speed, "full rt: speed");
    /* channel_count is always 32 after load (all channels available) */
    MT_ASSERT_EQ(song.channel_count, 32, "full rt: channels == 32");

    /* Verify instrument */
    MT_ASSERT_EQ(song.instruments[0].active, true, "full rt: inst active");
    MT_ASSERT_EQ(song.instruments[0].sample, 1, "full rt: inst sample");

    /* Verify sample */
    MT_ASSERT_EQ(song.samples[0].active, true, "full rt: samp active");
    MT_ASSERT_EQ(song.samples[0].default_volume, saved_samp_defvol,
                 "full rt: samp defvol");
    MT_ASSERT_EQ(song.samples[0].panning, saved_samp_panning,
                 "full rt: samp panning");
    MT_ASSERT(song.samples[0].base_freq >= saved_samp_freq - 4 &&
              song.samples[0].base_freq <= saved_samp_freq + 4,
              "full rt: samp freq");
    MT_ASSERT_EQ(song.samples[0].length, saved_samp_len, "full rt: samp len");
    MT_ASSERT(song.samples[0].pcm_data != NULL, "full rt: samp PCM exists");

    /* Verify PCM data is correct (square wave: first half negative, second positive) */
    if (song.samples[0].pcm_data && song.samples[0].length >= 256) {
        s8 *pcm = (s8 *)song.samples[0].pcm_data;
        MT_ASSERT(pcm[0] < 0, "full rt: PCM[0] negative (square low)");
        MT_ASSERT(pcm[127] < 0, "full rt: PCM[127] negative");
        MT_ASSERT(pcm[128] > 0, "full rt: PCM[128] positive (square high)");
        MT_ASSERT(pcm[255] > 0, "full rt: PCM[255] positive");
    }

    /* Verify pattern */
    pat = song.patterns[0];
    MT_ASSERT(pat != NULL, "full rt: pat loaded");
    if (pat) {
        MT_ASSERT_EQ(MT_CELL(pat, 0, 0)->note, 48, "full rt: note C-4");
        MT_ASSERT_EQ(MT_CELL(pat, 0, 0)->inst, 1, "full rt: inst 1");
        MT_ASSERT_EQ(MT_CELL(pat, 0, 0)->vol, 64, "full rt: vol 64");
        MT_ASSERT_EQ(MT_CELL(pat, 1, 0)->note, 52, "full rt: note E-4");
        MT_ASSERT_EQ(MT_CELL(pat, 2, 0)->note, NOTE_OFF, "full rt: note OFF");
        MT_ASSERT_EQ(MT_CELL(pat, 3, 0)->note, NOTE_CUT, "full rt: note CUT");
    }

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/*
 * T2.10: Pan/pitch envelope roundtrip.
 *
 * Background: maxtracker used to encode env_pan.enabled and
 * env_pitch.enabled into bits 4 and 5 of the env_flags byte. Those
 * bits don't exist in mmutil's MAS format — only bit 3 (volume
 * envelope enabled) is real, and pan/pitch envelopes are gated by the
 * EXISTS bits alone in maxmod's playback engine. The fix removes the
 * invented bits and derives the model's enabled field from
 * node_count > 0 instead.
 *
 * This test verifies that pan and pitch envelope data survives a
 * save/load cycle through the corrected encoder/decoder. If either
 * half regresses, node data will be lost or the enabled flag will
 * desync from the node count.
 */
static void test_envelope_pan_pitch_roundtrip(void)
{
    song_init();

    MT_Instrument *ins = &song.instruments[0];
    ins->active = true;

    /* Pan envelope: three nodes, non-trivial values so we'd notice
     * any byte ordering or width mistake on the way through. */
    ins->env_pan.node_count = 3;
    ins->env_pan.nodes[0].x = 0;   ins->env_pan.nodes[0].y = 32;
    ins->env_pan.nodes[1].x = 25;  ins->env_pan.nodes[1].y = 48;
    ins->env_pan.nodes[2].x = 50;  ins->env_pan.nodes[2].y = 16;
    ins->env_pan.loop_start = 255;
    ins->env_pan.loop_end   = 255;
    ins->env_pan.sus_start  = 255;
    ins->env_pan.sus_end    = 255;
    ins->env_pan.enabled    = true;

    /* Pitch envelope: two nodes. */
    ins->env_pitch.node_count = 2;
    ins->env_pitch.nodes[0].x = 0;   ins->env_pitch.nodes[0].y = 32;
    ins->env_pitch.nodes[1].x = 40;  ins->env_pitch.nodes[1].y = 56;
    ins->env_pitch.loop_start = 255;
    ins->env_pitch.loop_end   = 255;
    ins->env_pitch.sus_start  = 255;
    ins->env_pitch.sus_end    = 255;
    ins->env_pitch.enabled    = true;

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: env rt write failed (%d)\n", rc);
        song_free();
        return;
    }

    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "env rt: load ok");
    if (rc != 0) { song_free(); return; }

    /* Pan envelope survived */
    MT_ASSERT_EQ(song.instruments[0].env_pan.node_count, 3,
                 "env rt: pan node_count");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[0].x, 0,
                 "env rt: pan node[0].x");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[0].y, 32,
                 "env rt: pan node[0].y");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[1].x, 25,
                 "env rt: pan node[1].x");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[1].y, 48,
                 "env rt: pan node[1].y");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[2].x, 50,
                 "env rt: pan node[2].x");
    MT_ASSERT_EQ(song.instruments[0].env_pan.nodes[2].y, 16,
                 "env rt: pan node[2].y");

    /* Pitch envelope survived */
    MT_ASSERT_EQ(song.instruments[0].env_pitch.node_count, 2,
                 "env rt: pitch node_count");
    MT_ASSERT_EQ(song.instruments[0].env_pitch.nodes[0].x, 0,
                 "env rt: pitch node[0].x");
    MT_ASSERT_EQ(song.instruments[0].env_pitch.nodes[0].y, 32,
                 "env rt: pitch node[0].y");
    MT_ASSERT_EQ(song.instruments[0].env_pitch.nodes[1].x, 40,
                 "env rt: pitch node[1].x");
    MT_ASSERT_EQ(song.instruments[0].env_pitch.nodes[1].y, 56,
                 "env rt: pitch node[1].y");

    /* enabled is now derived from node_count and must reflect it
     * after the round trip, regardless of how the file got encoded */
    MT_ASSERT(song.instruments[0].env_pan.enabled,
              "env rt: pan enabled derived from node_count");
    MT_ASSERT(song.instruments[0].env_pitch.enabled,
              "env rt: pitch enabled derived from node_count");

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/*
 * T2.11: Empty pan/pitch envelopes stay empty.
 *
 * Verifies the symmetric case: an instrument with no pan/pitch nodes
 * loads back with no pan/pitch nodes and enabled flags cleared. This
 * catches a regression where the loader might spuriously create
 * default nodes or set enabled = true for an envelope that doesn't
 * exist in the file.
 */
static void test_envelope_pan_pitch_absent(void)
{
    song_init();

    MT_Instrument *ins = &song.instruments[0];
    ins->active = true;
    /* No pan or pitch envelope set up — node_count is zero from song_init */

    int rc = mas_write(TEST_MAS_PATH_DATA, &song);
    if (rc != 0) {
        iprintf("SKIP: env absent write failed (%d)\n", rc);
        song_free();
        return;
    }

    rc = mas_load(TEST_MAS_PATH_DATA, &song);
    MT_ASSERT_EQ(rc, 0, "env absent: load ok");
    if (rc != 0) { song_free(); return; }

    MT_ASSERT_EQ(song.instruments[0].env_pan.node_count, 0,
                 "env absent: pan node_count is 0");
    MT_ASSERT_EQ(song.instruments[0].env_pitch.node_count, 0,
                 "env absent: pitch node_count is 0");
    MT_ASSERT(!song.instruments[0].env_pan.enabled,
              "env absent: pan enabled is false");
    MT_ASSERT(!song.instruments[0].env_pitch.enabled,
              "env absent: pitch enabled is false");

    song_free();
    remove(TEST_MAS_PATH_DATA);
}

/* ------------------------------------------------------------------ */
/* T3.1-T3.4: Groove Tests (TDD stubs — compile only with feature)     */
/* ------------------------------------------------------------------ */

#ifdef MT_MAX_GROOVES

static void test_groove_init_defaults(void)
{
    song_init();

    for (int g = 0; g < MT_MAX_GROOVES; g++) {
        MT_ASSERT_EQ(song.grooves[g].length, 0, "groove length==0");
        for (int s = 0; s < MT_MAX_GROOVE_STEPS; s++) {
            MT_ASSERT_EQ(song.grooves[g].steps[s], 0, "groove step==0");
        }
    }
    MT_ASSERT_EQ(song.groove_count, 0, "groove_count==0");

    song_free();
}

static void test_groove_set_steps(void)
{
    song_init();

    /* Set groove 0: shuffle pattern {6, 4} */
    song.grooves[0].steps[0] = 6;
    song.grooves[0].steps[1] = 4;
    song.grooves[0].length   = 2;
    song.groove_count         = 1;

    MT_ASSERT_EQ(song.grooves[0].steps[0], 6, "groove[0].steps[0]==6");
    MT_ASSERT_EQ(song.grooves[0].steps[1], 4, "groove[0].steps[1]==4");
    MT_ASSERT_EQ(song.grooves[0].length, 2, "groove[0].length==2");
    MT_ASSERT_EQ(song.groove_count, 1, "groove_count==1");

    song_free();
}

static void test_groove_bounds(void)
{
    song_init();

    /* Fill groove 0 with maximum 16 steps */
    song.grooves[0].length = MT_MAX_GROOVE_STEPS;
    for (int i = 0; i < MT_MAX_GROOVE_STEPS; i++) {
        song.grooves[0].steps[i] = (u8)(i + 1);
    }

    /* Verify all 16 steps read back correctly */
    MT_ASSERT_EQ(song.grooves[0].length, 16, "groove max length==16");
    for (int i = 0; i < MT_MAX_GROOVE_STEPS; i++) {
        MT_ASSERT_EQ(song.grooves[0].steps[i], (u8)(i + 1),
                     "groove max step value");
    }

    song_free();
}

static void test_groove_empty(void)
{
    song_init();

    /* Groove 0 should start empty */
    MT_ASSERT_EQ(song.grooves[0].length, 0, "groove starts empty");

    /* Set groove 0 with 3 steps */
    song.grooves[0].steps[0] = 6;
    song.grooves[0].steps[1] = 4;
    song.grooves[0].steps[2] = 5;
    song.grooves[0].length   = 3;
    MT_ASSERT(song.grooves[0].length != 0, "groove no longer empty");

    /* Clear groove 0 */
    memset(song.grooves[0].steps, 0, MT_MAX_GROOVE_STEPS);
    song.grooves[0].length = 0;
    MT_ASSERT_EQ(song.grooves[0].length, 0, "groove empty again");
    for (int i = 0; i < MT_MAX_GROOVE_STEPS; i++) {
        MT_ASSERT_EQ(song.grooves[0].steps[i], 0, "groove step cleared");
    }

    song_free();
}

#endif /* MT_MAX_GROOVES */

/* ------------------------------------------------------------------ */
/* T3.5-3.8: Scale Enforcement Tests (TDD stubs)                       */
/* ------------------------------------------------------------------ */

#ifdef MT_SCALE_COUNT

static void test_scale_chromatic(void)
{
    /* All 12 semitones should be in the chromatic scale */
    for (u8 i = 0; i < 12; i++) {
        MT_ASSERT(mt_scale_contains(MT_SCALE_CHROMATIC, i),
                  "chromatic contains all semitones");
    }

    /* Snap should be identity for chromatic */
    MT_ASSERT_EQ(mt_scale_snap(MT_SCALE_CHROMATIC, 0, 48), 48,
                 "chromatic snap 48 unchanged");
    MT_ASSERT_EQ(mt_scale_snap(MT_SCALE_CHROMATIC, 0, 49), 49,
                 "chromatic snap 49 unchanged");
}

static void test_scale_major(void)
{
    /* C major = semitones {0, 2, 4, 5, 7, 9, 11} */
    const bool expected[12] = {
        true,  false, true,  false, true,  true,
        false, true,  false, true,  false, true
    };

    for (u8 i = 0; i < 12; i++) {
        bool result = mt_scale_contains(MT_SCALE_MAJOR, i);
        if (expected[i]) {
            MT_ASSERT(result == true, "major contains expected semitone");
        } else {
            MT_ASSERT(result == false, "major excludes expected semitone");
        }
    }

    /* Snap C#4 (note 49) to C major with root=0:
     * should give C4 (48) or D4 (50) — both are valid nearest notes */
    u8 snapped = mt_scale_snap(MT_SCALE_MAJOR, 0, 49);
    MT_ASSERT(snapped == 48 || snapped == 50,
              "major snap C#4 -> C4 or D4");
}

static void test_scale_next_prev(void)
{
    /* In C major (root=0): C D E F G A B */

    /* Next from C4 (48) = D4 (50) */
    MT_ASSERT_EQ(mt_scale_next(MT_SCALE_MAJOR, 0, 48), 50,
                 "major next C4 -> D4");

    /* Next from E4 (52) = F4 (53) */
    MT_ASSERT_EQ(mt_scale_next(MT_SCALE_MAJOR, 0, 52), 53,
                 "major next E4 -> F4");

    /* Prev from D4 (50) = C4 (48) */
    MT_ASSERT_EQ(mt_scale_prev(MT_SCALE_MAJOR, 0, 50), 48,
                 "major prev D4 -> C4");

    /* Prev from C4 (48) = B3 (47) */
    MT_ASSERT_EQ(mt_scale_prev(MT_SCALE_MAJOR, 0, 48), 47,
                 "major prev C4 -> B3");

    /* Next from B9 (119) should not exceed 119 */
    u8 top = mt_scale_next(MT_SCALE_MAJOR, 0, 119);
    MT_ASSERT(top <= 119, "next from 119 clamped");

    /* Prev from C0 (0) should not go below 0 */
    u8 bottom = mt_scale_prev(MT_SCALE_MAJOR, 0, 0);
    MT_ASSERT_EQ(bottom, 0, "prev from 0 clamped");
}

static void test_scale_pentatonic(void)
{
    /* C pentatonic = semitones {0, 2, 4, 7, 9} */
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 0) == true,
              "pentatonic contains C");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 2) == true,
              "pentatonic contains D");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 4) == true,
              "pentatonic contains E");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 5) == false,
              "pentatonic excludes F");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 7) == true,
              "pentatonic contains G");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 9) == true,
              "pentatonic contains A");
    MT_ASSERT(mt_scale_contains(MT_SCALE_PENTATONIC, 11) == false,
              "pentatonic excludes B");
}

#endif /* MT_SCALE_COUNT */

/* ------------------------------------------------------------------ */
/* T4: Name-field memory layout (after 23 -> 33 byte expansion)        */
/* ------------------------------------------------------------------ */

/*
 * Canary: the on-screen keyboard allows up to 32 characters in the
 * sample / song name fields. Both buffers must hold 32 chars plus a
 * NUL, so sizeof must be >= 33. If someone shrinks the field in
 * song.h this test fires before anyone tries to write past the end.
 */
static void test_name_field_sizes(void)
{
    /* Pure sizeof assertions — no allocation needed. */
    MT_ASSERT(sizeof(((MT_Sample *)0)->name) >= 33,
              "MT_Sample.name >= 33 bytes");
    MT_ASSERT(sizeof(((MT_Song   *)0)->name) >= 33,
              "MT_Song.name >= 33 bytes");
}

/*
 * Write a 32-character name and confirm strlen and the terminating
 * NUL land where we expect. Catches off-by-one bugs in any future
 * resize.
 */
static void test_sample_name_32_chars(void)
{
    song_init();

    MT_Sample *s = &song.samples[0];
    const char *name32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";  /* 32 chars */

    memset(s->name, 0, sizeof(s->name));
    strncpy(s->name, name32, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';

    MT_ASSERT_EQ(strlen(s->name), 32, "sample name length == 32");
    MT_ASSERT(strcmp(s->name, name32) == 0, "sample name content matches");
    MT_ASSERT_EQ(s->name[32], '\0', "sample name NUL at offset 32");

    song_free();
}

static void test_song_name_32_chars(void)
{
    song_init();

    const char *name32 = "ThisIsAThirtyTwoCharacterSongNAME";  /* 33? count */
    /* Exactly 32 chars: */
    const char *name32_exact = "SongNameThatIsExactly32Chars_OK!";

    memset(song.name, 0, sizeof(song.name));
    strncpy(song.name, name32_exact, sizeof(song.name) - 1);
    song.name[sizeof(song.name) - 1] = '\0';

    MT_ASSERT_EQ(strlen(song.name), 32, "song name length == 32");
    MT_ASSERT(strcmp(song.name, name32_exact) == 0,
              "song name content matches");

    /* Longer strings must truncate, not crash. */
    const char *too_long =
        "ThisStringIsDeliberatelyLongerThanThirtyTwoChars_XXXX";
    strncpy(song.name, too_long, sizeof(song.name) - 1);
    song.name[sizeof(song.name) - 1] = '\0';
    MT_ASSERT(strlen(song.name) <= 32, "song name truncated to <= 32");

    (void)name32;
    song_free();
}

/*
 * MAS serialization does not store sample or song names, but the
 * buffers sit next to data that IS serialized — length, volume,
 * pcm pointers etc. A layout regression (e.g. accidental struct
 * reorder, missing padding) would corrupt that adjacent data across
 * a write/read cycle. This test sets non-default fields, writes an
 * MAS, re-loads it, and verifies the reloaded fields match.
 *
 * The name fields themselves are expected to be cleared by mas_load
 * (MAS has no name section) — that's asserted explicitly.
 */
static void test_mas_roundtrip_after_name_expansion(void)
{
#ifdef ARM9
    const char *path = "fat:/maxtracker/_test_name_layout.mas";
#else
    const char *path = "_test_name_layout.mas";
#endif

    song_init();

    /* Long names on both song and sample. */
    strncpy(song.name, "RoundtripSong_LayoutSanityCheck!", 32);
    song.name[32] = '\0';

    /* Use a non-looping sample. A looping sample's total length is
     * implicitly loop_start+loop_length in MAS (the format only stores
     * those two fields), so a roundtrip can't preserve an arbitrary
     * `length` value. This test's purpose is "adjacent-to-name fields
     * survive the roundtrip", not "MAS looping semantics". */
    MT_Sample *s = &song.samples[0];
    s->active         = true;
    s->bits           = 16;
    s->format         = 1;
    s->length         = 64;
    s->loop_start     = 0;
    s->loop_length    = 0;
    s->loop_type      = 0;
    s->base_freq      = 44100;
    s->default_volume = 50;
    s->panning        = 200;
    s->global_volume  = 64;

    s->pcm_data = (u8 *)malloc(s->length * 2);
    MT_ASSERT(s->pcm_data != NULL, "sample pcm alloc");
    if (s->pcm_data) {
        s16 *pcm = (s16 *)s->pcm_data;
        for (u32 i = 0; i < s->length; i++)
            pcm[i] = (s16)(i * 123);
    }
    strncpy(s->name, "SampleNameExactly32CharsWideOKAY", 32);
    s->name[32] = '\0';

    if (song.samp_count < 1) song.samp_count = 1;

    int werr = mas_write(path, &song);
    MT_ASSERT_EQ(werr, 0, "mas_write returns 0");

    /* Remember key values before reload. */
    u32  exp_len  = s->length;
    u32  exp_loop = s->loop_start;
    u8   exp_bits = s->bits;
    u32  exp_rate = s->base_freq;

    song_free();

    /* Reload into a fresh song. */
    int lerr = mas_load(path, &song);
    MT_ASSERT(lerr == 0 || lerr == 1, "mas_load returns 0 or 1");

    MT_Sample *r = &song.samples[0];
    MT_ASSERT(r->active, "reloaded sample active");
    MT_ASSERT_EQ(r->length, exp_len,  "reloaded sample length");
    MT_ASSERT_EQ(r->loop_start, exp_loop, "reloaded sample loop_start");
    MT_ASSERT_EQ(r->loop_length, 0,       "reloaded sample loop_length");
    MT_ASSERT_EQ(r->bits, exp_bits, "reloaded sample bits");
    MT_ASSERT_EQ(r->base_freq, exp_rate, "reloaded sample rate");

    /* Names are NOT persisted by MAS — mas_load zeros them. */
    MT_ASSERT(r->name[0] == '\0', "reloaded sample name cleared");
    MT_ASSERT(song.name[0] == '\0', "reloaded song name cleared");

    song_free();
    remove(path);
}

/* ------------------------------------------------------------------ */
/* T5: Autosave helper contract                                        */
/* ------------------------------------------------------------------ */

/*
 * The autosave loop in main.c polls two globals — `song_modified` and
 * `autosave_dirty`. Every view mutating song state is required to
 * call `mt_mark_song_modified()` (not touch the flags directly), so
 * autosave only fires when both flags flip together. If a future
 * refactor accidentally drops one flag, autosave silently misses
 * those edits — a class we've been bitten by before. These tests
 * lock the helper's contract in place.
 */

extern bool song_modified;
extern bool autosave_dirty;
void mt_mark_song_modified(void); /* editor_state.c */

static void test_mark_song_modified_sets_both_flags(void)
{
    song_modified  = false;
    autosave_dirty = false;

    mt_mark_song_modified();

    MT_ASSERT(song_modified == true,  "helper sets song_modified");
    MT_ASSERT(autosave_dirty == true, "helper sets autosave_dirty");
}

static void test_mark_song_modified_is_idempotent(void)
{
    song_modified  = false;
    autosave_dirty = false;

    mt_mark_song_modified();
    mt_mark_song_modified();
    mt_mark_song_modified();

    MT_ASSERT(song_modified == true,  "still true after repeated calls");
    MT_ASSERT(autosave_dirty == true, "still true after repeated calls");
}

/*
 * Autosave happy path: simulate a modified song, write the backup to
 * a deterministic path, reload it, verify fields survive the round
 * trip. The real main-loop logic is just `if (dirty) mas_write()` —
 * this test is the end-to-end contract of that write.
 */
static void test_autosave_roundtrip(void)
{
#ifdef ARM9
    const char *path = "fat:/maxtracker/_test_autosave.mas";
#else
    const char *path = "_test_autosave.mas";
#endif

    song_init();
    strncpy(song.name, "AutosaveRoundtripCheck_________1", 32);
    song.name[32] = '\0';
    song.initial_tempo = 140;
    song.initial_speed = 4;

    /* Flag-set via the helper — what any editing path would do. */
    mt_mark_song_modified();
    MT_ASSERT(autosave_dirty == true, "pre-write: autosave_dirty set");

    /* Simulate the autosave-loop write + clear. */
    int err = mas_write(path, &song);
    if (err != 0) {
        iprintf("SKIP: mas_write failed (%d), no SD?\n", err);
        song_free();
        return;
    }
    autosave_dirty = false;
    MT_ASSERT(autosave_dirty == false, "post-write: flag cleared");

    /* Now reload and sanity-check. Name isn't stored in MAS — skip. */
    song_free();
    int lerr = mas_load(path, &song);
    MT_ASSERT(lerr == 0 || lerr == 1, "autosave reload ok");
    MT_ASSERT_EQ(song.initial_tempo, 140, "autosave preserved tempo");
    MT_ASSERT_EQ(song.initial_speed, 4,   "autosave preserved speed");

    song_free();
    remove(path);
}

/* ------------------------------------------------------------------ */
/* Test runner                                                         */
/* ------------------------------------------------------------------ */

int mt_run_tests(void)
{
    test_results.passed = 0;
    test_results.failed = 0;
    test_results.total  = 0;

    iprintf("=== maxtracker unit tests ===\n\n");

    /* T1.1: Song model */
    iprintf("-- T1.1 Song model --\n");
    test_song_init_defaults();
    test_song_ensure_pattern_alloc();
    test_song_ensure_pattern_idempotent();
    test_song_free_cleanup();
    test_cell_size();

    /* T1.2: Clipboard */
    iprintf("-- T1.2 Clipboard --\n");
    test_clipboard_copy_paste_single();
    test_clipboard_copy_paste_block();

    /* T1.3: Undo */
    iprintf("-- T1.3 Undo --\n");
    test_undo_push_pop_single();
    test_undo_ring_overflow();
    test_undo_block_push_pop();
    test_undo_pop_with_null_pattern();
    test_undo_full_row_block();

    /* T1.4: MAS roundtrip */
    iprintf("-- T1.4 MAS roundtrip --\n");
    test_mas_roundtrip_empty();
    test_mas_roundtrip_pattern_data();

    /* T2.1: Pattern clone */
    iprintf("-- T2.1 Clone --\n");
    test_clone_pattern();

    /* T2.2: Tempo bounds */
    iprintf("-- T2.2 Tempo --\n");
    test_tempo_bounds();

    /* T2.3: Note-off */
    iprintf("-- T2.3 Note-off --\n");
    test_note_off_value();

    /* T2.4: Save-as filename */
    iprintf("-- T2.4 Save-as --\n");
    test_save_as_filename();

    /* T2.5: Default sample */
    iprintf("-- T2.5 Default sample --\n");
    test_default_sample();

    /* T2.6-2.9: End-to-end MAS tests */
    iprintf("-- T2.6 Sample field order --\n");
    test_mas_sample_field_order();
    iprintf("-- T2.7 Panning encoding --\n");
    test_panning_encoding();
    iprintf("-- T2.8 Inst-sample mapping --\n");
    test_instrument_sample_mapping();
    iprintf("-- T2.9 Full MAS roundtrip --\n");
    test_mas_full_roundtrip();
    iprintf("-- T2.10 Pan/pitch envelope roundtrip --\n");
    test_envelope_pan_pitch_roundtrip();
    iprintf("-- T2.11 Pan/pitch envelope absent --\n");
    test_envelope_pan_pitch_absent();

#ifdef MT_MAX_GROOVES
    iprintf("-- T3.1-4 Groove --\n");
    test_groove_init_defaults();
    test_groove_set_steps();
    test_groove_bounds();
    test_groove_empty();
#endif

#ifdef MT_SCALE_COUNT
    iprintf("-- T3.5-8 Scale --\n");
    test_scale_chromatic();
    test_scale_major();
    test_scale_next_prev();
    test_scale_pentatonic();
#endif

    /* T4.1-4: Name-field layout after 23 -> 33 byte expansion */
    iprintf("-- T4.1 Name field sizes --\n");
    test_name_field_sizes();
    iprintf("-- T4.2 Sample 32-char name --\n");
    test_sample_name_32_chars();
    iprintf("-- T4.3 Song 32-char name --\n");
    test_song_name_32_chars();
    iprintf("-- T4.4 MAS roundtrip after name expansion --\n");
    test_mas_roundtrip_after_name_expansion();

    /* T5: Autosave helper contract */
    iprintf("-- T5.1 mt_mark_song_modified sets both flags --\n");
    test_mark_song_modified_sets_both_flags();
    iprintf("-- T5.2 helper is idempotent --\n");
    test_mark_song_modified_is_idempotent();
    iprintf("-- T5.3 autosave roundtrip --\n");
    test_autosave_roundtrip();

    /* Summary */
    iprintf("\n=== Results: %d/%d passed",
            test_results.passed, test_results.total);
    if (test_results.failed > 0)
        iprintf(", %d FAILED", test_results.failed);
    iprintf(" ===\n");

    return test_results.failed == 0 ? 0 : 1;
}
