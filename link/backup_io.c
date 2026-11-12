#include "backup_io.h"
#include <string.h>

void hd2_bk_reset(hd2_bk *b)
{
    if (!b) return;
    memset(b, 0, sizeof *b);
}

int hd2_bk_begin(hd2_bk *b, FILE *f)
{
    if (!b || !f) return -1;
    memset(b, 0, sizeof *b);
    b->f = f;
    sha256_init(&b->sha);
    return 0;
}

int hd2_bk_write(hd2_bk *b, uint32_t off, const uint8_t *data, uint32_t n)
{
    if (!b || !b->f || !data || n == 0) return -1;
    if ((uint64_t)off != b->got) return -1;
    if (b->have_sha) return -1; /* digest already finalized */
    if (fwrite(data, 1, n, b->f) != n || fflush(b->f) != 0) return -1;
    sha256_update(&b->sha, data, n);
    b->got += n;
    return 0;
}

int hd2_bk_read(hd2_bk *b, uint32_t off, uint8_t *dst, uint32_t n)
{
    if (!b || !b->f || !dst || n == 0) return -1;
    if ((uint64_t)off + n > b->got) return -1;
    /* A write-only FILE fails here. "wb+" is required. fflush then fseek
     * is the C rule for switching from writing to reading. */
    if (fflush(b->f) != 0) return -1;
    if (fseek(b->f, (long)off, SEEK_SET) != 0) return -1;
    if (fread(dst, 1, n, b->f) != n) return -1;
    if (fseek(b->f, (long)b->got, SEEK_SET) != 0) return -1;
    return 0;
}

int hd2_bk_digest(hd2_bk *b, uint8_t out[32])
{
    if (!b || !out) return -1;
    if (!b->have_sha) {
        sha256_final(&b->sha, b->dig);
        b->have_sha = 1;
    }
    memcpy(out, b->dig, 32);
    return 0;
}

int hd2_bk_sidecar(hd2_bk *b, FILE *out, const char *name)
{
    uint8_t dig[32];
    char hex[80];
    if (!out) return -1;
    if (hd2_bk_digest(b, dig) != 0) return -1;
    if (sha256_digest_hex(dig, hex, sizeof hex) != 0) return -1;
    if (fprintf(out, "%s  %s\n", hex, name && name[0] ? name : "flash-full.bin") < 0) return -1;
    if (fflush(out) != 0) return -1;
    return 0;
}
