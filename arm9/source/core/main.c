#include <nds.h>

int main(void)
{
    irqInit();
    irqEnable(IRQ_VBLANK);

    while (1) {
        swiWaitForVBlank();
    }

    return 0;
}
