#include "romflash.h"
#include "update_core.h"
#include "md5.h"
#include "third_party/miniz/miniz.h"
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

#ifndef TEST_MEM
/* Assets live at 0xA10000 and the image is 0x400000 bytes, so the mock
 * flash has to cover through 0xE10000. */
#define TEST_MEM 0xE20000
#endif
enum { MEM = TEST_MEM };

typedef struct {
    uint8_t mem[MEM];
    uint8_t rx[262144];
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
    int ignore_syncs;          /* first N SYNC commands get no reply */
    int silent_flash_end;      /* FLASH_END gets no reply; later commands still work */
    int drop_after_write;      /* after all FLASH_DATA, go silent (no FLASH_END or later) */
    int dead_after;            /* set once drop_after_write trips */
    int data_expected;         /* when drop_after_write, silence starts after this many FLASH_DATA */
    int sync_count;
    int silent_first_sync;     /* first SYNC after a classic reset is silent */
    int sync_replies;          /* extra identical SYNC replies to queue (0 = one) */
    int classic_reset_required;/* SYNC only answers when in download mode */
    int in_download;
    int syncs_this_reset;
    int lost_reset_to_reopen;
    int reopen_calls;
    int classic_resets;
    int saw_en_low;
    int saw_boot_hold;
    int dtr, rts;
    unsigned progress_writes;
    unsigned backup_done_max;
    unsigned sync_attempts_seen;
    unsigned log_lines;
    char last_phase[32];
    char last_log[160];
    /* v1.2.3 flasher stub emulation */
    int stub_ok;            /* ROM accepts MEM_BEGIN/DATA/END and starts the stub */
    int stub_no_ohai;       /* MEM_END runs, but the stub never greets; chip hangs until a classic reset */
    int stub_reject_baud;   /* stub rejects CHANGE_BAUD */
    int stub_bad_digest;    /* READ_FLASH final digest is wrong (every read) */
    int stub_corrupt_once;  /* first FLASH_DEFL_END leaves one wrong byte */
    int stub_fail_defl_seq; /* FLASH_DEFL_DATA with this seq+1 is rejected */
    int hung;
    int in_stub;
    uint32_t mem_bytes, mem_expect, mem_begins;
    int mem_bad_chk;
    uint32_t baud_new, baud_old;
    int spi_set_stub;
    int read_active;
    uint32_t rd_addr, rd_len, rd_sent, rd_acked, rd_frames, rd_acks, rd_calls, rd_big_calls;
    md5_ctx rd_md5;
    int defl_active, defl_begins, defl_ends, defl_bad, assets_defl;
    uint32_t defl_size, defl_off, defl_blocks, defl_bs, defl_next_seq, defl_len;
    uint8_t defl_buf[MEM];
    int md5_calls;
    unsigned clock_ms;
    int saw_backup, saw_write, saw_verify, saw_check, saw_stub_phase;
    unsigned write_done_max, verify_done_max;
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
    if (f->rx_pos > 0) {
        memmove(f->rx, f->rx + f->rx_pos, f->rx_len - f->rx_pos);
        f->rx_len -= f->rx_pos;
        f->rx_pos = 0;
    }
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

static void reply_stub(fake_t *f, uint8_t op, uint32_t val, const uint8_t *body, size_t body_n, uint16_t status)
{
    uint8_t pkt[128];
    uint16_t len = (uint16_t)(body_n + 2);
    pkt[0] = 1;
    pkt[1] = op;
    pkt[2] = (uint8_t)len;
    pkt[3] = (uint8_t)(len >> 8);
    put_u32(pkt + 4, val);
    if (body_n) memcpy(pkt + 8, body, body_n);
    pkt[8 + body_n] = (uint8_t)(status >> 8); /* esp-flasher-stub: big-endian status */
    pkt[9 + body_n] = (uint8_t)status;
    slip_queue(f, pkt, 8 + len);
}

/* Mirrors span_ok() for the offsets a first-install is allowed to program. */
static int mock_span_ok(uint32_t off, uint32_t size)
{
    uint32_t erase = (size + 4095u) & ~4095u;
    if (size == 0 || off + size < off || off + erase < off || off + erase > MEM) return 0;
    if (off < 0xF000u && 0x9000u < off + erase) return 0; /* NVS */
    if (off < 0x10000u && 0xF000u < off + erase) return 0; /* phy_init */
    if (off == 0 && erase <= 0x8000u) return 1;
    if (off == 0x8000u && erase <= 0x1000u) return 1;
    if (off == 0x10000u && off + erase <= 0xA10000u) return 1;
    if (off == 0xA10000u && erase <= 0x400000u && off + erase <= 0xE10000u) return 1;
    return 0;
}

static void stub_queue_read_frame(fake_t *f)
{
    uint8_t block[4096];
    uint32_t n = f->rd_len - f->rd_sent;
    if (n > 4096) n = 4096;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = f->rd_addr + f->rd_sent + i;
        block[i] = a < MEM ? f->mem[a] : 0xFF;
    }
    md5_update(&f->rd_md5, block, n);
    slip_queue(f, block, n);
    f->rd_sent += n;
    f->rd_frames++;
}

static void handle_stub(fake_t *f, const uint8_t *pkt, size_t n)
{
    uint8_t op;
    const uint8_t *data = pkt + 8;
    if (n == 4 && f->read_active) {
        uint32_t ack = get_u32(pkt);
        f->rd_acks++;
        if (ack > f->rd_sent || ack < f->rd_acked) f->defl_bad = 1; /* nonsense ack */
        f->rd_acked = ack;
        if (f->rd_sent < f->rd_len) {
            stub_queue_read_frame(f);
        } else if (f->rd_acked == f->rd_len) {
            uint8_t d[16];
            md5_final(&f->rd_md5, d);
            if (f->stub_bad_digest) d[0] ^= 0xFF;
            slip_queue(f, d, 16);
            f->read_active = 0;
        }
        return;
    }
    if (n < 8 || pkt[0] != 0) return;
    op = pkt[1];
    switch (op) {
    case 0x08:
        reply_stub(f, op, 0, NULL, 0, 0);
        return;
    case 0x0F:
        f->baud_new = get_u32(data);
        f->baud_old = get_u32(data + 4);
        reply_stub(f, op, 0, NULL, 0, f->stub_reject_baud ? 0xC000 : 0);
        return;
    case 0x0A:
        reply_stub(f, op, get_u32(data) == 0x40001000u ? 9u : 0u, NULL, 0, 0);
        return;
    case 0x0B:
        f->spi_set_stub++;
        reply_stub(f, op, 0, NULL, 0, n == 8 + 24 ? 0 : 0xC000);
        return;
    case 0x13: {
        uint32_t addr = get_u32(data), size = get_u32(data + 4);
        md5_ctx c;
        uint8_t d[16];
        md5_init(&c);
        for (uint32_t i = 0; i < size; i++) {
            uint8_t b = addr + i < MEM ? f->mem[addr + i] : 0xFF;
            md5_update(&c, &b, 1);
        }
        md5_final(&c, d);
        f->md5_calls++;
        reply_stub(f, op, 0, d, 16, 0);
        return;
    }
    case 0xD2: {
        uint32_t i, window;
        f->rd_addr = get_u32(data);
        f->rd_len = get_u32(data + 4);
        if (get_u32(data + 8) != 4096 || get_u32(data + 12) != 64) {
            reply_stub(f, op, 0, NULL, 0, 0xC000);
            return;
        }
        f->rd_calls++;
        if (f->rd_len >= 0x4000) f->rd_big_calls++;
        f->rd_sent = f->rd_acked = 0;
        md5_init(&f->rd_md5);
        f->read_active = 1;
        reply_stub(f, op, 0, NULL, 0, 0);
        window = 2; /* a couple of packets in flight, then one per ack */
        for (i = 0; i < window && f->rd_sent < f->rd_len; i++) stub_queue_read_frame(f);
        return;
    }
    case 0x10:
        f->defl_size = get_u32(data);
        f->defl_blocks = get_u32(data + 4);
        f->defl_bs = get_u32(data + 8);
        f->defl_off = get_u32(data + 12);
        f->defl_len = 0;
        f->defl_next_seq = 0;
        f->defl_begins++;
        if (f->defl_off == 0xA10000u) f->assets_defl++;
        if (n != 8 + 20 || f->defl_bs != 0x4000 || !mock_span_ok(f->defl_off, f->defl_size)) {
            reply_stub(f, op, 0, NULL, 0, 0xC000);
            return;
        }
        memset(f->mem + f->defl_off, 0xFF, (f->defl_size + 4095u) & ~4095u);
        f->defl_active = 1;
        f->began++;
        reply_stub(f, op, 0, NULL, 0, 0);
        return;
    case 0x11: {
        uint32_t len = get_u32(data), seq = get_u32(data + 4);
        uint32_t chk = get_u32(pkt + 4), x = 0xEF;
        if (!f->defl_active || 24 + len != n || seq != f->defl_next_seq) {
            f->defl_bad = 1;
            reply_stub(f, op, 0, NULL, 0, 0xC000);
            return;
        }
        for (uint32_t i = 0; i < len; i++) x ^= data[16 + i];
        if ((chk & 0xFF) != (x & 0xFF)) {
            reply_stub(f, op, 0, NULL, 0, 0xC100);
            return;
        }
        if (f->stub_fail_defl_seq && (int)seq + 1 == f->stub_fail_defl_seq) {
            f->stub_fail_defl_seq = 0;
            reply_stub(f, op, 0, NULL, 0, 0xC400);
            return;
        }
        memcpy(f->defl_buf + f->defl_len, data + 16, len);
        f->defl_len += len;
        f->defl_next_seq++;
        reply_stub(f, op, 0, NULL, 0, 0);
        return;
    }
    case 0x12: {
        size_t out;
        if (!f->defl_active || get_u32(data) != 1) {
            reply_stub(f, op, 0, NULL, 0, 0xC600);
            return;
        }
        out = tinfl_decompress_mem_to_mem(f->mem + f->defl_off, MEM - f->defl_off, f->defl_buf, f->defl_len,
                                          TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32);
        f->defl_active = 0;
        f->defl_ends++;
        if (out != f->defl_size || f->defl_next_seq != f->defl_blocks) {
            reply_stub(f, op, 0, NULL, 0, 0xC800);
            return;
        }
        if (f->stub_corrupt_once) {
            f->stub_corrupt_once = 0;
            f->mem[f->defl_off + 40] ^= 0x01;
        }
        reply_stub(f, op, 0, NULL, 0, 0);
        return;
    }
    default:
        reply_stub(f, op, 0, NULL, 0, 0xC300);
        return;
    }
}

static void handle(fake_t *f, const uint8_t *pkt, size_t n)
{
    if (f->in_stub && !f->dead_after && !f->hung && n == 4) {
        handle_stub(f, pkt, n); /* READ_FLASH ack */
        return;
    }
    if (n < 8) return;
    uint8_t op = pkt[1];
    const uint8_t *data = pkt + 8;
    if (op == 0xD0 || op == 0xD1) {
        f->saw_erase = 1;
        reply(f, op, 0, NULL, 0, 1);
        return;
    }
    if (f->dead_after || f->hung)
        return;
    if (f->in_stub) {
        handle_stub(f, pkt, n);
        return;
    }
    if (op == 0x05 && f->stub_ok) {
        f->mem_begins++;
        f->mem_expect = get_u32(data);
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    if (op == 0x07 && f->stub_ok) {
        uint32_t len = get_u32(data);
        uint32_t chk = get_u32(pkt + 4), x = 0xEF;
        for (uint32_t i = 0; i < len && 24 + i < n; i++) x ^= data[16 + i];
        if ((chk & 0xFF) != (x & 0xFF) || 24 + len != n) f->mem_bad_chk = 1;
        f->mem_bytes += len;
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    if (op == 0x06 && f->stub_ok) {
        reply(f, op, 0, NULL, 0, 0);
        if (f->stub_no_ohai) {
            f->hung = 1;
            return;
        }
        slip_queue(f, (const uint8_t *)"OHAI", 4);
        f->in_stub = 1;
        return;
    }
    if (op == 0x08) {
        f->sync_count++;
        if (f->classic_reset_required && !f->in_download)
            return;
        if (f->ignore_syncs > 0) {
            f->ignore_syncs--;
            return;
        }
        if (f->silent_first_sync && f->syncs_this_reset == 0) {
            f->syncs_this_reset++;
            return;
        }
        f->syncs_this_reset++;
        {
            int n = f->sync_replies > 0 ? f->sync_replies : 1;
            int i;
            for (i = 0; i < n; i++)
                reply(f, op, 1, NULL, 0, 0);
        }
        return;
    }
    if (op == 0x0A) {
        uint32_t addr = get_u32(data);
        reply(f, op, addr == 0x40001000u ? 9u : 0u, NULL, 0, 0);
        return;
    }
    if (op == 0x04) {
        if (f->silent_flash_end)
            return;
        if (f->drop_after_write) {
            f->dead_after = 1;
            return;
        }
        reply(f, op, 0, NULL, 0, 0);
        return;
    }
    if (op == 0x0D || op == 0x0B || op == 0x0F) {
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
        if (mock_span_ok(f->begin_off, f->begin_erase > 0 ? f->begin_erase : 1) &&
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

static FILE *g_dump; /* HD2_WIRE_DUMP: decoded host packets, one hex line each */
static int fake_write(void *ctx, const uint8_t *data, size_t n)
{
    fake_t *f = ctx;
    static uint8_t pkt[20000];
    size_t pn = 0;
    if (!slip_decode(data, n, pkt, sizeof pkt, &pn)) return -1;
    if (g_dump) {
        for (size_t i = 0; i < pn; i++) fprintf(g_dump, "%02x", pkt[i]);
        fputc('\n', g_dump);
    }
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

static void fake_lines(void *ctx, int dtr, int rts)
{
    fake_t *f = ctx;
    /* ClassicReset: (DTR=0,RTS=1) -> (1,0) -> (0,0). Hard reset: (0,1) -> (0,0). */
    if (dtr == 0 && rts == 1) {
        f->saw_en_low = 1;
        f->saw_boot_hold = 0;
    } else if (f->saw_en_low && dtr == 1 && rts == 0) {
        f->saw_boot_hold = 1;
    } else if (f->saw_boot_hold && dtr == 0 && rts == 0) {
        f->in_download = 1;
        f->in_stub = 0;
        f->hung = 0;
        f->read_active = 0;
        f->syncs_this_reset = 0;
        f->classic_resets++;
        f->saw_en_low = 0;
        f->saw_boot_hold = 0;
    } else if (f->saw_en_low && !f->saw_boot_hold && dtr == 0 && rts == 0) {
        f->in_download = 0;
        f->in_stub = 0;
        f->hung = 0;
        f->read_active = 0;
        f->saw_en_low = 0;
    }
    f->dtr = dtr;
    f->rts = rts;
}

static int fake_reopen(void *ctx)
{
    fake_t *f = ctx;
    f->reopen_calls++;
    /* A CH340 open can toggle DTR/RTS and drop the chip out of download mode
     * if this happens between the reset pulse and the first successful SYNC. */
    if (f->in_download && f->syncs_this_reset == 0) {
        f->in_download = 0;
        f->lost_reset_to_reopen = 1;
    }
    return 0;
}

static void fake_log(void *ctx, const char *msg)
{
    fake_t *f = ctx;
    f->log_lines++;
    if (msg) snprintf(f->last_log, sizeof f->last_log, "%s", msg);
    if (msg && getenv("HD2_TEST_LOG")) fprintf(stderr, "[%6u] %s\n", f->clock_ms, msg);
}
static void fake_flush(void *ctx)
{
    fake_t *f = ctx;
    f->rx_len = 0;
    f->rx_pos = 0;
}
static void fake_sleep(void *ctx, unsigned ms)
{
    fake_t *f = ctx;
    f->clock_ms += ms;
}
static unsigned fake_now(void *ctx)
{
    fake_t *f = ctx;
    return f->clock_ms += 1;
}
static void fake_progress(void *ctx, const char *phase, unsigned done, unsigned total)
{
    fake_t *f = ctx;
    (void)total;
    f->progress_writes++;
    if (phase) {
        snprintf(f->last_phase, sizeof f->last_phase, "%s", phase);
        if (strcmp(phase, "backup") == 0 && done > f->backup_done_max)
            f->backup_done_max = done;
        if (strcmp(phase, "sync") == 0 && done > f->sync_attempts_seen)
            f->sync_attempts_seen = done;
        if (strcmp(phase, "backup") == 0) f->saw_backup = 1;
        if (strncmp(phase, "write", 5) == 0) {
            f->saw_write = 1;
            if (done > f->write_done_max) f->write_done_max = done;
        }
        if (strncmp(phase, "verify", 6) == 0) {
            f->saw_verify = 1;
            if (done > f->verify_done_max) f->verify_done_max = done;
        }
        if (strcmp(phase, "check") == 0) f->saw_check = 1;
        if (strcmp(phase, "stub") == 0) f->saw_stub_phase = 1;
    }
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

static uint8_t g_saved[MEM];
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
    io.log = fake_log;
    io.now_ms = fake_now;
    return io;
}

static void prep(fake_t *f, uint8_t *old_img, uint32_t old_len)
{
    memset(f, 0, sizeof *f);
    install_table(f);
    memcpy(f->mem + 0x10000, old_img, old_len);
    g_saved_n = 0;
}

/* Larger image: one segment with a pseudo-random payload, so it spans several
 * 4 KB READ_FLASH packets and several 16 KB FLASH_DEFL blocks. */
static void build_big(uint8_t *img, size_t cap, uint32_t *len, const char *ver, uint32_t payload, uint32_t seed)
{
    uint32_t x = seed;
    memset(img, 0, cap);
    img[0] = 0xE9;
    img[1] = 1;
    img[12] = 9;
    img[23] = 1;
    put_u32(img + 24, 0x42000020);
    put_u32(img + 28, payload);
    for (uint32_t i = 0; i < payload; i++) {
        x = x * 1103515245u + 12345u;
        img[32 + i] = (uint8_t)(x >> 16);
        if ((i & 255) > 200) img[32 + i] = 0; /* some compressible runs */
    }
    memset(img + 32 + 56, 0, 40);
    memcpy(img + 32 + 64, ver, strlen(ver));
    {
        size_t pos = 32 + payload;
        uint32_t padded = ((uint32_t)pos + 16u) & ~15u;
        img[padded - 1] = 0x5A;
        memset(img + padded, 0x11, 32);
        *len = padded + 32;
    }
}

static uint8_t g_chip_backup[0xE20000];
static uint32_t g_chip_backup_len;
static int g_backup_during_write;

static int chip_backup_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    fake_t *f = ctx;
    if (f->began || f->defl_begins || f->data_writes) g_backup_during_write = 1;
    if ((uint64_t)off + n > sizeof g_chip_backup) return -1;
    memcpy(g_chip_backup + off, data, n);
    if (off + n > g_chip_backup_len) g_chip_backup_len = off + n;
    return 0;
}

static int chip_backup_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)
{
    (void)ctx;
    if ((uint64_t)off + n > g_chip_backup_len) return -1;
    memcpy(dst, g_chip_backup + off, n);
    return 0;
}

static int chip_backup_fail(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    (void)ctx;
    (void)off;
    (void)data;
    (void)n;
    return -1;
}

static void fill_cp7_table(uint8_t *t)
{
    memset(t, 0xFF, 0xC00);
    memset(t, 0, 96);
    t[0] = 0xAA; t[1] = 0x50; t[2] = 0x01; t[3] = 0x02;
    put_u32(t + 4, 0x9000); put_u32(t + 8, 0x6000); memcpy(t + 12, "nvs", 3);
    t[32] = 0xAA; t[33] = 0x50; t[34] = 0x00; t[35] = 0x00;
    put_u32(t + 36, 0x10000); put_u32(t + 40, 0xA00000); memcpy(t + 44, "factory", 7);
    t[64] = 0xAA; t[65] = 0x50; t[66] = 0x01; t[67] = 0x81;
    put_u32(t + 68, 0xA10000); put_u32(t + 72, 0x400000); memcpy(t + 76, "assets", 6);
}

static void prep_stock(fake_t *f)
{
    memset(f, 0, sizeof *f);
    memset(f->mem, 0xFF, sizeof f->mem);
    f->mem[0x9000] = 0xA5;
    f->mem[0xF000] = 0x5A;
    f->mem[0xA10010] = 0x6E; /* stock byte inside the assets region */
    f->stub_ok = 1;
    f->classic_reset_required = 1;
    f->silent_first_sync = 1;
    f->sync_replies = 8;
    g_chip_backup_len = 0;
    g_backup_during_write = 0;
}

static int g_find_calls;
static const uint8_t *g_find_src;
static uint32_t g_find_len;
static int find_backup_cb(void *ctx, uint32_t len, const uint8_t md5[16], uint8_t *out)
{
    uint8_t d[16];
    (void)ctx;
    g_find_calls++;
    if (!g_find_src || len != g_find_len) return -1;
    md5_buf(g_find_src, len, d);
    if (memcmp(d, md5, 16) != 0) return -1;
    memcpy(out, g_find_src, len);
    return 0;
}

static void prep_stub(fake_t *f, const uint8_t *old_img, uint32_t old_len)
{
    memset(f, 0, sizeof *f);
    install_table(f);
    memcpy(f->mem + 0x10000, old_img, old_len);
    f->stub_ok = 1;
    f->classic_reset_required = 1;
    f->silent_first_sync = 1;
    f->sync_replies = 8;
    g_saved_n = 0;
}

int main(void)
{
    uint8_t old_img[256], new_img[256];
    uint32_t old_len = 0, new_len = 0;
    static fake_t fake;
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
    expect(rr.verify_ran == 1 && rr.verified == 1, "happy path verified");
    expect(rr.confirmed == 1, "happy path confirmed after reset");
    expect(strcmp(rr.confirmed_version, "v1.9.1-cp7.2") == 0, "confirmed version");
    expect(fake.backup_done_max == old_len, "backup progress reached the end");
    expect(fake.sync_attempts_seen >= 1, "sync progress was reported");

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

    /* Slow sync: first three SYNC frames are ignored, then the chip answers. */
    prep(&fake, old_img, old_len);
    fake.ignore_syncs = 3;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
    expect(fake.sync_count >= 4, "slow sync needed more than one attempt");
    expect(fake.sync_attempts_seen >= 2, "slow sync showed attempt numbers");
    expect(rr.wrote == 1 && rr.verified == 1 && rr.restored == 0, "slow sync still verified");
    expect(memcmp(fake.mem + 0x10000, new_img, new_len) == 0, "slow sync left the new image");

    /* FLASH_END is silent, but a later SYNC still works, so readback runs. */
    prep(&fake, old_img, old_len);
    fake.silent_flash_end = 1;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
    expect(rr.wrote == 1 && rr.verify_ran == 1 && rr.verified == 1, "silent FLASH_END recovered into verify");
    expect(rr.restored == 0, "silent FLASH_END did not roll back");
    expect(memcmp(fake.mem + 0x10000, new_img, new_len) == 0, "silent FLASH_END left the new image");

    /* After a finished write the port goes silent. Verify cannot run; do not roll back. */
    prep(&fake, old_img, old_len);
    fake.drop_after_write = 1;
    io = io_of(&fake);
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "drop after write should fail verify");
    expect(rr.wrote == 1, "drop after write already sent the image");
    expect(rr.verify_ran == 0, "drop after write never compared bytes");
    expect(rr.verified == 0, "drop after write did not claim a match");
    expect(rr.restored == 0, "drop after write must not roll back");
    expect(strstr(rr.error, "verify could not run") != NULL, rr.error);
    expect(strstr(rr.error, "write failed") == NULL, "must not say write failed when verify never ran");
    expect(memcmp(fake.mem + 0x10000, new_img, new_len) == 0, "new image left in place after unverifiable write");

    /* First SYNC after reset is silent, then the ROM answers that SYNC with 8 packets. */
    prep(&fake, old_img, old_len);
    fake.classic_reset_required = 1;
    fake.silent_first_sync = 1;
    fake.sync_replies = 8;
    io = io_of(&fake);
    io.reopen = fake_reopen;
    expect(rom_update_app(&io, new_img, new_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
    expect(fake.classic_resets >= 1, "classic reset was used");
    expect(fake.lost_reset_to_reopen == 0, "port was not re-opened between reset and SYNC");
    expect(rr.wrote == 1 && rr.verified == 1, "8 leftover SYNC replies were drained");
    expect(memcmp(fake.mem + 0x10000, new_img, new_len) == 0, "burst-SYNC write left the new image");
    expect(fake.log_lines > 0, "protocol log received lines");

    /* Sanity: re-opening after the pulse, before SYNC, drops download mode. */
    prep(&fake, old_img, old_len);
    fake.classic_reset_required = 1;
    fake.silent_first_sync = 1;
    fake.sync_replies = 8;
    io = io_of(&fake);
    io.reopen = fake_reopen;
    io.set_dtr_rts(io.ctx, 0, 1);
    io.set_dtr_rts(io.ctx, 1, 0);
    io.set_dtr_rts(io.ctx, 0, 0);
    expect(fake.in_download == 1, "classic pulse entered download mode");
    expect(io.reopen(io.ctx) == 0, "reopen hook runs");
    expect(fake.lost_reset_to_reopen == 1, "reopen between reset and SYNC lost download mode");
    expect(fake.in_download == 0, "chip left download mode after a mid-reset reopen");

    /* ===================== v1.2.3 flasher stub tests ===================== */
    {
        static uint8_t big_old[80000], big_new[80000];
        uint32_t bo_len = 0, bn_len = 0;
        rom_probe_result pr;
        uint8_t d[16];
        char hx[33];
        build_big(big_old, sizeof big_old, &bo_len, "v1.8.6-cp7.2", 41000, 1);
        build_big(big_new, sizeof big_new, &bn_len, "v1.9.1-cp7.2", 47000, 2);
        expect(upd_image_length(big_new, bn_len, &walked, err, sizeof err) && walked == bn_len, "big image walks");

        /* Stub happy path: upload, OHAI, CHANGE_BAUD(460800, 115200), READ_FLASH backup with acks,
         * MD5 check, FLASH_DEFL write, MD5 verify, ROM confirm after reset. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        io.reopen = fake_reopen;
        if (getenv("HD2_WIRE_DUMP")) {
            FILE *imgf;
            char path[512];
            g_dump = fopen(getenv("HD2_WIRE_DUMP"), "w");
            snprintf(path, sizeof path, "%s.image", getenv("HD2_WIRE_DUMP"));
            imgf = fopen(path, "wb");
            if (imgf) {
                fwrite(big_new, 1, bn_len, imgf);
                fclose(imgf);
            }
        }
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        if (g_dump) {
            fclose(g_dump);
            g_dump = NULL;
        }
        expect(rr.used_stub == 1 && rr.stub_fallback == 0, "stub path used, no fallback");
        expect(fake.mem_begins == 2 && fake.mem_bytes == 7908u + 144u && fake.mem_bad_chk == 0, "stub uploaded in 2 segments with good checksums");
        expect(fake.baud_new == 460800 && fake.baud_old == 115200, "stub CHANGE_BAUD carries new and old baud");
        expect(fake.baud == 460800, "port at 460800 for the stub");
        expect(fake.spi_set_stub >= 1, "stub got SPI_SET_PARAMS");
        expect(fake.rd_big_calls == 1, "one full READ_FLASH for the backup");
        expect(fake.rd_acks == fake.rd_frames, "every READ_FLASH packet was acked");
        expect(fake.defl_bad == 0, "acks and DEFL sequence were well formed");
        expect(fake.defl_begins == 1 && fake.defl_ends == 1 && fake.defl_blocks >= 3, "one compressed write in several 16 KB blocks");
        expect(fake.data_writes == 0, "no ROM FLASH_DATA on the stub path");
        expect(fake.md5_calls >= 2, "board MD5 before backup and after write");
        expect(memcmp(fake.mem + 0x10000, big_new, bn_len) == 0, "stub write left the new image");
        expect(g_saved_n == bo_len && memcmp(g_saved, big_old, bo_len) == 0, "stub backup bytes");
        expect(strstr(rr.backup_source, "read from the board") != NULL, rr.backup_source);
        expect(rr.verify_ran && rr.verified && rr.confirmed && strcmp(rr.confirmed_version, "v1.9.1-cp7.2") == 0, "stub verify + confirm");
        expect(fake.mem[0x9000] == 0xA5 && fake.saw_erase == 0, "NVS untouched, no chip erase");
        expect(fake.saw_stub_phase && fake.saw_check && fake.saw_backup && fake.saw_write && fake.saw_verify, "UI saw every phase");
        expect(fake.write_done_max == bn_len && fake.verify_done_max == bn_len, "write and verify progress reached the end");
        expect(fake.backup_done_max == bo_len, "backup progress reached the end");
        expect(fake.log_lines < 400, "log stays small (no per-packet lines)");
        expect(rr.ms_backup > 0 && rr.ms_write > 0, "phase timings recorded");
        expect(fake.lost_reset_to_reopen == 0, "no reopen between reset and SYNC");

        /* Same image reinstall: the package is the backup (board MD5 matched). No big READ_FLASH. */
        prep_stub(&fake, big_new, bn_len);
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 1, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.rd_big_calls == 0, "reinstall skipped the backup read");
        expect(strstr(rr.backup_source, "package") != NULL, rr.backup_source);
        expect(g_saved_n == bn_len && memcmp(g_saved, big_new, bn_len) == 0, "reinstall backup bytes are the board's");
        expect(rr.verified == 1 && rr.used_stub == 1, "reinstall verified on the stub");

        /* full_backup forces the read even when the package matches. */
        prep_stub(&fake, big_new, bn_len);
        io = io_of(&fake);
        io.full_backup = 1;
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 1, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.rd_big_calls == 1, "full_backup read the board");

        /* A local copy found by MD5 is used instead of reading. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        io.find_backup = find_backup_cb;
        g_find_src = big_old;
        g_find_len = bo_len;
        g_find_calls = 0;
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(g_find_calls == 1 && fake.rd_big_calls == 0, "local backup copy used");
        expect(strstr(rr.backup_source, "earlier backup") != NULL, rr.backup_source);
        expect(memcmp(g_saved, big_old, bo_len) == 0, "local copy bytes saved as the backup");
        /* A wrong local copy is ignored. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        io.find_backup = find_backup_cb;
        g_find_src = big_new;
        g_find_len = bo_len;
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.rd_big_calls == 1, "mismatched local copy ignored, board read instead");
        g_find_src = NULL;

        /* No OHAI: chip hangs; reconnect with a classic reset and finish on the ROM path. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_no_ohai = 1;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(rr.stub_fallback == 1 && rr.used_stub == 0, "no OHAI fell back to ROM");
        expect(fake.data_writes > 0 && fake.defl_begins == 0, "ROM FLASH_DATA wrote the image");
        expect(memcmp(fake.mem + 0x10000, big_new, bn_len) == 0, "ROM fallback left the new image");
        expect(rr.verified == 1 && rr.confirmed == 1, "ROM fallback verified and confirmed");
        expect(strstr(fake.last_log, "finished") == NULL, "log hook still alive");

        /* ROM rejects MEM_BEGIN (older ROM / secure download): ROM path. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_ok = 0;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(rr.stub_fallback == 1 && rr.used_stub == 0 && rr.verified == 1, "MEM_BEGIN rejection fell back to ROM");

        /* Stub rejects CHANGE_BAUD: ROM path. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_reject_baud = 1;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(rr.stub_fallback == 1 && rr.used_stub == 0 && rr.verified == 1, "baud rejection fell back to ROM");
        expect(fake.baud <= 460800, "baud cap on fallback");

        /* rom_only: never uploads the stub. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        io.rom_only = 1;
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.mem_begins == 0 && rr.used_stub == 0 && rr.stub_fallback == 0, "rom_only skipped the stub");

        /* Bad READ_FLASH digest before writing: nothing written on the stub, whole update reruns on ROM. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_bad_digest = 1;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(rr.stub_fallback == 1 && rr.used_stub == 0 && fake.defl_begins == 0, "digest failure reran on ROM before any write");
        expect(memcmp(fake.mem + 0x10000, big_new, bn_len) == 0 && rr.verified == 1, "ROM rerun wrote and verified");
        expect(memcmp(g_saved, big_old, bo_len) == 0, "ROM rerun backup bytes");

        /* MD5 mismatch after the write: roll back with FLASH_DEFL and verify the rollback by MD5. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_corrupt_once = 1;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "corrupt write fails");
        expect(rr.wrote == 1 && rr.verify_ran == 1 && rr.verified == 0, "MD5 verify ran and caught it");
        expect(rr.restored == 1, rr.error);
        expect(fake.defl_begins == 2, "rollback used FLASH_DEFL");
        expect(memcmp(fake.mem + 0x10000, big_old, bo_len) == 0, "old app restored");
        expect(fake.mem[0x9000] == 0xA5, "NVS after stub rollback");

        /* FLASH_DEFL_DATA rejected mid-write: roll back. */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_fail_defl_seq = 2;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) != 0, "defl failure fails");
        expect(rr.wrote == 1 && rr.restored == 1, rr.error);
        expect(memcmp(fake.mem + 0x10000, big_old, bo_len) == 0, "old app restored after defl failure");
        expect(rr.stub_fallback == 0, "a failure after writing never reruns the update");

        /* Downgrade refused on the stub path too, nothing written. */
        prep_stub(&fake, big_new, bn_len);
        io = io_of(&fake);
        expect(rom_update_app(&io, big_old, bo_len, "v1.8.6-cp7.2", 1, 1, save_backup, NULL, &rr) != 0, "stub downgrade refused");
        expect(rr.wrote == 0 && fake.defl_begins == 0 && rr.stub_fallback == 0, "refusal is not a stub failure");

        /* Dry run uses the stub and writes nothing. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 0, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.defl_begins == 0 && fake.began == 0 && strcmp(rr.device_version, "v1.8.6-cp7.2") == 0, "stub dry run");

        /* Probe: read-only, stub, MD5 of the installed app, then reset. */
        prep_stub(&fake, big_old, bo_len);
        io = io_of(&fake);
        expect(rom_probe(&io, &pr) == 0, pr.error);
        md5_buf(big_old, bo_len, d);
        for (int i = 0; i < 16; i++) snprintf(hx + i * 2, 3, "%02x", d[i]);
        expect(strcmp(pr.app_md5, hx) == 0, "probe MD5 matches the installed app");
        expect(pr.stub == 1 && pr.app_len == bo_len && pr.layout_cp7 == 1, "probe facts");
        expect(strcmp(pr.version, "v1.8.6-cp7.2") == 0, "probe version");
        expect(fake.began == 0 && fake.defl_begins == 0 && fake.data_writes == 0, "probe wrote nothing");
        expect(fake.in_stub == 0 && fake.in_download == 0, "probe reset the board back to the app");
        /* Probe without a stub still works (ROM read). */
        prep_stub(&fake, big_old, bo_len);
        fake.stub_no_ohai = 1;
        io = io_of(&fake);
        expect(rom_probe(&io, &pr) == 0, pr.error);
        expect(pr.stub == 0 && pr.stub_fallback == 1 && strcmp(pr.app_md5, hx) == 0, "probe ROM fallback MD5");

        /* App-only update writes 0x10000 and does not touch assets, NVS, or phy. */
        prep_stub(&fake, big_old, bo_len);
        fake.mem[0x9000] = 0xA5;
        fake.mem[0xF000] = 0x5A;
        fake.mem[0xA10000] = 0x3C;
        io = io_of(&fake);
        expect(rom_update_app(&io, big_new, bn_len, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
        expect(fake.mem[0xA10000] == 0x3C && fake.assets_defl == 0, "app-only update did not write assets");
        expect(fake.mem[0x9000] == 0xA5 && fake.mem[0xF000] == 0x5A && fake.saw_erase == 0, "app-only left NVS and phy");
    }

    /* Factory first-install: full backup first, then bootloader, cp7 table,
     * app, and assets. NVS and phy_init stay. No chip-erase opcode. */
    {
        static uint8_t boot[256], part[0xC00], app[40000], assets_img[4096];
        uint32_t boot_len = 0, app_len = 0;
        const uint32_t backup_len = 0xA20000;
        rom_install_image imgs[4];
        rom_install_result ir;
        uint8_t md_want[16], md_got[16];
        build_image(boot, sizeof boot, &boot_len, "boot-new");
        fill_cp7_table(part);
        build_big(app, sizeof app, &app_len, "v1.9.1-cp7.2", 20000, 9);
        for (int ai = 0; ai < (int)sizeof assets_img; ai++) assets_img[ai] = (uint8_t)(ai * 3 + 1);
        expect(upd_partition_is_cp7(part, sizeof part, err, sizeof err), err);
        expect(0x10000u + app_len < 0xA10000u, "test app stays out of assets");
        expect(backup_len >= 0xA10000u + sizeof assets_img, "backup covers assets");
        imgs[0] = (rom_install_image){0, boot, boot_len, "bootloader"};
        imgs[1] = (rom_install_image){0x8000, part, sizeof part, "partitions"};
        imgs[2] = (rom_install_image){0x10000, app, app_len, "app"};
        imgs[3] = (rom_install_image){0xA10000, assets_img, sizeof assets_img, "assets"};

        imgs[0].offset = 0x9000;
        prep_stock(&fake);
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) != 0, "NVS region refused");
        expect(fake.defl_begins == 0 && fake.began == 0 && fake.saw_erase == 0, "refused region never touched the wire");
        expect(fake.mem[0x9000] == 0xA5, "NVS still the stock byte");
        imgs[0].offset = 0;

        prep_stock(&fake);
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_fail, chip_backup_read, &fake, &ir) != 0, "backup save failure");
        expect(ir.wrote == 0 && fake.defl_begins == 0 && fake.saw_erase == 0, "failed backup writes nothing");
        expect(fake.mem[0x9000] == 0xA5, "NVS after failed backup");

        prep_stock(&fake);
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 0,
                                 chip_backup_write, chip_backup_read, &fake, &ir) == 0, ir.error);
        expect(ir.backed_up == 1 && ir.backup_bytes == backup_len && ir.wrote == 0, "dry run backed up only");
        expect(fake.defl_begins == 0 && fake.data_writes == 0 && fake.saw_erase == 0, "dry run did not program");
        expect(g_chip_backup[0x9000] == 0xA5 && g_backup_during_write == 0, "dry-run backup contains NVS and finished before any program");
        expect(fake.mem[0] == 0xFF && fake.mem[0x9000] == 0xA5, "dry run left the chip");

        prep_stock(&fake);
        io = io_of(&fake);
        io.reopen = fake_reopen;
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) == 0, ir.error);
        expect(ir.used_stub == 1 && ir.stub_fallback == 0 && ir.verified == 1 && ir.confirmed == 1, "stub first-install verified");
        expect(strcmp(ir.confirmed_version, "v1.9.1-cp7.2") == 0, ir.confirmed_version);
        expect(ir.nvs_written == 0 && ir.restored == 0 && ir.wrote == 1, "NVS flag stays clear");
        expect(fake.saw_erase == 0, "no chip erase on first-install");
        expect(g_backup_during_write == 0, "backup completed before the first program");
        expect(g_chip_backup_len == backup_len && g_chip_backup[0x9000] == 0xA5, "backup is the stock flash");
        expect(memcmp(fake.mem, boot, boot_len) == 0, "bootloader landed");
        expect(memcmp(fake.mem + 0x8000, part, sizeof part) == 0, "partition table landed");
        expect(memcmp(fake.mem + 0x10000, app, app_len) == 0, "app landed");
        expect(memcmp(fake.mem + 0xA10000, assets_img, sizeof assets_img) == 0, "assets landed");
        md5_buf(assets_img, sizeof assets_img, md_want);
        md5_buf(fake.mem + 0xA10000, sizeof assets_img, md_got);
        expect(memcmp(md_want, md_got, 16) == 0, "assets MD5 matches the image");
        expect(fake.assets_defl >= 1, "assets write used FLASH_DEFL");
        expect(fake.mem[0x9000] == 0xA5 && fake.mem[0xF000] == 0x5A, "NVS and phy_init untouched");
        expect(fake.saw_backup && fake.saw_write && fake.saw_verify, "install progress phases");
        expect(fake.backup_done_max == backup_len, "backup progress covered the whole read");

        /* A bad bootloader MD5 rolls every started region back. NVS stays. */
        prep_stock(&fake);
        fake.stub_corrupt_once = 1;
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) != 0, "corrupt install fails");
        expect(ir.wrote == 1 && ir.restored == 1 && ir.nvs_written == 0, ir.error);
        expect(fake.mem[0] == 0xFF, "bootloader restored to stock 0xFF");
        expect(fake.mem[0x8000] == 0xFF, "partition table restored");
        expect(fake.mem[0x10000] == 0xFF, "app restored");
        expect(fake.mem[0xA10010] == 0x6E, "assets restored from the backup");
        expect(fake.mem[0x9000] == 0xA5 && fake.mem[0xF000] == 0x5A && fake.saw_erase == 0, "rollback did not touch NVS, phy, or erase the chip");

        /* First data block rejected: bootloader began, then the backup is put back. */
        prep_stock(&fake);
        fake.stub_fail_defl_seq = 1;
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) != 0, "defl failure fails the install");
        expect(ir.wrote == 1 && ir.restored == 1 && fake.mem[0x9000] == 0xA5, ir.error);
        expect(fake.mem[0] == 0xFF && fake.saw_erase == 0, "partial boot write rolled back");

        /* Stub read digest fails before any program: ROM path still installs. */
        prep_stock(&fake);
        fake.stub_bad_digest = 1;
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) == 0, ir.error);
        expect(ir.stub_fallback == 1 && ir.used_stub == 0 && ir.verified == 1 && ir.confirmed == 1, "ROM fallback first-install");
        expect(memcmp(fake.mem + 0x10000, app, app_len) == 0, "ROM fallback wrote the app");
        expect(fake.mem[0x9000] == 0xA5 && fake.saw_erase == 0, "ROM fallback left NVS and did not erase the chip");

        /* No OHAI: the session itself drops to the ROM loader and still installs. */
        prep_stock(&fake);
        fake.stub_no_ohai = 1;
        io = io_of(&fake);
        expect(rom_first_install(&io, imgs, 4, backup_len, "v1.9.1-cp7.2", 1,
                                 chip_backup_write, chip_backup_read, &fake, &ir) == 0, ir.error);
        expect(ir.stub_fallback == 1 && ir.used_stub == 0 && ir.verified == 1, "no OHAI first-install used ROM");
        expect(fake.mem[0x9000] == 0xA5, "NVS after no-OHAI install");

        {
            FILE *pf = fopen("firmware/partitions.bin", "rb");
            FILE *bf = fopen("firmware/bootloader.bin", "rb");
            uint8_t t[0xC00];
            uint8_t *boot = NULL;
            size_t n = 0, bn = 0;
            uint32_t blen = 0;
            expect(pf != NULL, "firmware/partitions.bin is present");
            expect(bf != NULL, "firmware/bootloader.bin is present");
            if (pf) {
                n = fread(t, 1, sizeof t, pf);
                fclose(pf);
            }
            expect(n == sizeof t && upd_partition_is_cp7(t, n, err, sizeof err), err[0] ? err : "shipped partition table");
            if (bf) {
                fseek(bf, 0, SEEK_END);
                bn = (size_t)ftell(bf);
                rewind(bf);
                boot = (uint8_t *)malloc(bn);
                expect(boot && fread(boot, 1, bn, bf) == bn, "bootloader read");
                fclose(bf);
            }
            expect(boot && upd_image_length(boot, bn, &blen, err, sizeof err) && blen == bn && bn <= 0x7000, err[0] ? err : "shipped bootloader walks");
            expect(boot && boot[0] == 0xE9 && boot[12] == 9, "shipped bootloader is ESP32-S3");
            free(boot);
        }
        {
            FILE *af = fopen("firmware/assets_fat_0xa10000.img", "rb");
            uint8_t *aimg = NULL;
            uint8_t md[16];
            char hx[33];
            rom_install_image real_imgs[4];
            const uint32_t full_backup = 0xE10000;
            expect(af != NULL, "firmware/assets_fat_0xa10000.img is present");
            if (af) {
                aimg = (uint8_t *)malloc(0x400000);
                expect(aimg && fread(aimg, 1, 0x400000, af) == 0x400000, "assets image is 4 MiB");
                fclose(af);
            }
            if (aimg) {
                md5_buf(aimg, 0x400000, md);
                for (int i = 0; i < 16; i++) snprintf(hx + i * 2, 3, "%02x", md[i]);
                expect(strcmp(hx, "56ad5746c2f2940c636553dcdcae383e") == 0, "shipped assets MD5");
                real_imgs[0] = imgs[0];
                real_imgs[1] = imgs[1];
                real_imgs[2] = imgs[2];
                real_imgs[3] = (rom_install_image){0xA10000, aimg, 0x400000, "assets"};
                prep_stock(&fake);
                io = io_of(&fake);
                expect(rom_first_install(&io, real_imgs, 4, full_backup, "v1.9.1-cp7.2", 1,
                                         chip_backup_write, chip_backup_read, &fake, &ir) == 0, ir.error);
                expect(fake.saw_erase == 0 && ir.nvs_written == 0, "real assets install did not erase the chip");
                expect(fake.mem[0x9000] == 0xA5 && fake.mem[0xF000] == 0x5A, "real assets install left NVS and phy");
                expect(memcmp(fake.mem + 0xA10000, aimg, 0x400000) == 0, "real assets image is byte-identical on the mock chip");
                expect(ir.used_stub == 1 && ir.verified == 1 && fake.assets_defl >= 1, "real assets used compressed stub writes");
                expect(g_backup_during_write == 0 && ir.backed_up == 1, "backup was saved before the assets write");
            }
            free(aimg);
        }
    }

    /* Optional: the real 2.4 MB release image through the stub path (build with -DTEST_MEM=0x400000
     * and set HD2_REAL_IMAGE=<path to the .bin>). Upgrade from a small old app, then reinstall. */
    if (getenv("HD2_REAL_IMAGE")) {
        static uint8_t real[0x300000], small_old[80000];
        uint32_t rl = 0, so_len = 0, w2 = 0;
        FILE *rf = fopen(getenv("HD2_REAL_IMAGE"), "rb");
        expect(rf != NULL, "real image opens");
        if (rf) {
            rl = (uint32_t)fread(real, 1, sizeof real, rf);
            fclose(rf);
        }
        if (rf && 0x10000u + rl + 0x1000u > (uint32_t)MEM) {
            fprintf(stderr, "skip real image: rebuild with -DTEST_MEM=0x400000\n");
        } else if (rf) {
            expect(upd_image_length(real, rl, &w2, err, sizeof err) && w2 == rl, "real image walks");
            build_big(small_old, sizeof small_old, &so_len, "v1.8.6-cp7.2", 41000, 3);
            prep_stub(&fake, small_old, so_len);
            io = io_of(&fake);
            io.reopen = fake_reopen;
            expect(rom_update_app(&io, real, rl, "v1.9.1-cp7.2", 0, 1, save_backup, NULL, &rr) == 0, rr.error);
            expect(rr.used_stub == 1 && rr.verified == 1 && rr.restored == 0, "real image stub upgrade verified");
            expect(memcmp(fake.mem + 0x10000, real, rl) == 0, "real image landed byte-exact");
            fprintf(stderr, "real image upgrade: %u DEFL blocks, %u acks/%u frames\n", fake.defl_blocks, fake.rd_acks, fake.rd_frames);
            prep_stub(&fake, real, rl);
            io = io_of(&fake);
            io.reopen = fake_reopen;
            expect(rom_update_app(&io, real, rl, "v1.9.1-cp7.2", 1, 1, save_backup, NULL, &rr) == 0, rr.error);
            expect(rr.used_stub == 1 && rr.verified == 1 && fake.rd_big_calls == 0, "real image reinstall: backup from package, no full read");
            fprintf(stderr, "real image reinstall: backup=%s\n", rr.backup_source);
        }
    }

    if (g_fail) {
        fprintf(stderr, "%d romflash failures\n", g_fail);
        return 1;
    }
    puts("test_romflash ok");
    return 0;
}
