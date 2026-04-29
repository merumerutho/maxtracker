/*
 * devkitpro_compat.h
 *
 * Compatibility shim: maps BlocksDS libnds symbol names used by maxmod
 * to their devkitPro libnds equivalents.
 *
 * Injected via -include from maxmod.mak so that no maxmod source
 * files need editing.
 */
#ifndef DEVKITPRO_COMPAT_H
#define DEVKITPRO_COMPAT_H

#ifdef __NDS__

/* ---------- Timer ---------- */

#ifndef LIBNDS_DEFAULT_TIMER_MUSIC
#define LIBNDS_DEFAULT_TIMER_MUSIC  0
#endif

/* ---------- Master sound control register ---------- */

#ifndef SOUNDCNT_ENABLE
#define SOUNDCNT_ENABLE     SOUND_ENABLE
#endif
#ifndef SOUNDCNT_VOL
#define SOUNDCNT_VOL(n)     SOUND_VOL(n)
#endif

#ifndef SOUNDCNT_LEFT_OUTPUT_MASK
#define SOUNDCNT_LEFT_OUTPUT_MASK       (3 << 8)
#define SOUNDCNT_LEFT_OUTPUT_MIXER      (0 << 8)
#define SOUNDCNT_LEFT_OUTPUT_CH1        (1 << 8)
#define SOUNDCNT_LEFT_OUTPUT_CH3        (2 << 8)
#define SOUNDCNT_LEFT_OUTPUT_CH1_CH3    (3 << 8)
#endif
#ifndef SOUNDCNT_RIGHT_OUTPUT_MASK
#define SOUNDCNT_RIGHT_OUTPUT_MASK      (3 << 10)
#define SOUNDCNT_RIGHT_OUTPUT_MIXER     (0 << 10)
#define SOUNDCNT_RIGHT_OUTPUT_CH1       (1 << 10)
#define SOUNDCNT_RIGHT_OUTPUT_CH3       (2 << 10)
#define SOUNDCNT_RIGHT_OUTPUT_CH1_CH3   (3 << 10)
#endif

/* ---------- Per-channel sound control register ---------- */

#ifndef REG_SOUNDXCNT
#define REG_SOUNDXCNT(n)    SCHANNEL_CR(n)
#endif

#ifndef SOUNDXCNT_ENABLE
#define SOUNDXCNT_ENABLE        SCHANNEL_ENABLE
#endif
#ifndef SOUNDXCNT_REPEAT
#define SOUNDXCNT_REPEAT        SOUND_REPEAT
#endif
#ifndef SOUNDXCNT_FORMAT_8BIT
#define SOUNDXCNT_FORMAT_8BIT   SOUND_FORMAT_8BIT
#endif
#ifndef SOUNDXCNT_FORMAT_16BIT
#define SOUNDXCNT_FORMAT_16BIT  SOUND_FORMAT_16BIT
#endif

#ifndef SOUNDXCNT_VOL_MUL
#define SOUNDXCNT_VOL_MUL(n)    (n)
#endif
#ifndef SOUNDXCNT_PAN
#define SOUNDXCNT_PAN(n)        ((n) << 16)
#endif

/* ---------- Sound capture control bits ---------- */

#ifndef SNDCAPCNT_FORMAT_8BIT
#define SNDCAPCNT_FORMAT_8BIT   (1 << 3)
#endif
#ifndef SNDCAPCNT_START_BUSY
#define SNDCAPCNT_START_BUSY    (1 << 7)
#endif

/* ---------- DMA registers ---------- */

#ifndef REG_DMA1_DEST
#define REG_DMA1_DEST   DMA1_DEST
#endif

/* ---------- Per-channel sound sub-registers ---------- */

#ifndef REG_SOUNDXSAD
#define REG_SOUNDXSAD(n)    SCHANNEL_SOURCE(n)
#endif
#ifndef REG_SOUNDXTMR
#define REG_SOUNDXTMR(n)    SCHANNEL_TIMER(n)
#endif
#ifndef REG_SOUNDXPNT
#define REG_SOUNDXPNT(n)    SCHANNEL_REPEAT_POINT(n)
#endif
#ifndef REG_SOUNDXLEN
#define REG_SOUNDXLEN(n)    SCHANNEL_LENGTH(n)
#endif
#ifndef REG_SOUNDXVOL
#define REG_SOUNDXVOL(n)    SCHANNEL_VOL(n)
#endif
#ifndef REG_SOUNDXPAN
#define REG_SOUNDXPAN(n)    SCHANNEL_PAN(n)
#endif

/* ---------- fifoWaitAddressAsync (BlocksDS only) ---------- */

#include <nds/fifocommon.h>
#include <nds/interrupts.h>
#ifndef fifoWaitAddressAsync
static inline __attribute__((unused))
void fifoWaitAddressAsync(int channel)
{
    while (!fifoCheckAddress(channel))
        swiIntrWait(1, IRQ_FIFO_NOT_EMPTY);
}
#endif

/* ---------- ARM_CODE attribute ---------- */

#ifndef ARM_CODE
#define ARM_CODE __attribute__((target("arm")))
#endif

/* ---------- CPSR helpers (ARM7 only) ---------- */

#ifndef CPSR_FLAG_IRQ_DIS
#define CPSR_FLAG_IRQ_DIS   (1 << 7)
#endif

#ifdef ARM7

#ifndef getCPSR
__attribute__((target("arm"), noinline, unused))
static unsigned int getCPSR(void)
{
    unsigned int cpsr;
    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    return cpsr;
}
#endif

#ifndef setCPSR
__attribute__((target("arm"), noinline, unused))
static void setCPSR(unsigned int val)
{
    __asm__ volatile ("msr cpsr, %0" :: "r"(val));
}
#endif

#endif /* ARM7 */

/* ---------- libndsCrash stub ---------- */

#ifndef libndsCrash
static inline __attribute__((noreturn, unused))
void libndsCrash(const char *msg)
{
    (void)msg;
    for (;;) ;
}
#endif

#endif /* __NDS__ */
#endif /* DEVKITPRO_COMPAT_H */
