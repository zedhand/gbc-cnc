/*
 * LinkBus v1 — shared framing for GBA link-port modules.
 * Plain C99, used unchanged by the GBA ROMs (devkitARM) and the ESP modules (Arduino).
 *
 * Layers (see docs/PROTOCOL.md):
 *   1. Word layer   — each 32-bit Normal-mode SIO transfer carries 0..3 bytes:
 *                     [31:24] = 0xA0 | count, [23:16] b0, [15:8] b1, [7:0] b2
 *   2. Packet layer — 0x7E, LEN, TYPE, PAYLOAD[LEN-1], CRC8(LEN..PAYLOAD)
 */
#ifndef LINKBUS_PROTO_H
#define LINKBUS_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LB_PROTO_VERSION   1

#define LB_WORD_HDR        0xA0u   /* upper nibble marks a LinkBus word          */
#define LB_WORD_HDR_MASK   0xFCu   /* low 2 bits of the header byte = byte count */

#define LB_SOF             0x7Eu
#define LB_MAX_PAYLOAD     240u    /* payload bytes after TYPE                   */
#define LB_MAX_FRAME       (LB_MAX_PAYLOAD + 4u) /* SOF+LEN+TYPE+payload+CRC     */

/* ---- word layer -------------------------------------------------------- */

/* Pack up to 3 bytes into a LinkBus word. n = 0 gives an idle word (0xA0000000). */
uint32_t lb_word_pack(const uint8_t *bytes, int n);

/* Unpack a received word. Returns byte count (0..3), or -1 if it is not a LinkBus word
 * (e.g. GBA not armed, multiboot BIOS reply, line floating). */
int lb_word_unpack(uint32_t word, uint8_t out[3]);

/* ---- packet layer ------------------------------------------------------ */

uint8_t lb_crc8(uint8_t crc, uint8_t b);   /* poly 0x07, init 0x00 */

/* Encode a frame into out. Returns frame length, or 0 if it does not fit / too long. */
size_t lb_encode(uint8_t *out, size_t outcap, uint8_t type, const void *payload, size_t plen);

typedef struct {
    uint8_t  state;
    uint8_t  len;                       /* TYPE + payload length                */
    uint8_t  idx;
    uint8_t  crc;
    uint8_t  buf[LB_MAX_PAYLOAD + 1];   /* buf[0] = TYPE, buf[1..] = payload     */
    uint16_t crc_errors;
    uint16_t frames_ok;
} lb_parser_t;

void lb_parser_init(lb_parser_t *p);

/* Feed one byte. Returns 1 when a complete, CRC-valid packet is in p->buf:
 * type = p->buf[0], payload = p->buf + 1, payload length = p->len - 1.
 * The packet stays valid until the next call. */
int lb_parser_feed(lb_parser_t *p, uint8_t b);

/* Call when an idle word (count 0) arrives. Senders queue whole frames atomically, so an
 * idle word is always a frame boundary: any half-received frame is junk and is dropped. */
void lb_parser_idle(lb_parser_t *p);

/* ---- little-endian helpers --------------------------------------------- */

static inline void lb_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void lb_put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t lb_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t lb_get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---- byte ring buffer (power-of-two size, single producer/consumer) ----- */

typedef struct {
    uint8_t *data;
    uint16_t mask;              /* size - 1 */
    volatile uint16_t head;     /* write index */
    volatile uint16_t tail;     /* read index  */
} lb_ring_t;

void     lb_ring_init(lb_ring_t *r, uint8_t *storage, uint16_t size_pow2);
uint16_t lb_ring_used(const lb_ring_t *r);
uint16_t lb_ring_free(const lb_ring_t *r);
int      lb_ring_put(lb_ring_t *r, uint8_t b);          /* 0 if full  */
int      lb_ring_get(lb_ring_t *r, uint8_t *b);         /* 0 if empty */
int      lb_ring_write(lb_ring_t *r, const uint8_t *src, uint16_t n); /* all-or-nothing */

#ifdef __cplusplus
}
#endif
#endif
