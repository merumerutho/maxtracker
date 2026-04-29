#include <nds.h>
#include "song.h"
#include "screen.h"
#include "editor_state.h"
#include "navigation.h"
#include "disk_view.h"

bool song_modified  = false;
bool autosave_dirty = false;

char status_msg[64] = "";
int  status_timer   = 0;

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    song_init();
    screen_init();
    ui_apply_key_repeat();

    while (1) {
        swiWaitForVBlank();

        scanKeys();
        u32 kd = keysDown();
        u32 kh = keysHeld();

        navigation_handle_shift(kd, kh);

        screen_flush();
    }

    return 0;
}
