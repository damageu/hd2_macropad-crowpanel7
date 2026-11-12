#ifndef HD2_MD5_H
#define HD2_MD5_H
/* RFC 1321 MD5. Used only to compare flash contents with the flasher stub's
 * SPI_FLASH_MD5 and READ_FLASH digests. Not used for any security decision;
 * package integrity is still SHA-256. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t n);
void md5_final(md5_ctx *c, uint8_t out[16]);
void md5_buf(const void *data, size_t n, uint8_t out[16]);
#endif
