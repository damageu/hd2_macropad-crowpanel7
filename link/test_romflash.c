#include "romflash.h"
#include "update_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

enum { MEM = 96 * 1024 };

typedef struct {
    uint8_t mem[MEM];
    uint8_t rx[16384];
    size_t rx_len, rx_pos;
    int fail_data_writes;
    int data_writes;
    int saw_erase;
    int began;
    uint32_t begin_off, begin_erase;
    int cancelled;
    int baud;
    int reads_left; /* 0 = unlimited; the read that hits 0 kills the port */
    int dead;
    unsigned progress_writes;
} fake_t;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int slip_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_n)
{
    size_t i = 0, o = 0;
    int started = 0, esc = 0;
    while (i < n) {
        uint8_t b = in[i++];
        if (!started) {
            if (b == 0xC0) started = 1;
            continue;
        }
        if (b == 0xC0) {
            *out_n = o;
            return 1;
        }
        if (esc) {
            if (b == 0xDC) b = 0xC0;
            else if (b == 0xDD) b = 0xDB;
            else return 0;
            esc = 0;
        } else if (b == 0xDB) {
            esc = 1;
            continue;
        }
        if (o >= cap) return 0;
        out[o++] = b;
    }
    return 0;
}

static void slip_queue(fake_t *f, const uint8_t *pkt, size_t n)
{
    if (f->rx_len + n * 2 + 2 >= sizeof f->rx) return;
    f->rx[f->rx_len++] = 0xC0;
    for (size_t i = 0; i < n; i++) {
        if (pkt[i] == 0xC0) {
            f->rx[f->rx_len++] = 0xDB;
            f->rx[f->rx_len++] = 0xDC;
        } else if (pkt[i] == 0xDB) {
            f->rx[f->rx_len++] = 0xDB;
            f->rx[f->rx_len++] = 0xDD;
        } else {
            f->rx[f->rx_len++] = pkt[i];
        }
    }
    f->rx[f->rx_len++] = 0xC0;
}

static void reply(fake_t *f, uint8_t op, uint32_t val, const uint8_t *body, size_t body_n, int status)
{
    uint8_t pkt[128 + 64];
    uint16_t len = (uint16_t)(body_n + 4);
    if ((size_t)len + 8 > sizeof pkt) return;
    pkt[0] = 1;
    pkt[1] = op;
    pkt[2] = (uint8_t)len;
    pkt[3] = (uint8_t)(len >> 8);
    put_u32(pkt + 4, val);
    if (body_n) memcpy(pkt + 8, body, body_n);
    pkt[8 + body_n] = (uint8_t)status;
    pkt[9 + body_n] = 0;
    pkt[10 + body_n] = 0;
    pkt[11 + body_n] = 0;
    slip_queue(f, pkt, 8 + len);
}

static void handle(fake_t *f, const uint8_t *pkt, size_t n)
{
    if (n < 8) return;
    uint8_t op = pkt[1];
    const uint8_t *data = pkt + 8;
    if (op == 0xD0 || op == 0xD1) {
        f->saw_erase = 1;
        reply(f, op, 0, NULL, 0, 1);
        return;
    }
    if (op == 0x08) {
        reply(f, op, 1, NULL, 0, 0);
        return;
    }
    if (op == 0x0A) {
        uint32_t addr = get_u32(data);
        reply(f, op, addr == 0x40001000u ? 9u : 0u, NULL, 0, 0);
        return;
    }
    if (op == 0x0D || op == 0x0B || op == 0x0F || op == 0x04) {
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    if (op == 0x0E) {
        uint32_t addr = get_u32(data);
        uint32_t want = get_u32(data + 4);
        uint8_t block[64];
        memset(block, 0xFF, sizeof block);
        if (want > 64) want = 64;
        if (addr < MEM) {
            uint32_t have = MEM - addr;
            if (have > want) have = want;
            memcpy(block, f->mem + addr, have);
        }
        reply(f, op, 0, block, 64, 0);
        return;
    }
    if (op == 0x02) {
        f->began++;
        f->begin_off = get_u32(data + 12);
        f->begin_erase = get_u32(data);
        if (f->begin_off == 0x10000 && f->begin_off + f->begin_erase <= 0xA10000 &&
            f->begin_off + f->begin_erase <= MEM) {
            memset(f->mem + f->begin_off, 0xFF, f->begin_erase);
        }
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    if (op == 0x03) {
        uint32_t seq = get_u32(data + 4);
        uint32_t chk = get_u32(pkt + 4);
        uint32_t expect = 0xEF;
        for (size_t i = 0; i < 0x400 && 16 + i < n; i++) expect ^= data[16 + i];
        f->data_writes++;
        if (f->fail_data_writes > 0 && f->data_writes == f->fail_data_writes) {
            reply(f, op, 0, NULL, 0, 5);
            return;
        }
        if ((chk & 0xFF) != (expect & 0xFF)) {
            reply(f, op, 0, NULL, 0, 6);
            return;
        }
        uint32_t dest = f->begin_off + seq * 0x400;
        if (dest < MEM && dest + 0x400 <= MEM) memcpy(f->mem + dest, data + 16, 0x400);
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    reply(f, op, 0, NULL, 0, 1);
}

static int fake_write(void *ctx, const uint8_t *data, size_t n)
{
    fake_t *f = ctx;
    uint8_t pkt[1400];
    size_t pn = 0;
    if (!slip_decode(data, n, pkt, sizeof pkt, &pn)) return -1;
    handle(f, pkt, pn);
    return 0;
}

static int fake_read(void *ctx, uint8_t *data, size_t n, int timeout_ms)
{
    fake_t *f = ctx;
    (void)timeout_ms;
    if (f->dead) return -1;
    if (f->reads_left > 0 && --f->reads_left == 0) {
        f->dead = 1;
        return -1;
    }
    if (f->rx_pos >= f->rx_len) return 0;
    size_t have = f->rx_len - f->rx_pos;
    if (have > n) have = n;
    memcpy(data, f->rx + f->rx_pos, have);
    f->rx_pos += have;
    return (int)have;
}

static int fake_baud(void *ctx, uint32_t baud)
{
    fake_t *f = ctx;
    if (baud > 460800) return -1;
    f->baud = (int)baud;
    return 0;
}

static void fake_lines(void *ctx, int dtr, int rts) { (void)ctx; (void)dtr; (void)rts; }
static void fake_flush(void *ctx)
{
    fake_t *f = ctx;
    f->rx_len = 0;
    f->rx_pos = 0;
}
static void fake_sleep(void *ctx, unsigned ms) { (void)ctx; (void)ms; }
static void fake_progress(void *ctx, const char *phase, unsigned done, unsigned total)
{
    fake_t *f = ctx;
    (void)phase; (void)done; (void)total;
    f->progress_writes++;
}
static int fake_cancel(void *ctx)
{
    fake_t *f = ctx;
    return f->cancelled;
}

static void build_image(uint8_t *img, size_t cap, uint32_t *len, const char *ver)
{
    memset(img, 0, cap);
    img[0] = 0xE9;
    img[1] = 1;
    img[12] = 9;
    img[23] = 1;
    size_t payload = 48;
    put_u32(img + 24, 0x42000020);
    put_u32(img + 28, (uint32_t)payload);
    memcpy(img + 32, ver, strlen(ver));
    size_t pos = 32 + payload;
    uint32_t padded = ((uint32_t)pos + 16u) & ~15u;
    img[padded - 1] = 0x5A;
    memset(img + padded, 0x11, 32);
    *len = padded + 32;
}

static void install_table(fake_t *f)
{
    uint8_t *t = f->mem + 0x8000;
    memset(t, 0xFF, 0xC00);
    memset(t, 0, 96);
    t[0] = 0xAA; t[1] = 0x50; t[2] = 0x01; t[3] = 0x02;
    put_u32(t + 4, 0x9000); put_u32(t + 8, 0x6000); memcpy(t + 12, "nvs", 3);
    t[32] = 0xAA; t[33] = 0x50; t[34] = 0x00; t[35] = 0x00;
    put_u32(t + 36, 0x10000); put_u32(t + 40, 0xA00000); memcpy(t + 44, "factory", 7);
    t[64] = 0xAA; t[65] = 0x50; t[66] = 0x01; t[67] = 0x81;
    put_u32(t + 68, 0xA10000); put_u32(t + 72, 0x400000); memcpy(t + 76, "assets", 6);
    t[95] = 0xFF;
    f->mem[0x9000] = 0xA5;
}

static uint8_t g_saved[512];
static size_t g_saved_n;
static int save_backup(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    if (n > sizeof g_saved) return -1;
    memcpy(g_saved, data, n);
    g_saved_n = n;
    return 0;
}

static rom_io io_of(fake_t *f)
{
    rom_io io;
    memset(&io, 0, sizeof io);
    io.ctx = f;
    io.write = fake_write;
    io.read = fake_read;
    io.set_baud = fake_baud;
    io.set_dtr_rts = fake_lines;
    io.flush_rx = fake_flush;
    io.sleep_ms = fake_sleep;
    io.progress = fake_progress;
    io.cancelled = fake_cancel;
    return io;
}

static void prep(fake_t *f, uint8_t *old_img, uint32_t old_len)
{
    memset(f, 0, sizeof *f);
    install_table(f);
    memcpy(f->mem + 0x10000, old_img, old_len);
    g_saved_n = 0;
}

int main(void)
{
    uint8_t old_img[256], new_img[256];
    uint32_t old_len = 0, new_len = 0;
    fake_t fake;
    rom_result rr;
    char err[80];
    uint32_t walked = 0;

    build_image(old_img, sizeof old_img, &old_len, "v1.8.6-cp7.2");
    build_image(new_img, sizeof new_img, &new_len, "v1.9.1-cp7.2");
    expect(upd_image_length(new_img, new_len, &walked, err, sizeof err), err);
    expect(walked == new_len, "walker length");
    {
        char ver[32];
        expect(upd_image_version(new_img, new_len, ver, sizeof ver, err, sizeof err), err);
        expect(strcmp(ver, "v1.9.1-cp7.2") == 0, "image version");
    }

    prep(&fake, old_img, old_len);
    rom_io io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
    expect(fake.saw_erase == 0, "no chip erase");
    expect(fake.baud <= 460800, "baud cap");
    expect(fake.mem[0x9000] == 0xA5, "nvs byte preserved");
    expect(memcmp(fake.mem + 0x10000, new_img, new_len) == 0, "new image read back in the mock");
    expect(g_saved_n == old_len, "backup complete");
    expect(memcmp(g_saved, old_img, old_len) == 0, "backup bytes");
    expect(rr.wrote == 1 && rr.restored == 0, "wrote once");

    prep(&fake, old_img, old_len);
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 0, save_backup, NULL, &rr) == 0, rr.error);
    expect(rr.wrote == 0, "dry run does not write");
    expect(strcmp(rr.device_version, "v1.8.6-cp7.2") == 0, "dry run saw old version");
    expect(memcmp(fake.mem + 0x10000, old_img, old_len) == 0, "dry run left the app");
    expect(fake.began == 0, "dry run sent no flash begin");

    prep(&fake, new_img, new_len);
    io = io_of(&fake);
    expect(rom_update_app(&io, old_img, old_len, "v1.8.6-cp7.2", 1, 1, save_backup, NULL, &rr) != 0, "downgrade must fail");
    expect(rr.wrote == 0, "downgrade did not write");
    expect(fake.mem[0x9000] == 0xA5, "nvs after refused downgrade");
    expect(strstr(rr.error, "downgrade") != NULL, rr.error);

    prep(&fake, old_img, old_len);
    fake.fail_data_writes = 1;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "injected write failure");
    expect(rr.wrote == 1, "failure happened after begin");
    expect(rr.restored == 1, "backup restored");
    expect(memcmp(fake.mem + 0x10000, old_img, old_len) == 0, "restored bytes");
    expect(fake.mem[0x9000] == 0xA5, "nvs after restore");
    expect(fake.saw_erase == 0, "restore did not erase the chip");

    prep(&fake, old_img, old_len);
    fake.cancelled = 1;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "cancel");
    expect(rr.wrote == 0 && fake.began == 0, "cancel before any flash command");
    expect(memcmp(fake.mem + 0x10000, old_img, old_len) == 0, "cancel left the image");

    prep(&fake, old_img, old_len);
    fake.mem[0x8000] = 0x00; /* break the partition magic */
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "bad layout");
    expect(rr.wrote == 0 && fake.began == 0, "bad layout never begins a write");

    prep(&fake, old_img, old_len);
    fake.reads_left = 3;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "disconnect");
    expect(rr.wrote == 0 && fake.began == 0, "disconnect before writing");
    expect(fake.mem[0x9000] == 0xA5, "nvs after disconnect");

    prep(&fake, old_img, old_len);
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, NULL, NULL, &rr) != 0, "missing backup callback");
    expect(rr.wrote == 0 && fake.began == 0, "no backup means no write");

    if (g_fail) {
        fprintf(stderr, "%d romflash failures\n", g_fail);
        return 1;
    }
    puts("test_romflash ok");
    return 0;
}
