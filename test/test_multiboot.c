/* Simulated GBA BIOS for the multiboot uploader. Replies lag one transfer, like real SPI. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lb_multiboot.h"

static uint8_t rom[0x3000]; static uint32_t romsize;
static uint8_t got[0x40000]; static uint32_t got_len;
static uint32_t reply = 0, st = 0, seed, crcA, crcB = 0x5A, crcC, idx, fsize, final_crc, waits;
static int boot_ok = 0, xfers = 0;

static uint32_t crc_step(uint32_t crc, uint32_t d){ for(int b=0;b<32;b++){uint32_t bit=(crc^d)&1;crc=(crc>>1)^(bit?0xC37B:0);d>>=1;} return crc; }

static uint32_t bios(void *c, uint32_t w)
{
    (void)c; uint32_t out = reply; xfers++;
    switch (st) {
    case 0: if (w == 0x6202) { reply = 0x72020000; st = 1; } else reply = 0; break;
    case 1: if (w == 0x6102) { st = 2; idx = 0; reply = 0x72020000; } else if (w==0x6202) reply=0x72020000; break;
    case 2: got[idx] = w & 0xFF; got[idx+1] = (w >> 8) & 0xFF; idx += 2; reply = ((0xC0 - idx)/2) << 16; if (idx == 0xC0) st = 3; break;
    case 3: if (w == 0x6200) st = 4; break;
    case 4: if (w == 0x6202) st = 5; break;
    case 5: if (w == 0x63D1) { reply = 0x73CC0000; st = 6; } break;   /* key 0xCC */
    case 6: if ((w >> 8) == 0x64) { crcA = w & 0xFF; if (crcA != ((0xCC + 0xF) & 0xFF)) { printf("bad crcA\n"); exit(1);} seed = 0xFFFF00D1 | (0xCC << 8); reply = 0x73000000 | (crcB << 16); st = 7; } else reply = 0x73CC0000; break;
    case 7: fsize = w * 4 + 0x190; idx = 0xC0; crcC = 0xC387; st = 8; reply = 0; break;
    case 8: { seed = seed * 0x6F646573u + 1; uint32_t d = w ^ (0xFE000000u - idx) ^ seed ^ 0x43202F2Fu;
              memcpy(got + idx, &d, 4); crcC = crc_step(crcC, d); idx += 4; if (idx >= fsize) { st = 9; got_len = idx; } } break;
    case 9: if (w == 0x65) { reply = (++waits > 2) ? 0x00750000 : 0x00740000; } else if (w == 0x66) st = 10; break;
    case 10: final_crc = w; st = 11; boot_ok = (w == (crc_step(crcC, 0xFFFF0000u | (crcB << 8) | crcA) & 0xFFFF)); break;
    }
    return out;
}
static int rd(void *c, uint32_t off, uint8_t *b, uint32_t n){ (void)c; for(uint32_t i=0;i<n;i++) b[i] = (off+i < romsize) ? rom[off+i] : 0; return 1; }
static void dl(void *c, uint32_t ms){ (void)c; (void)ms; }

int main(void)
{
    romsize = 0x2345; for (uint32_t i = 0; i < romsize; i++) rom[i] = (uint8_t)(i * 7 + 3);
    mb_io_t io = { bios, rd, dl, NULL, NULL };
    mb_result_t r = mb_upload(&io, romsize, 10);
    int ok = (r == MB_OK) && boot_ok && got_len == ((romsize + 15) & ~15u) && memcmp(got, rom, romsize) == 0;
    for (uint32_t i = romsize; i < got_len; i++) if (got[i]) ok = 0;
    printf("multiboot: result=%s boot_crc=%s data=%s xfers=%d -> %s\n", mb_strerror(r), boot_ok ? "match" : "MISMATCH",
           memcmp(got, rom, romsize) == 0 ? "match" : "MISMATCH", xfers, ok ? "PASS" : "FAIL");
    /* no GBA */
    st = 99; reply = 0; r = mb_upload(&io, romsize, 3); printf("no-gba: %s -> %s\n", mb_strerror(r), r == MB_ERR_NO_GBA ? "PASS" : "FAIL");
    return !(ok && r == MB_ERR_NO_GBA);
}
