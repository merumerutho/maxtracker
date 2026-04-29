/*
 * tab_tone.h — descriptor export for the Test Tone generator tab.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Test Tone is a developer-only utility: it renders a pure sine
 * wave for sanity-checking the audio engine + sample draft path. End
 * users never need it, so the tab is gated on a separate flag
 * (MAXTRACKER_LFE_ENABLE_TESTTONE) which is NOT defined in normal
 * builds. Set it manually in your local Makefile (or pass
 * `-DMAXTRACKER_LFE_ENABLE_TESTTONE` on the command line) when you
 * want the tab back for debugging.
 */

#ifndef LFE_TAB_TONE_H
#define LFE_TAB_TONE_H

#if defined(MAXTRACKER_LFE) && defined(MAXTRACKER_LFE_ENABLE_TESTTONE)

#include "wv_common.h"

extern const wv_tab wv_tab_tone;

#endif /* MAXTRACKER_LFE && MAXTRACKER_LFE_ENABLE_TESTTONE */

#endif /* LFE_TAB_TONE_H */
