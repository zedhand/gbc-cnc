/*
 * GBA multiboot (Normal mode, 32-bit, module = clock master).
 * Protocol per GBATEK "BIOS Multi Boot (Single Game Pak)"; structure follows the well-known
 * akkera102 Arduino uploader. Hardware-independent: you supply a 32-bit transfer function.
 */
#ifndef LB_MULTIBOOT_H
#define LB_MULTIBOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MIN_SIZE  0x1C0u
#define MB_MAX_SIZE  0x40000u   /* 256 KiB, loads to EWRAM 0x02000000 */

typedef struct {
    /* exchange one 32-bit word, MSB first; returns what the GBA sent back */
    uint32_t (*xfer)(void *ctx, uint32_t out);
    /* sequential reader: copy n bytes starting at offset off into buf; bytes past the end
     * of the image must be zero-filled. Return 0 on I/O error. */
    int      (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t n);
    void     (*delay_ms)(void *ctx, uint32_t ms);
    void     (*yield)(void *ctx);        /* optional, called every 256 words (watchdog) */
    void     *ctx;
} mb_io_t;

typedef enum {
    MB_OK = 0,
    MB_ERR_SIZE,        /* image too small/large                 */
    MB_ERR_NO_GBA,      /* no 0x7202 handshake reply             */
    MB_ERR_HANDSHAKE,   /* unexpected reply mid-protocol         */
    MB_ERR_READ,        /* image read failed                     */
    MB_ERR_CRC          /* GBA never acknowledged the final CRC  */
} mb_result_t;

/* Is a GBA sitting in the multiboot BIOS right now? One probe transfer, non-blocking. */
int mb_probe(const mb_io_t *io);

/* Upload an image. handshake_tries x 15 ms is how long to wait for the BIOS. */
mb_result_t mb_upload(const mb_io_t *io, uint32_t size, int handshake_tries);

const char *mb_strerror(mb_result_t r);

#ifdef __cplusplus
}
#endif
#endif
