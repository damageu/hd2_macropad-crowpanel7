#ifndef HD2_BACKUP_IO_H
#define HD2_BACKUP_IO_H
/*
 * A flash backup that can be written and then read back on the same FILE.
 * The FILE must be opened for reading and writing ("wb+" / L"wb+").
 * "wb" is write-only: fread fails, and the installer must not program the chip.
 *
 * The sidecar line is the SHA-256 of the bytes written, hex-encoded.
 * It is not SHA-256(the digest).
 */
#include <stdio.h>
#include <stdint.h>
#include "sha256.h"

typedef struct {
    FILE *f;
    sha256_ctx sha;
    uint64_t got;
    int have_sha;
    uint8_t dig[32];
} hd2_bk;

void hd2_bk_reset(hd2_bk *b);
/* f is borrowed. It must be readable and writable, at position 0. */
int hd2_bk_begin(hd2_bk *b, FILE *f);
int hd2_bk_write(hd2_bk *b, uint32_t off, const uint8_t *data, uint32_t n);
int hd2_bk_read(hd2_bk *b, uint32_t off, uint8_t *dst, uint32_t n);
/* Running digest of every byte passed to hd2_bk_write. */
int hd2_bk_digest(hd2_bk *b, uint8_t out[32]);
/* "hex  name\n" using hd2_bk_digest. name is the label after the hash. */
int hd2_bk_sidecar(hd2_bk *b, FILE *out, const char *name);
#endif
