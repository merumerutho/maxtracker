#include <nds.h>
#include <maxmod7.h>

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
    dmaFillWords(0, (void *)0x04000400, 0x100);

    readUserSettings();

    irqInit();
    fifoInit();

    SetYtrigger(80);

    installSoundFIFO();

    mmInstall(FIFO_MAXMOD);

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
