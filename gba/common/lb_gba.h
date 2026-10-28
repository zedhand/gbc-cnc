/*
 * LinkBus v1 — GBA side. The GBA is the Normal-mode 32-bit SIO slave; the module clocks.
 * Every completed transfer raises the serial IRQ: we unpack the received word into the RX
 * ring, load the next outgoing word and re-arm. Reusable by any LinkBus ROM.
 */
#ifndef LB_GBA_H
#define LB_GBA_H
#include "gba.h"
#include "linkbus_proto.h"
#include "linkbus_ids.h"

typedef struct {
    u8  proto;
    u16 module_id;
    u16 fw_version;
    char name[24];
    u8  valid;
} lb_module_info_t;

void lb_init(void);                                 /* call before enabling IME */
void lb_irq(void);                                  /* call from the IRQ handler on IRQ_SERIAL */

int  lb_send(u8 type, const void *payload, u32 len);      /* 0 if TX buffer full */
/* Returns 1 per packet received (common packets are handled internally and not returned). */
int  lb_poll(u8 *type, const u8 **payload, u8 *len);

/* link health */
u32  lb_words_rx(void);          /* total transfers completed (increments even when idle) */
u16  lb_crc_errors(void);
const lb_module_info_t *lb_module(void);
void lb_forget_module(void);

#endif
