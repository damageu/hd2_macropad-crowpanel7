#include "sha256.h"
#include <string.h>

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void block(sha256_ctx *c, const uint8_t b[64])
{
    uint32_t w[64];
    unsigned i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->s[0], b0 = c->s[1], cc = c->s[2], d = c->s[3];
    uint32_t e = c->s[4], f = c->s[5], g = c->s[6], h = c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b0) ^ (a & cc) ^ (b0 & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b0; b0 = a; a = t1 + t2;
    }
    c->s[0] += a; c->s[1] += b0; c->s[2] += cc; c->s[3] += d;
    c->s[4] += e; c->s[5] += f; c->s[6] += g; c->s[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->s[0] = 0x6a09e667; c->s[1] = 0xbb67ae85; c->s[2] = 0x3c6ef372; c->s[3] = 0xa54ff53a;
    c->s[4] = 0x510e527f; c->s[5] = 0x9b05688c; c->s[6] = 0x1f83d9ab; c->s[7] = 0x5be0cd19;
    c->bits = 0;
    c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bits += (uint64_t)len * 8;
    while (len) {
        unsigned take = 64 - c->n;
        if (take > len) take = (unsigned)len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            block(c, c->buf);
            c->n = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        while (c->n < 64) c->buf[c->n++] = 0;
        block(c, c->buf);
        c->n = 0;
    }
    while (c->n < 56) c->buf[c->n++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->n++] = (uint8_t)(c->bits >> (i * 8));
    block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->s[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->s[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->s[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->s[i];
    }
}

void sha256_buf(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

int sha256_hex(const void *data, size_t len, char *out, size_t out_cap)
{
    static const char *hexd = "0123456789abcdef";
    uint8_t dig[32];
    if (out_cap < 65) return -1;
    sha256_buf(data, len, dig);
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0xf];
    }
    out[64] = 0;
    return 0;
}

int sha256_digest_hex(const uint8_t dig[32], char *out, size_t out_cap)
{
    static const char *hexd = "0123456789abcdef";
    if (!dig || !out || out_cap < 65) return -1;
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0xf];
    }
    out[64] = 0;
    return 0;
}

int sha256_hex_eq(const char *hex, const uint8_t dig[32])
{
    static const char *hexd = "0123456789abcdef";
    if (!hex) return 0;
    for (int i = 0; i < 32; i++) {
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        if (a != hexd[dig[i] >> 4] || b != hexd[dig[i] & 0xf]) return 0;
    }
    return hex[64] == 0;
}
