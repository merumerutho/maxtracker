#include <nds.h>

volatile bool exitflag = false;

static void VblankHandler(void) {}

static void VcountHandler(void)
{
    inputGetAndSend();
}

static void powerButtonCB(void)
{
    exitflag = true;
}

int main(void)
{
    readUserSettings();

    irqInit();
    fifoInit();

    SetYtrigger(80);

    installSystemFIFO();

    irqSet(IRQ_VCOUNT, VcountHandler);
    irqSet(IRQ_VBLANK, VblankHandler);
    irqEnable(IRQ_VBLANK | IRQ_VCOUNT);

    setPowerButtonCB(powerButtonCB);

    while (!exitflag) {
        swiWaitForVBlank();
    }

    return 0;
}
