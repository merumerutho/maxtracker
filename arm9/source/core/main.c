#include <nds.h>
#include "screen.h"

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    screen_init();

    while (1) {
        swiWaitForVBlank();
        screen_flush();
    }

    return 0;
}
