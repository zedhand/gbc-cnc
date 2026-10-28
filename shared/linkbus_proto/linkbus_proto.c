#include "linkbus_proto.h"

/* ---- word layer -------------------------------------------------------- */

uint32_t lb_word_pack(const uint8_t *bytes, int n)
{
    uint32_t w;
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    w = (uint32_t)(LB_WORD_HDR | (uint8_t)n) << 24;
    if (n > 0) w |= (uint32_t)bytes[0] << 16;
    if (n > 1) w |= (uint32_t)bytes[1] << 8;
    if (n > 2) w |= (uint32_t)bytes[2];
    return w;
}

int lb_word_unpack(uint32_t word, uint8_t out[3])
{
    uint8_t hdr = (uint8_t)(word >> 24);
    int n;
    if ((hdr & LB_WORD_HDR_MASK) != LB_WORD_HDR) return -1;
    n = hdr & 0x03;
    out[0] = (uint8_t)(word >> 16);
    out[1] = (uint8_t)(word >> 8);
    out[2] = (uint8_t)word;
    return n;
}

/* ---- packet layer ------------------------------------------------------ */

uint8_t lb_crc8(uint8_t crc, uint8_t b)
{
    int i;
    crc ^= b;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    return crc;
}

size_t lb_encode(uint8_t *out, size_t outcap, uint8_t type, const void *payload, size_t plen)
{
    const uint8_t *pl = (const uint8_t *)payload;
    size_t i, n = 0;
    uint8_t crc = 0, len;

    if (plen > LB_MAX_PAYLOAD || outcap < plen + 4) return 0;
    len = (uint8_t)(plen + 1);

    out[n++] = LB_SOF;
    out[n++] = len;   crc = lb_crc8(crc, len);
    out[n++] = type;  crc = lb_crc8(crc, type);
    for (i = 0; i < plen; i++) { out[n++] = pl[i]; crc = lb_crc8(crc, pl[i]); }
    out[n++] = crc;
    return n;
}

enum { PS_SOF = 0, PS_LEN, PS_DATA, PS_CRC };

void lb_parser_init(lb_parser_t *p)
{
    p->state = PS_SOF;
    p->len = p->idx = p->crc = 0;
    p->crc_errors = 0;
    p->frames_ok = 0;
}

void lb_parser_idle(lb_parser_t *p)
{
    if (p->state != PS_SOF) p->crc_errors++;
    p->state = PS_SOF;
}

int lb_parser_feed(lb_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case PS_SOF:
        if (b == LB_SOF) p->state = PS_LEN;
        return 0;

    case PS_LEN:
        if (b == 0 || b > LB_MAX_PAYLOAD + 1) {
            /* bad length: if it was another SOF, stay ready for LEN */
            p->state = (b == LB_SOF) ? PS_LEN : PS_SOF;
            return 0;
        }
        p->len = b;
        p->idx = 0;
        p->crc = lb_crc8(0, b);
        p->state = PS_DATA;
        return 0;

    case PS_DATA:
        p->buf[p->idx++] = b;
        p->crc = lb_crc8(p->crc, b);
        if (p->idx >= p->len) p->state = PS_CRC;
        return 0;

    case PS_CRC:
        p->state = PS_SOF;
        if (b == p->crc) { p->frames_ok++; return 1; }
        p->crc_errors++;
        if (b == LB_SOF) p->state = PS_LEN;   /* cheap resync */
        return 0;
    }
    p->state = PS_SOF;
    return 0;
}

/* ---- ring buffer ------------------------------------------------------- */

void lb_ring_init(lb_ring_t *r, uint8_t *storage, uint16_t size_pow2)
{
    r->data = storage;
    r->mask = (uint16_t)(size_pow2 - 1);
    r->head = r->tail = 0;
}

uint16_t lb_ring_used(const lb_ring_t *r) { return (uint16_t)((r->head - r->tail) & r->mask); }
uint16_t lb_ring_free(const lb_ring_t *r) { return (uint16_t)(r->mask - lb_ring_used(r)); }

int lb_ring_put(lb_ring_t *r, uint8_t b)
{
    uint16_t next = (uint16_t)((r->head + 1) & r->mask);
    if (next == r->tail) return 0;
    r->data[r->head] = b;
    r->head = next;
    return 1;
}

int lb_ring_get(lb_ring_t *r, uint8_t *b)
{
    if (r->tail == r->head) return 0;
    *b = r->data[r->tail];
    r->tail = (uint16_t)((r->tail + 1) & r->mask);
    return 1;
}

int lb_ring_write(lb_ring_t *r, const uint8_t *src, uint16_t n)
{
    uint16_t i;
    if (lb_ring_free(r) < n) return 0;
    for (i = 0; i < n; i++) lb_ring_put(r, src[i]);
    return 1;
}
