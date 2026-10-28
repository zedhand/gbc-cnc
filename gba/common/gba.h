/* Minimal GBA hardware definitions — no libgba/libtonc dependency. */
#ifndef GBA_H
#define GBA_H
#include <stdint.h>

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;

#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))

#ifdef HOST_BUILD   /* host test harness: registers are plain variables */
extern volatile u16 hreg[0x210];
extern volatile u32 hreg_siodata32;
extern volatile u16 hreg_bios_if;
extern void (*volatile hreg_isr)(void);
#undef REG16
#undef REG32
#define REG16(a) (hreg[((a) - 0x04000000) / 2])
#define REG32(a) hreg_siodata32
#endif

/* video */
#define REG_DISPCNT   REG16(0x04000000)
#define REG_DISPSTAT  REG16(0x04000004)
#define REG_VCOUNT    REG16(0x04000006)
#define DCNT_MODE3    0x0003
#define DCNT_BG2      0x0400
#define DSTAT_VBL_IRQ 0x0008
#define SCREEN_W      240
#define SCREEN_H      160

/* keys (active low in REG_KEYINPUT) */
#define REG_KEYINPUT  REG16(0x04000130)
#define KEY_A      0x0001
#define KEY_B      0x0002
#define KEY_SELECT 0x0004
#define KEY_START  0x0008
#define KEY_RIGHT  0x0010
#define KEY_LEFT   0x0020
#define KEY_UP     0x0040
#define KEY_DOWN   0x0080
#define KEY_R      0x0100
#define KEY_L      0x0200
#define KEY_MASK   0x03FF

/* serial */
#define REG_SIODATA32 REG32(0x04000120)
#define REG_SIOCNT    REG16(0x04000128)
#define REG_RCNT      REG16(0x04000134)
#define SIO_CLK_INT   0x0001   /* 0 = external clock (we are the slave) */
#define SIO_SO_HIGH   0x0008   /* SO level while inactive               */
#define SIO_START     0x0080   /* slave: "ready"; cleared by hardware when a transfer completes */
#define SIO_32BIT     0x1000   /* Normal mode, 32-bit                   */
#define SIO_IRQ       0x4000

/* interrupts */
#define REG_IE        REG16(0x04000200)
#define REG_IF        REG16(0x04000202)
#define REG_IME       REG16(0x04000208)
#ifdef HOST_BUILD
#define BIOS_IF       hreg_bios_if
#define ISR_MAIN      hreg_isr
#else
#define BIOS_IF       (*(volatile u16 *)0x03007FF8)
#define ISR_MAIN      (*(void (*volatile *)(void))0x03007FFC)
#endif
#define IRQ_VBLANK    0x0001
#define IRQ_SERIAL    0x0080

/* code placement: IWRAM is 32-bit, zero wait state — use it for the interrupt path */
#ifdef HOST_BUILD
#define IWRAM_CODE
#else
#define IWRAM_CODE __attribute__((section(".iwram"), long_call, target("arm"), noinline))
#endif

#ifdef HOST_BUILD
void vblank_wait(void);
#else
static inline void vblank_wait(void)
{
#if defined(__thumb__)
    __asm__ volatile("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory");
#else
    __asm__ volatile("swi 0x050000" ::: "r0", "r1", "r2", "r3", "memory");
#endif
}
#endif

#define RGB15(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

#endif
