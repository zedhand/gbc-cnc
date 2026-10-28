/* LinkBus CNC pendant — GBA multiboot ROM. Loaded over the link cable by the module. */
#include "gba.h"
#include "gfx.h"
#include "lb_gba.h"
#include "app.h"

void app_init(void);
void app_packet(u8 type, const u8 *p, u8 len);
void app_input(u16 keys);
void app_link_tick(u32 words_rx, u16 crc_errors);
void app_tick(void);

IWRAM_CODE static void isr(void)
{
    u16 f = REG_IF & REG_IE;
    if (f & IRQ_SERIAL) lb_irq();
    REG_IF = f;          /* acknowledge */
    BIOS_IF |= f;        /* for VBlankIntrWait */
}

int main(void)
{
    gfx_init();
    app_init();

    REG_IME = 0;
    ISR_MAIN = isr;
    REG_DISPSTAT |= DSTAT_VBL_IRQ;
    REG_IE = IRQ_VBLANK;
    lb_init();           /* adds IRQ_SERIAL and arms the first transfer */
    REG_IME = 1;

    for (;;) {
        u8 type, len;
        const u8 *pl;

        vblank_wait();
        app.frame++;

        while (lb_poll(&type, &pl, &len)) app_packet(type, pl, len);
        app_link_tick(lb_words_rx(), lb_crc_errors());
        app_input((u16)(~REG_KEYINPUT & KEY_MASK));
        app_tick();
        if (app.dirty || app.dirty_full) ui_draw();
    }
}
