#include "lb_gba.h"

static u8 rx_store[2048];
static u8 tx_store[1024];
static lb_ring_t rx, tx;
static lb_parser_t parser;
static volatile u32 words_rx;
static u8 last_idle = 1;   /* IRQ-only: one marker per run of idle words */
static lb_module_info_t module;

/* Interrupt path: ARM code in IWRAM, ring access inlined (no calls out to Thumb code). */
IWRAM_CODE void lb_irq(void)
{
    u32 in = REG_SIODATA32;
    u32 out;
    u8  hdr = (u8)(in >> 24);
    int n, i;

    words_rx++;

    /* 1. received word -> RX ring (one marker byte per idle word keeps frame boundaries) */
    if ((hdr & LB_WORD_HDR_MASK) == LB_WORD_HDR) {
        n = hdr & 3;
        if (n == 0) {
            /* first idle word after data = frame boundary: store marker FF 00 for lb_poll */
            if (!last_idle) {
                u16 h = rx.head;
                u16 n1 = (u16)((h + 1) & rx.mask), n2 = (u16)((h + 2) & rx.mask);
                if (n1 != rx.tail && n2 != rx.tail) {
                    rx.data[h] = 0xFF; rx.data[n1] = 0x00; rx.head = n2;
                    last_idle = 1;
                }
            }
        } else {
            last_idle = 0;
            for (i = 0; i < n; i++) {
                u8 b = (u8)(in >> (16 - 8 * i));
                u16 h = rx.head;
                u16 nx = (u16)((h + 1) & rx.mask);
                if (b == 0xFF) {            /* escape literal 0xFF as FF FF */
                    u16 nx2 = (u16)((nx + 1) & rx.mask);
                    if (nx == rx.tail || nx2 == rx.tail) break;
                    rx.data[h] = 0xFF; rx.data[nx] = 0xFF; rx.head = nx2;
                } else {
                    if (nx == rx.tail) break;
                    rx.data[h] = b; rx.head = nx;
                }
            }
        }
    }

    /* 2. next outgoing word from the TX ring */
    out = (u32)LB_WORD_HDR << 24;
    for (i = 0; i < 3 && tx.tail != tx.head; i++) {
        out |= (u32)tx.data[tx.tail] << (16 - 8 * i);
        tx.tail = (u16)((tx.tail + 1) & tx.mask);
    }
    out |= (u32)i << 24;

    /* 3. re-arm */
    REG_SIODATA32 = out;
    REG_SIOCNT |= SIO_START;
}

void lb_init(void)
{
    lb_ring_init(&rx, rx_store, sizeof(rx_store));
    lb_ring_init(&tx, tx_store, sizeof(tx_store));
    lb_parser_init(&parser);
    module.valid = 0;

    REG_RCNT = 0;                                   /* SIO mode, not GPIO/JOY */
    REG_SIOCNT = SIO_32BIT;                         /* Normal 32-bit, external clock */
    REG_SIOCNT = SIO_32BIT | SIO_SO_HIGH | SIO_IRQ;
    REG_SIODATA32 = (u32)LB_WORD_HDR << 24;         /* first reply: idle word */
    REG_SIOCNT |= SIO_START;
    REG_IE |= IRQ_SERIAL;
}

int lb_send(u8 type, const void *payload, u32 len)
{
    u8  frame[LB_MAX_FRAME];
    u32 n = (u32)lb_encode(frame, sizeof(frame), type, payload, len);
    u16 ime;
    int ok;
    if (!n) return 0;
    /* whole frame in one go, so the IRQ never sees half a frame (idle words = boundaries) */
    ime = REG_IME;
    REG_IME = 0;
    ok = lb_ring_write(&tx, frame, (u16)n);
    REG_IME = ime;
    return ok;
}

static void handle_common(u8 type, const u8 *p, u8 len)
{
    if (type == LB_HELLO && len >= 6) {
        int i;
        module.proto = p[0];
        module.module_id = lb_get16(p + 1);
        module.fw_version = lb_get16(p + 3);
        for (i = 0; i < (int)sizeof(module.name) - 1 && 5 + i < len && p[5 + i]; i++)
            module.name[i] = (char)p[5 + i];
        module.name[i] = 0;
        module.valid = 1;
    } else if (type == LB_PING) {
        lb_send(LB_PONG, p, len);
    }
}

int lb_poll(u8 *type, const u8 **payload, u8 *len)
{
    u8 b;
    static u8 esc = 0;
    while (lb_ring_get(&rx, &b)) {
        if (esc) {
            esc = 0;
            if (b == 0x00) { lb_parser_idle(&parser); continue; }
            /* b == 0xFF: literal 0xFF, fall through */
        } else if (b == 0xFF) {
            esc = 1;
            continue;
        }
        if (lb_parser_feed(&parser, b)) {
            u8 t = parser.buf[0];
            if (t == LB_HELLO || t == LB_PING) {
                handle_common(t, parser.buf + 1, (u8)(parser.len - 1));
                if (t == LB_PING) continue;
            }
            *type = t;
            *payload = parser.buf + 1;
            *len = (u8)(parser.len - 1);
            return 1;
        }
    }
    return 0;
}

u32  lb_words_rx(void)  { return words_rx; }
u16  lb_crc_errors(void){ return parser.crc_errors; }
const lb_module_info_t *lb_module(void) { return &module; }
void lb_forget_module(void) { module.valid = 0; }
