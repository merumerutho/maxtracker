#include <nds.h>
#include "song.h"
#include "screen.h"

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    song_init();
    screen_init();

    while (1) {
        swiWaitForVBlank();
        screen_flush();
    }

    return 0;
}
