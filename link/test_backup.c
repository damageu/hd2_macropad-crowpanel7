/* Catches the v1.2.5 first-install bugs:
 * - a write-only backup cannot be re-read, so the chip must not be programmed
 * - the sidecar must be the SHA-256 of the file bytes, not SHA-256 of that digest
 */
#include "backup_io.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static int read_sidecar_hex(const char *path, char *hex, size_t cap)
{
    FILE *f = fopen(path, "rb");
    char line[160];
    size_t n;
    if (!f) return -1;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strcspn(line, " \t\r\n");
    if (n != 64 || n + 1 > cap) return -1;
    memcpy(hex, line, n);
    hex[n] = 0;
    return 0;
}

int main(void)
{
    char dir[] = "backup-test-XXXXXX";
    char binpath[256], sidepath[256];
    uint8_t chunk[1000];
    uint8_t head[64], tail[64];
    uint8_t filedig[32], running[32];
    char good[80], hashed_digest[80], sidehex[80];
    hd2_bk bk;
    FILE *f;
    int i;
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(binpath, sizeof binpath, "%s/flash-full.bin", dir);
    snprintf(sidepath, sizeof sidepath, "%s/flash-full.bin.sha256", dir);
    for (i = 0; i < (int)sizeof chunk; i++) chunk[i] = (uint8_t)(i * 17 + 3);

    /* Write-only mode cannot re-read. That is the v1.2.5 failure. */
    f = fopen(binpath, "wb");
    expect(f != NULL, "wb opens");
    expect(hd2_bk_begin(&bk, f) == 0, "begin wb");
    expect(hd2_bk_write(&bk, 0, chunk, sizeof chunk) == 0, "write on wb");
    expect(hd2_bk_read(&bk, 0, head, 64) != 0, "write-only backup cannot be re-read");
    fclose(f);

    /* Read/write mode: re-read head and tail, then sidecar matches the file. */
    f = fopen(binpath, "wb+");
    expect(f != NULL, "wb+ opens");
    expect(hd2_bk_begin(&bk, f) == 0, "begin wb+");
    expect(hd2_bk_write(&bk, 0, chunk, 400) == 0, "first chunk");
    expect(hd2_bk_write(&bk, 400, chunk + 400, sizeof chunk - 400) == 0, "second chunk");
    expect(hd2_bk_read(&bk, 0, head, 64) == 0, "re-read head while the file is still open");
    expect(memcmp(head, chunk, 64) == 0, "head matches what was written");
    expect(hd2_bk_read(&bk, sizeof chunk - 64, tail, 64) == 0, "re-read tail");
    expect(memcmp(tail, chunk + sizeof chunk - 64, 64) == 0, "tail matches what was written");
    expect(hd2_bk_digest(&bk, running) == 0, "running digest");
    {
        FILE *sf = fopen(sidepath, "wb");
        expect(sf != NULL, "sidecar opens");
        expect(hd2_bk_sidecar(&bk, sf, "flash-full.bin") == 0, "sidecar write");
        fclose(sf);
    }
    fclose(f);

    /* After the handle is closed, the bytes on disk still re-read and hash. */
    f = fopen(binpath, "rb");
    expect(f != NULL, "reopen backup");
    expect(fread(head, 1, 64, f) == 64, "re-read head after close");
    expect(memcmp(head, chunk, 64) == 0, "closed file head");
    expect(fseek(f, (long)sizeof chunk - 64, SEEK_SET) == 0, "seek tail");
    expect(fread(tail, 1, 64, f) == 64, "re-read tail after close");
    expect(memcmp(tail, chunk + sizeof chunk - 64, 64) == 0, "closed file tail");
    fclose(f);
    {
        uint8_t *all = malloc(sizeof chunk);
        FILE *rf = fopen(binpath, "rb");
        expect(all && rf && fread(all, 1, sizeof chunk, rf) == sizeof chunk, "read whole backup");
        if (rf) fclose(rf);
        sha256_buf(all, sizeof chunk, filedig);
        free(all);
    }
    expect(memcmp(filedig, running, 32) == 0, "running digest is the file SHA-256");
    expect(sha256_digest_hex(filedig, good, sizeof good) == 0, "format digest");
    expect(sha256_hex(filedig, 32, hashed_digest, sizeof hashed_digest) == 0, "hash the digest");
    expect(strcmp(good, hashed_digest) != 0, "hashing the digest is not the file hash");
    expect(read_sidecar_hex(sidepath, sidehex, sizeof sidehex) == 0, "read sidecar");
    expect(strcmp(sidehex, good) == 0, "sidecar matches sha256sum of the file");
    expect(strcmp(sidehex, hashed_digest) != 0, "sidecar is not sha256 of the digest");

    remove(binpath);
    remove(sidepath);
    rmdir(dir);
    if (g_fail) {
        fprintf(stderr, "%d backup failures\n", g_fail);
        return 1;
    }
    puts("test_backup ok");
    return 0;
}
