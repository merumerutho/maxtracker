/*
 * debug_view.c — Tier 1 on-screen debug overlay implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See debug_view.h for the rationale. The implementation is
 * intentionally simple: fixed-size buffers, no allocations, no locks.
 * If the rest of the UI is broken, this file is the one we still want
 * to work — so nothing fancy.
 */

#include "debug_view.h"

#include "screen.h"
#include "font.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* sbrk */

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define DBG_LOG_LINES  8
#define DBG_LOG_COLS   64

/* Overlay location on the top screen (5 rows). Runtime-computed so the
 * rows track the current font mode (SMALL: 27..31, BIG: 19..23). */
#define DBG_ROW_HEADER  (FONT_ROWS - 5)
#define DBG_ROW_LOG0    (FONT_ROWS - 4)
#define DBG_ROW_LOG1    (FONT_ROWS - 3)
#define DBG_ROW_LOG2    (FONT_ROWS - 2)
#define DBG_ROW_ERROR   (FONT_ROWS - 1)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static char      dbg_log_buf[DBG_LOG_LINES][DBG_LOG_COLS];
static int       dbg_log_count = 0;    /* entries present (capped at LINES) */
static int       dbg_log_head  = 0;    /* next write slot (circular) */
static uint32_t  dbg_frame_count = 0;
static char      dbg_last_error[DBG_LOG_COLS] = "";
static bool      dbg_visible = false;  /* off by default; toggle from PROJECT settings */
static uintptr_t dbg_heap_start = 0;

/* ------------------------------------------------------------------ */
/* Heap usage                                                          */
/*                                                                     */
/* sbrk(0) returns the current program break (top of heap). We snap    */
/* it at boot and report the delta as "heap used" — not a free-ram     */
/* number, but rather "how much has been allocated and not released    */
/* back to the OS since boot". Climbing = leak; large = heavy use.     */
/* ------------------------------------------------------------------ */

static uint32_t dbg_heap_used(void)
{
    uintptr_t now = (uintptr_t)sbrk(0);
    if (now >= dbg_heap_start) return (uint32_t)(now - dbg_heap_start);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void dbg_init(void)
{
    memset(dbg_log_buf, 0, sizeof(dbg_log_buf));
    dbg_log_count = 0;
    dbg_log_head  = 0;
    dbg_frame_count = 0;
    dbg_last_error[0] = '\0';
    dbg_visible = false;   /* user opts in via PROJECT settings */
    dbg_heap_start = (uintptr_t)sbrk(0);
}

void dbg_log(const char *fmt, ...)
{
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dbg_log_buf[dbg_log_head], DBG_LOG_COLS, fmt, ap);
    va_end(ap);
    dbg_log_head = (dbg_log_head + 1) % DBG_LOG_LINES;
    if (dbg_log_count < DBG_LOG_LINES) dbg_log_count++;
}

void dbg_set_last_error(const char *fmt, ...)
{
    if (!fmt) { dbg_last_error[0] = '\0'; return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dbg_last_error, sizeof(dbg_last_error), fmt, ap);
    va_end(ap);
}

void dbg_frame_tick(void)
{
    dbg_frame_count++;
}

void dbg_toggle(void)
{
    dbg_visible = !dbg_visible;
}

bool dbg_is_visible(void)
{
    /* BIG font only has 24 rows — the overlay's 5-row footer (rows
     * 27-31 in SMALL) would be off-screen, and forcing it into the
     * BIG grid would clobber real view content. Honour the user's
     * preference but suppress rendering while BIG is active; the
     * stored flag is restored as soon as they switch back to SMALL. */
    return dbg_visible && font_get_mode() != FONT_MODE_BIG;
}

void dbg_draw_overlay(u8 *top_fb)
{
    if (!top_fb || !dbg_is_visible()) return;

    /* Clear our 5 rows before drawing so the overlay doesn't let
     * previous content bleed through. We use PAL_BG for the log area
     * and PAL_HEADER_BG for the header row to visually mark the strip. */
    font_fill_row(top_fb, DBG_ROW_HEADER, 0, FONT_COLS, PAL_HEADER_BG);
    for (int r = DBG_ROW_LOG0; r <= DBG_ROW_ERROR; r++) {
        font_fill_row(top_fb, r, 0, FONT_COLS, PAL_BG);
    }

    /* Header row: identifier + frame counter + heap growth. */
    font_printf(top_fb, 0, DBG_ROW_HEADER, PAL_TEXT,
                "DBG f=%6lu heap+%7luB",
                (unsigned long)dbg_frame_count,
                (unsigned long)dbg_heap_used());

    /* Three most recent log lines, newest first. */
    int shown = 0;
    for (int i = 0; i < dbg_log_count && shown < 3; i++) {
        int idx = (dbg_log_head - 1 - i + DBG_LOG_LINES) % DBG_LOG_LINES;
        if (dbg_log_buf[idx][0] != '\0') {
            font_puts(top_fb, 0, DBG_ROW_LOG0 + shown,
                      dbg_log_buf[idx], PAL_GRAY);
        }
        shown++;
    }

    /* Error slot — persists after logs scroll away. Uses PAL_ORANGE
     * for visibility; empty is shown as a dimmed placeholder. */
    if (dbg_last_error[0]) {
        font_puts(top_fb, 0, DBG_ROW_ERROR, "ERR:", PAL_ORANGE);
        font_puts(top_fb, 5, DBG_ROW_ERROR, dbg_last_error, PAL_ORANGE);
    } else {
        font_puts(top_fb, 0, DBG_ROW_ERROR, "(no errors)", PAL_DIM);
    }
}
