#ifndef MT_PROJECT_VIEW_H
#define MT_PROJECT_VIEW_H
#include <nds.h>
void project_view_draw(u8 *top_fb, u8 *bot_fb);
void project_view_input(u32 down, u32 held);

/* Clear confirmation flags that shouldn't survive screen transitions. */
void project_view_reset_transient(void);

#endif
