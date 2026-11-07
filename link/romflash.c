#include "romflash.h"
#include "update_core.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ROM_SYNC 0x08
#define ROM_READ_REG 0x0A
#define ROM_FLASH_BEGIN 0x02
#define ROM_FLASH_DATA 0x03
#define ROM_FLASH_END 0x04
#define ROM_SPI_SET 0x0B
#define ROM_SPI_ATTACH 0x0D
#define ROM_READ_SLOW 0x0E
#define ROM_CHANGE_BAUD 0x0F
#define ROM_ERASE_FLASH 0xD0
#define ROM_ERASE_REGION 0xD1
#define ROM_BLOCK 0x400
#define ROM_STATUS 4
#define ROM_CHIP_REG 0x40001000u

static void fail(rom_result *r, const char *msg)
{
    if (r) snprintf(r->error, sizeof r->error, "%s", msg);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t xor_sum(const uint8_t *p, size_t n)
{
    uint32_t s = 0xEF;
    for (size_t i = 0; i < n; i++) s ^= p[i];
    return s;
}

static int slip_write(rom_io *io, const uint8_t *pkt, size_t n)
{
    uint8_t buf[2400];
    size_t o = 0;
    if (n > 1100) return -1;
    buf[o++] = 0xC0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = pkt[i];
        if (b == 0xC0) {
            buf[o++] = 0xDB;
            buf[o++] = 0xDC;
        } else if (b == 0xDB) {
            buf[o++] = 0xDB;
            buf[o++] = 0xDD;
        } else {
            buf[o++] = b;
        }
        if (o + 2 >= sizeof buf) return -1;
    }
    buf[o++] = 0xC0;
    return io->write(io->ctx, buf, o);
}

static int slip_read(rom_io *io, uint8_t *out, size_t cap, int timeout_ms)
{
    int started = 0;
    size_t n = 0;
    int esc = 0;
    for (int spins = 0; spins < 8000; spins++) {
        uint8_t b;
        int got = io->read(io->ctx, &b, 1, started ? 300 : timeout_ms);
        if (got < 0) return -1;
        if (got == 0) return -1;
        if (!started) {
            if (b == 0xC0) started = 1;
            continue;
        }
        if (b == 0xC0) {
            if (n == 0) continue;
            return (int)n;
        }
        if (esc) {
            if (b == 0xDC) b = 0xC0;
            else if (b == 0xDD) b = 0xDB;
            else return -1;
            esc = 0;
        } else if (b == 0xDB) {
            esc = 1;
            continue;
        }
        if (n >= cap) return -1;
        out[n++] = b;
    }
    return -1;
}

static int rom_cmd(rom_io *io, uint8_t op, const uint8_t *data, uint16_t dlen, uint32_t chk,
                   uint32_t *val_out, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                   int timeout_ms, char *err, size_t err_cap)
{
    uint8_t pkt[1200];
    if (op == ROM_ERASE_FLASH || op == ROM_ERASE_REGION) {
        snprintf(err, err_cap, "internal error: chip erase is not implemented");
        return -1;
    }
    if ((size_t)dlen + 8 > sizeof pkt) return -1;
    pkt[0] = 0x00;
    pkt[1] = op;
    pkt[2] = (uint8_t)dlen;
    pkt[3] = (uint8_t)(dlen >> 8);
    put_u32(pkt + 4, chk);
    if (dlen) memcpy(pkt + 8, data, dlen);
    if (slip_write(io, pkt, 8 + dlen) != 0) {
        snprintf(err, err_cap, "the serial port stopped accepting data");
        return -1;
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t frame[1400];
        int n = slip_read(io, frame, sizeof frame, timeout_ms);
        if (n < 8) continue;
        if (frame[0] != 0x01 || frame[1] != op) continue;
        uint16_t len = (uint16_t)(frame[2] | (frame[3] << 8));
        if ((size_t)n < 8u + len || len < ROM_STATUS) {
            snprintf(err, err_cap, "bootloader reply was too short");
            return -1;
        }
        const uint8_t *st = frame + 8 + len - ROM_STATUS;
        if (st[0] != 0) {
            snprintf(err, err_cap, "bootloader rejected a command (status %u)", st[0]);
            return -1;
        }
        if (val_out) *val_out = get_u32(frame + 4);
        size_t body = (size_t)len - ROM_STATUS;
        if (body > payload_cap) {
            snprintf(err, err_cap, "bootloader reply was too large");
            return -1;
        }
        if (body && payload) memcpy(payload, frame + 8, body);
        if (payload_len) *payload_len = body;
        return 0;
    }
    snprintf(err, err_cap, "no bootloader reply");
    return -1;
}

static void pulse_download(rom_io *io)
{
    io->set_dtr_rts(io->ctx, 0, 1);
    io->sleep_ms(io->ctx, 100);
    io->set_dtr_rts(io->ctx, 1, 0);
    io->sleep_ms(io->ctx, 50);
    io->set_dtr_rts(io->ctx, 0, 0);
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
}

static void hard_reset(rom_io *io)
{
    io->set_dtr_rts(io->ctx, 0, 1);
    io->sleep_ms(io->ctx, 100);
    io->set_dtr_rts(io->ctx, 0, 0);
    io->sleep_ms(io->ctx, 100);
}

static int sync_chip(rom_io *io, char *err, size_t err_cap)
{
    uint8_t body[36];
    body[0] = 0x07;
    body[1] = 0x07;
    body[2] = 0x12;
    body[3] = 0x20;
    memset(body + 4, 0x55, 32);
    for (int i = 0; i < 8; i++) {
        uint32_t val = 0;
        if (rom_cmd(io, ROM_SYNC, body, 36, 0, &val, NULL, 0, NULL, 150, err, err_cap) == 0)
            return 0;
    }
    snprintf(err, err_cap, "the chip did not enter the bootloader");
    return -1;
}

static int read_exact(rom_io *io, uint32_t addr, uint8_t *dst, uint32_t len, char *err, size_t err_cap)
{
    uint32_t done = 0;
    while (done < len) {
        uint8_t req[8], block[64];
        size_t got = 0;
        uint32_t chunk = len - done;
        if (chunk > 64) chunk = 64;
        put_u32(req, addr + done);
        put_u32(req + 4, chunk);
        if (rom_cmd(io, ROM_READ_SLOW, req, 8, 0, NULL, block, sizeof block, &got, 1000, err, err_cap) != 0)
            return -1;
        if (got < chunk) {
            snprintf(err, err_cap, "short read from flash");
            return -1;
        }
        memcpy(dst + done, block, chunk);
        done += chunk;
    }
    return 0;
}

static int read_u32_reg(rom_io *io, uint32_t addr, uint32_t *val, char *err, size_t err_cap)
{
    uint8_t req[4];
    put_u32(req, addr);
    return rom_cmd(io, ROM_READ_REG, req, 4, 0, val, NULL, 0, NULL, 500, err, err_cap);
}

static int attach_flash(rom_io *io, char *err, size_t err_cap)
{
    uint8_t arg[8];
    uint8_t params[24];
    memset(arg, 0, sizeof arg);
    if (rom_cmd(io, ROM_SPI_ATTACH, arg, 8, 0, NULL, NULL, 0, NULL, 1000, err, err_cap) != 0)
        return -1;
    memset(params, 0, sizeof params);
    put_u32(params + 4, 16u * 1024u * 1024u);
    put_u32(params + 8, 64u * 1024u);
    put_u32(params + 12, 4096);
    put_u32(params + 16, 256);
    put_u32(params + 20, 0xFFFF);
    return rom_cmd(io, ROM_SPI_SET, params, 24, 0, NULL, NULL, 0, NULL, 1000, err, err_cap);
}

static int change_baud(rom_io *io, uint32_t baud, char *err, size_t err_cap)
{
    uint8_t req[8];
    if (baud > UPD_FLASH_BAUD_MAX) {
        snprintf(err, err_cap, "refusing flash baud above 460800");
        return -1;
    }
    put_u32(req, baud);
    put_u32(req + 4, 0);
    if (rom_cmd(io, ROM_CHANGE_BAUD, req, 8, 0, NULL, NULL, 0, NULL, 500, err, err_cap) != 0)
        return -1;
    if (io->set_baud(io->ctx, baud) != 0) {
        snprintf(err, err_cap, "the PC serial port rejected the flash baud");
        return -1;
    }
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
    return 0;
}

static int flash_region(rom_io *io, uint32_t offset, const uint8_t *data, uint32_t len,
                        char *err, size_t err_cap, void (*progress)(void *, const char *, unsigned, unsigned), void *pctx)
{
    uint32_t erase = (len + 4095u) & ~4095u;
    uint32_t blocks = (len + ROM_BLOCK - 1) / ROM_BLOCK;
    uint8_t begin[20];
    if (!upd_range_is_app_slot(offset, len) || offset + erase < offset || offset + erase > UPD_APP_SLOT_END) {
        snprintf(err, err_cap, "refusing a write outside the factory app slot");
        return -1;
    }
    put_u32(begin, erase);
    put_u32(begin + 4, blocks);
    put_u32(begin + 8, ROM_BLOCK);
    put_u32(begin + 12, offset);
    put_u32(begin + 16, 0);
    if (rom_cmd(io, ROM_FLASH_BEGIN, begin, 20, 0, NULL, NULL, 0, NULL, 30000, err, err_cap) != 0)
        return -1;
    for (uint32_t seq = 0; seq < blocks; seq++) {
        uint8_t pkt[16 + ROM_BLOCK];
        uint32_t off = seq * ROM_BLOCK;
        uint32_t n = len - off;
        if (n > ROM_BLOCK) n = ROM_BLOCK;
        memset(pkt, 0xFF, sizeof pkt);
        put_u32(pkt, ROM_BLOCK);
        put_u32(pkt + 4, seq);
        put_u32(pkt + 8, 0);
        put_u32(pkt + 12, 0);
        memcpy(pkt + 16, data + off, n);
        if (rom_cmd(io, ROM_FLASH_DATA, pkt, 16 + ROM_BLOCK, xor_sum(pkt + 16, ROM_BLOCK),
                    NULL, NULL, 0, NULL, 5000, err, err_cap) != 0)
            return -1;
        if (progress && (seq % 8 == 0 || seq + 1 == blocks))
            progress(pctx, "write", off + n, len);
    }
    {
        uint8_t endp[4];
        put_u32(endp, 1); /* stay in the bootloader for the readback */
        if (rom_cmd(io, ROM_FLASH_END, endp, 4, 0, NULL, NULL, 0, NULL, 2000, err, err_cap) != 0)
            return -1;
    }
    return 0;
}

int rom_update_app(rom_io *io, const uint8_t *image, uint32_t image_len,
                   const char *package_version, int allow_same, int do_write,
                   int (*backup_write)(void *ctx, const uint8_t *data, size_t n),
                   void *backup_ctx, rom_result *result)
{
    char err[160];
    uint8_t *backup = NULL;
    uint32_t chip = 0, old_len = 0, new_len = 0;
    uint8_t table[UPD_PART_TABLE_LEN];
    uint8_t header[64];
    if (!result) return -1;
    memset(result, 0, sizeof *result);
    err[0] = 0;
    if (!io || !io->write || !io->read || !io->set_baud || !io->set_dtr_rts || !io->flush_rx || !io->sleep_ms) {
        fail(result, "internal error: missing serial operations");
        return -1;
    }
    if (!image || !upd_image_length(image, image_len, &new_len, err, sizeof err) || new_len != image_len ||
        !upd_range_is_app_slot(UPD_APP_OFFSET, image_len)) {
        fail(result, err[0] ? err : "new image failed its local checks");
        return -1;
    }
    {
        char img_ver[32];
        if (!upd_image_version(image, image_len, img_ver, sizeof img_ver, err, sizeof err) ||
            !package_version || strcmp(img_ver, package_version) != 0) {
            fail(result, "new image version does not match the package");
            return -1;
        }
    }
    if (io->cancelled && io->cancelled(io->ctx)) {
        fail(result, "cancelled before the macropad was restarted");
        return -1;
    }
    if (io->progress) io->progress(io->ctx, "reset", 0, 1);
    pulse_download(io);
    if (sync_chip(io, err, sizeof err) != 0) goto leave;
    if (read_u32_reg(io, ROM_CHIP_REG, &chip, err, sizeof err) != 0) goto leave;
    if (chip != UPD_CHIP_ID_S3) {
        snprintf(err, sizeof err, "this is not an ESP32-S3 (id %u); nothing was written", chip);
        goto leave;
    }
    if (attach_flash(io, err, sizeof err) != 0) goto leave;
    if (change_baud(io, UPD_FLASH_BAUD_MAX, err, sizeof err) != 0) {
        /* 115200 still works. Keep going rather than inventing a higher rate. */
        err[0] = 0;
    }
    if (read_exact(io, UPD_PART_TABLE, table, sizeof table, err, sizeof err) != 0) goto leave;
    if (!upd_partition_is_cp7(table, sizeof table, err, sizeof err)) goto leave;
    if (read_exact(io, UPD_APP_OFFSET, header, sizeof header, err, sizeof err) != 0) goto leave;
    if (header[0] != UPD_IMAGE_MAGIC) {
        snprintf(err, sizeof err, "no application image at 0x10000; nothing was written");
        goto leave;
    }
    if (!upd_image_length(header, sizeof header, &old_len, err, sizeof err)) {
        /* Header alone cannot hold the segments. Walk them from the device. */
        unsigned segs = header[1];
        uint16_t id = (uint16_t)(header[12] | (header[13] << 8));
        size_t pos = 24;
        if (id != UPD_CHIP_ID_S3 || segs == 0 || segs > 16) {
            snprintf(err, sizeof err, "the application header is not CrowPanel firmware");
            goto leave;
        }
        for (unsigned i = 0; i < segs; i++) {
            uint8_t sh[8];
            uint32_t sz;
            if (read_exact(io, UPD_APP_OFFSET + (uint32_t)pos, sh, 8, err, sizeof err) != 0) goto leave;
            sz = get_u32(sh + 4);
            if (sz > UPD_FACTORY_SIZE) {
                snprintf(err, sizeof err, "the installed application is an unexpected size");
                goto leave;
            }
            pos += 8u + sz;
        }
        old_len = ((uint32_t)pos + 16u) & ~15u;
        if (header[23] == 1) old_len += 32;
    }
    if (old_len < 24 || old_len > UPD_FACTORY_SIZE) {
        snprintf(err, sizeof err, "could not measure the installed application");
        goto leave;
    }
    backup = (uint8_t *)malloc(old_len);
    if (!backup) {
        snprintf(err, sizeof err, "not enough memory to back up the application");
        goto leave;
    }
    if (io->progress) io->progress(io->ctx, "backup", 0, old_len);
    if (read_exact(io, UPD_APP_OFFSET, backup, old_len, err, sizeof err) != 0) goto leave;
    {
        uint8_t edge[64];
        uint32_t tail = old_len >= 64 ? old_len - 64 : 0;
        if (read_exact(io, UPD_APP_OFFSET, edge, 64, err, sizeof err) != 0) goto leave;
        if (memcmp(edge, backup, 64) != 0) {
            snprintf(err, sizeof err, "backup read did not repeat; nothing was written");
            goto leave;
        }
        if (old_len >= 64) {
            if (read_exact(io, UPD_APP_OFFSET + tail, edge, 64, err, sizeof err) != 0) goto leave;
            if (memcmp(edge, backup + tail, 64) != 0) {
                snprintf(err, sizeof err, "backup tail did not repeat; nothing was written");
                goto leave;
            }
        }
    }
    if (!upd_image_version(backup, old_len, result->device_version, sizeof result->device_version, err, sizeof err))
        goto leave;
    result->identified = 1;
    err[0] = 0;
    {
        upd_ver dev, pkg;
        int cmp;
        if (!upd_ver_parse(result->device_version, &dev) || !upd_ver_parse(package_version, &pkg)) {
            snprintf(err, sizeof err, "version on the board could not be compared");
            goto leave;
        }
        cmp = upd_ver_cmp(pkg, dev);
        if (cmp < 0) {
            snprintf(err, sizeof err, "refusing to downgrade %s to %s", result->device_version, package_version);
            goto leave_reset;
        }
        if (cmp == 0 && !allow_same) {
            snprintf(err, sizeof err, "the board is already on %s", result->device_version);
            goto leave_reset;
        }
    }
    if (!do_write) {
        hard_reset(io);
        snprintf(result->error, sizeof result->error, "dry run: board is %s and the layout is cp7; nothing was written",
                 result->device_version);
        free(backup);
        return 0;
    }
    if (io->cancelled && io->cancelled(io->ctx)) {
        snprintf(err, sizeof err, "cancelled before writing");
        goto leave_reset;
    }
    if (!backup_write || backup_write(backup_ctx, backup, old_len) != 0) {
        snprintf(err, sizeof err, "could not save the backup on this PC; nothing was written");
        goto leave_reset;
    }
    if (io->progress) io->progress(io->ctx, "write", 0, image_len);
    if (flash_region(io, UPD_APP_OFFSET, image, image_len, err, sizeof err, io->progress, io->ctx) != 0) {
        result->wrote = 1;
        goto restore;
    }
    result->wrote = 1;
    {
        uint8_t *check = (uint8_t *)malloc(image_len);
        if (!check) {
            snprintf(err, sizeof err, "not enough memory to read the new image back");
            goto restore;
        }
        if (io->progress) io->progress(io->ctx, "readback", 0, image_len);
        if (read_exact(io, UPD_APP_OFFSET, check, image_len, err, sizeof err) != 0 ||
            memcmp(check, image, image_len) != 0) {
            free(check);
            snprintf(err, sizeof err, "readback did not match the new image");
            goto restore;
        }
        free(check);
    }
    hard_reset(io);
    free(backup);
    snprintf(result->error, sizeof result->error, "updated to %s", package_version);
    return 0;

restore:
    if (backup && flash_region(io, UPD_APP_OFFSET, backup, old_len, err, sizeof err, NULL, NULL) == 0) {
        uint8_t *check = (uint8_t *)malloc(old_len);
        if (check && read_exact(io, UPD_APP_OFFSET, check, old_len, err, sizeof err) == 0 &&
            memcmp(check, backup, old_len) == 0) {
            result->restored = 1;
            snprintf(result->error, sizeof result->error,
                     "the new image did not verify; the previous app was written back. NVS was not erased. %s",
                     err);
            free(check);
            hard_reset(io);
            free(backup);
            return -1;
        }
        free(check);
    }
    snprintf(result->error, sizeof result->error,
             "write failed and the previous app was not verified. NVS was not targeted, but the app may not boot. "
             "Use the backup file with a 115200-baud app-only write at 0x10000. This is not an atomic update. %s",
             err);
    hard_reset(io);
    free(backup);
    return -1;

leave_reset:
    hard_reset(io);
leave:
    fail(result, err[0] ? err : "update stopped");
    free(backup);
    return -1;
}
