#include "lb_multiboot.h"

static uint32_t crc_step(uint32_t crc, uint32_t data)
{
    int b;
    for (b = 0; b < 32; b++) {
        uint32_t bit = (crc ^ data) & 1u;
        crc = (crc >> 1) ^ (bit ? 0xC37Bu : 0u);
        data >>= 1;
    }
    return crc;
}

#define X(v) io->xfer(io->ctx, (uint32_t)(v))

int mb_probe(const mb_io_t *io)
{
    return (X(0x00006202) >> 16) == 0x7202;
}

mb_result_t mb_upload(const mb_io_t *io, uint32_t size, int handshake_tries)
{
    uint8_t  buf[0xC0];
    uint32_t r, i, fsize, seed, crcA, crcB, crcC;
    int      t;

    if (size < MB_MIN_SIZE || size > MB_MAX_SIZE) return MB_ERR_SIZE;

    /* 1. wait for the BIOS to answer the "recognition" word */
    for (t = 0; t < handshake_tries; t++) {
        if ((X(0x00006202) >> 16) == 0x7202) break;
        io->delay_ms(io->ctx, 15);
    }
    if (t == handshake_tries) return MB_ERR_NO_GBA;

    /* 2. header: 0xC0 bytes as 96 halfwords, unencrypted */
    X(0x00006102);
    if (!io->read(io->ctx, 0, buf, 0xC0)) return MB_ERR_READ;
    for (i = 0; i < 0xC0; i += 2)
        X(buf[i] | (buf[i + 1] << 8));
    X(0x00006200);

    /* 3. palette / key exchange */
    X(0x00006202);
    X(0x000063D1);
    r = X(0x000063D1);
    if ((r >> 24) != 0x73) return MB_ERR_HANDSHAKE;

    crcA = (r >> 16) & 0xFF;
    seed = 0xFFFF00D1u | (crcA << 8);
    crcA = (crcA + 0xF) & 0xFF;
    X(0x00006400 | crcA);

    fsize = (size + 0xF) & ~0xFu;
    r = X((fsize - 0x190) / 4);
    crcB = (r >> 16) & 0xFF;
    crcC = 0xC387;

    /* 4. encrypted payload from 0xC0 */
    for (i = 0xC0; i < fsize; i += 4) {
        uint8_t w[4];
        uint32_t dat;
        if (!io->read(io->ctx, i, w, 4)) return MB_ERR_READ;
        dat = (uint32_t)w[0] | ((uint32_t)w[1] << 8) | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
        crcC = crc_step(crcC, dat);
        seed = seed * 0x6F646573u + 1u;
        X(dat ^ (0xFE000000u - i) ^ seed ^ 0x43202F2Fu);
        if (io->yield && ((i & 0x3FF) == 0)) io->yield(io->ctx);
    }

    /* 5. final CRC */
    crcC = crc_step(crcC, 0xFFFF0000u | (crcB << 8) | crcA);
    X(0x00000065);
    for (t = 0; t < 200; t++) {
        if ((X(0x00000065) >> 16) == 0x0075) break;
        io->delay_ms(io->ctx, 1);
    }
    if (t == 200) return MB_ERR_CRC;
    X(0x00000066);
    X(crcC & 0xFFFF);
    return MB_OK;
}

const char *mb_strerror(mb_result_t r)
{
    switch (r) {
    case MB_OK:            return "ok";
    case MB_ERR_SIZE:      return "image size out of range (0x1C0..256K)";
    case MB_ERR_NO_GBA:    return "no GBA in multiboot mode";
    case MB_ERR_HANDSHAKE: return "unexpected reply during key exchange";
    case MB_ERR_READ:      return "image read failed";
    case MB_ERR_CRC:       return "GBA did not acknowledge final CRC";
    }
    return "?";
}
