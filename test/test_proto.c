#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "linkbus_proto.h"
#include "linkbus_ids.h"
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } }while(0)

/* push a frame through the word layer (3 bytes/word) with injected garbage words */
static int roundtrip(const uint8_t *frame, size_t n, lb_parser_t *p, uint8_t *type, uint8_t *pl, int *plen, int garbage)
{
    size_t i = 0; int got = 0;
    while (i < n) {
        uint8_t b[3]; int k = (n - i) > 3 ? 3 : (int)(n - i);
        if (garbage && (rand() % 4 == 0)) { uint8_t o[3]; CHECK(lb_word_unpack(0xFFFFFFFFu, o) < 0); CHECK(lb_word_unpack(0x72020000u, o) < 0); }
        uint32_t w = lb_word_pack(frame + i, k);
        int m = lb_word_unpack(w, b); CHECK(m == k);
        for (int j = 0; j < m; j++) if (lb_parser_feed(p, b[j])) { *type = p->buf[0]; *plen = p->len - 1; memcpy(pl, p->buf + 1, *plen); got++; }
        i += k;
    }
    return got;
}

int main(void)
{
    lb_parser_t p; lb_parser_init(&p);
    uint8_t f[LB_MAX_FRAME], pl[LB_MAX_PAYLOAD], t; int plen;
    /* idle word */
    { uint8_t o[3]; CHECK(lb_word_unpack(lb_word_pack(NULL, 0), o) == 0); CHECK(lb_word_unpack(0, o) == -1); }
    /* all lengths, random payloads, including 0x7E bytes inside */
    srand(1);
    for (int len = 0; len <= (int)LB_MAX_PAYLOAD; len++) {
        uint8_t src[LB_MAX_PAYLOAD];
        for (int i = 0; i < len; i++) src[i] = (i % 5 == 0) ? 0x7E : (uint8_t)rand();
        size_t n = lb_encode(f, sizeof f, 0x40, src, len); CHECK(n == (size_t)len + 4);
        CHECK(roundtrip(f, n, &p, &t, pl, &plen, 1) == 1);
        CHECK(t == 0x40 && plen == len && memcmp(pl, src, len) == 0);
    }
    CHECK(lb_encode(f, sizeof f, 1, pl, LB_MAX_PAYLOAD + 1) == 0);
    /* corruption: flip a byte -> no packet, next good frame still parses */
    { uint8_t s[10] = {1,2,3,4,5,6,7,8,9,10}; size_t n = lb_encode(f, sizeof f, 0x11, s, 10);
      f[6] ^= 0x55; CHECK(roundtrip(f, n, &p, &t, pl, &plen, 0) == 0); CHECK(p.crc_errors >= 1);
      f[6] ^= 0x55; CHECK(roundtrip(f, n, &p, &t, pl, &plen, 0) == 1); }
    /* truncated frame followed by good frame (resync) */
    { uint8_t s[20]; memset(s, 0x7E, sizeof s); size_t n = lb_encode(f, sizeof f, 0x12, s, 20);
      roundtrip(f, 9, &p, &t, pl, &plen, 0);
      lb_parser_idle(&p);                 /* idle word between frames */
      CHECK(roundtrip(f, n, &p, &t, pl, &plen, 0) == 1 && t == 0x12); }
    /* ring */
    { uint8_t st[16]; lb_ring_t r; lb_ring_init(&r, st, 16); CHECK(lb_ring_free(&r) == 15);
      uint8_t d[15]; for (int i = 0; i < 15; i++) d[i] = i; CHECK(lb_ring_write(&r, d, 15)); CHECK(!lb_ring_put(&r, 1));
      uint8_t b; for (int i = 0; i < 15; i++) { CHECK(lb_ring_get(&r, &b) && b == i); } CHECK(!lb_ring_get(&r, &b)); }
    printf(fails ? "proto: %d FAILURES\n" : "proto: all tests passed\n", fails);
    return fails != 0;
}
