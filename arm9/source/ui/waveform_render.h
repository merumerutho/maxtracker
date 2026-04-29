/*
 * waveform_render.h — Shared waveform visualization helper.
 *
 * Provides a single rendering routine used by sample_view, waveform_view,
 * and lfe_fx_view so all three scopes look and behave identically.
 *
 * Style: vertical fill from horizontal center line to sample value,
 * one sample per pixel column. Optional overlays for selection range
 * and loop markers.
 *
 * Zoom model: `visible == 0` is FIT (always available). Otherwise
 * `visible` is the target number of samples shown across 256 columns,
 * clamped to the actual sample length so short samples degrade
 * gracefully at any zoom level.
 */

#ifndef MT_WAVEFORM_RENDER_H
#define MT_WAVEFORM_RENDER_H

#include <nds.h>
#include <stdbool.h>

/* Canonical zoom levels. Index 0 is FIT (always available). */
#define WAVEFORM_ZOOM_COUNT 4
extern const u32 waveform_zoom_levels[WAVEFORM_ZOOM_COUNT];

/* Config struct; zero-init gives a valid "no overlays" scope. */
typedef struct {
    /* Framebuffer region */
    u8 *fb;             /* shadow framebuffer (256 px wide stride) */
    int y_top;          /* top pixel row (inclusive) */
    int y_height;       /* pixel height of scope region */

    /* Sample data */
    const void *pcm;    /* s8* or s16* */
    bool is_16bit;
    u32 length;         /* samples in pcm */

    /* Zoom/scroll. visible == 0 means FIT (show all `length` samples). */
    u32 visible;
    u32 scroll;

    /* Colors */
    u8 color_bg;
    u8 color_center;
    u8 color_wave;      /* default wave color */

    /* Optional selection range. sel_end > sel_start enables. Samples
     * inside the range use color_sel_in; outside use color_wave. */
    u32 sel_start;
    u32 sel_end;
    u8  color_sel_in;
    u8  color_sel_marker;   /* vertical boundary lines; 0 = disabled */

    /* Optional loop markers (dotted vertical lines). loop_length > 0
     * enables. Draws at loop_start and loop_start + loop_length. */
    u32 loop_start;
    u32 loop_length;
    u8  color_loop;
} WaveformRenderCfg;

/* Clear the configured region and render the scope per cfg. */
void waveform_render(const WaveformRenderCfg *cfg);

#endif /* MT_WAVEFORM_RENDER_H */
