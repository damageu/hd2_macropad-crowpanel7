#ifndef HD2_SHA256_H
#define HD2_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t s[8];
    uint64_t bits;
    uint8_t buf[64];
    unsigned n;
} sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256_buf(const void *data, size_t len, uint8_t out[32]);
/* SHA-256 of data, then lowercase hex. 64 hex chars plus NUL. Returns 0 on success. */
int sha256_hex(const void *data, size_t len, char *out, size_t out_cap);
/* Lowercase hex of an existing 32-byte digest. Does not hash dig. */
int sha256_digest_hex(const uint8_t dig[32], char *out, size_t out_cap);
int sha256_hex_eq(const char *hex, const uint8_t dig[32]);
#endif
