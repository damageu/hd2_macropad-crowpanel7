#include "romflash.h"
#include "update_core.h"
#include "md5.h"
#include "stub_esp32s3.h"
#include "third_party/miniz/miniz.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

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
#define ROM_READ_CHUNK 64
/* v1.2.3 flasher stub (esptool loader.py / cmds.py, esptool 5.2.0) */
#define ROM_MEM_BEGIN 0x05
#define ROM_MEM_END 0x06
#define ROM_MEM_DATA 0x07
#define STUB_DEFL_BEGIN 0x10
#define STUB_DEFL_DATA 0x11
#define STUB_DEFL_END 0x12
#define STUB_MD5 0x13
#define STUB_READ_FLASH 0xD2
#define STUB_RAM_BLOCK 0x1800    /* ESPLoader.ESP_RAM_BLOCK */
#define STUB_WRITE_SIZE 0x4000   /* StubMixin.FLASH_WRITE_SIZE */
#define STUB_SECTOR 0x1000       /* FLASH_SECTOR_SIZE, READ_FLASH packet size */
#define STUB_READ_INFLIGHT 64    /* esptool read_flash max in-flight packets */
#define STUB_TIMEOUT_MS 3000     /* DEFAULT_TIMEOUT */
#define LOG_EVERY 0x10000u       /* bulk progress summary in the log, every 64 KB */

/* Session state for one rom_update_app call. Link runs one update at a time. */
static int s_stub;          /* 1 while the flasher stub is answering */
static int s_stub_disabled; /* 1 after a stub failure: ROM-only for the rest of the run */
static int s_stub_io_fail;  /* 1 if a stub command failed before anything was written */
static int s_quiet;         /* 1 inside bulk loops: no per-packet trace lines */
static int s_layout_write;  /* 1 only inside rom_first_install: bootloader + table + app */

static void fail(rom_result *r, const char *msg)
{
    if (r) snprintf(r->error, sizeof r->error, "%s", msg);
}

static const char *op_name(uint8_t op)
{
    switch (op) {
    case ROM_SYNC: return "SYNC";
    case ROM_READ_REG: return "READ_REG";
    case ROM_FLASH_BEGIN: return "FLASH_BEGIN";
    case ROM_FLASH_DATA: return "FLASH_DATA";
    case ROM_FLASH_END: return "FLASH_END";
    case ROM_SPI_SET: return "SPI_SET";
    case ROM_SPI_ATTACH: return "SPI_ATTACH";
    case ROM_READ_SLOW: return "READ_FLASH_SLOW";
    case ROM_CHANGE_BAUD: return "CHANGE_BAUD";
    case ROM_MEM_BEGIN: return "MEM_BEGIN";
    case ROM_MEM_END: return "MEM_END";
    case ROM_MEM_DATA: return "MEM_DATA";
    case STUB_DEFL_BEGIN: return "FLASH_DEFL_BEGIN";
    case STUB_DEFL_DATA: return "FLASH_DEFL_DATA";
    case STUB_DEFL_END: return "FLASH_DEFL_END";
    case STUB_MD5: return "SPI_FLASH_MD5";
    case STUB_READ_FLASH: return "READ_FLASH";
    default: return "CMD";
    }
}

static void rom_log(rom_io *io, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!io || !io->log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    io->log(io->ctx, buf);
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
    uint8_t sbuf[2400];
    uint8_t *buf = sbuf;
    size_t cap = sizeof sbuf;
    size_t o = 0;
    int rc;
    if (n > 16u + STUB_WRITE_SIZE + 64u) return -1;
    if (2 * n + 4 > cap) {
        cap = 2 * n + 4;
        buf = (uint8_t *)malloc(cap);
        if (!buf) return -1;
    }
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
        if (o + 2 >= cap) {
            if (buf != sbuf) free(buf);
            return -1;
        }
    }
    buf[o++] = 0xC0;
    rc = io->write(io->ctx, buf, o);
    if (buf != sbuf) free(buf);
    return rc;
}

static int slip_read(rom_io *io, uint8_t *out, size_t cap, int timeout_ms)
{
    int started = 0;
    size_t n = 0;
    int esc = 0;
    size_t limit = cap * 2 + 64;
    if (limit < 8000) limit = 8000;
    for (size_t spins = 0; spins < limit; spins++) {
        uint8_t b;
        int got = io->read(io->ctx, &b, 1, started ? 300 : timeout_ms);
        if (got < 0) return -1;
        if (got == 0) return 0; /* timeout: no frame, do not keep spinning */
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
    return 0;
}

/* timeout_attempts is how many idle timeouts to allow. Leftover frames for a
 * different opcode are drained and do not consume a timeout. */
/* Request header <BBHI: 0x00, op, len, checksum> + data (esptool ESPLoader.command). */
static int send_cmd(rom_io *io, uint8_t op, const uint8_t *data, uint16_t dlen, uint32_t chk)
{
    uint8_t spkt[1200];
    uint8_t *pkt = spkt;
    int rc;
    if ((size_t)dlen + 8 > sizeof spkt) {
        pkt = (uint8_t *)malloc((size_t)dlen + 8);
        if (!pkt) return -1;
    }
    pkt[0] = 0x00;
    pkt[1] = op;
    pkt[2] = (uint8_t)dlen;
    pkt[3] = (uint8_t)(dlen >> 8);
    put_u32(pkt + 4, chk);
    if (dlen) memcpy(pkt + 8, data, dlen);
    rc = slip_write(io, pkt, 8u + dlen);
    if (pkt != spkt) free(pkt);
    return rc;
}

static int rom_cmd(rom_io *io, uint8_t op, const uint8_t *data, uint16_t dlen, uint32_t chk,
                   uint32_t *val_out, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                   int timeout_ms, int timeout_attempts, char *err, size_t err_cap)
{
    int skips = 0, timeouts = 0;
    if (op == ROM_ERASE_FLASH || op == ROM_ERASE_REGION) {
        snprintf(err, err_cap, "internal error: chip erase is not implemented");
        return -1;
    }
    if (timeout_attempts < 1) timeout_attempts = 1;
    if (timeout_attempts > 4) timeout_attempts = 4;
    if (!s_quiet) rom_log(io, "send %s (0x%02X) timeout=%d ms", op_name(op), op, timeout_ms);
    if (send_cmd(io, op, data, dlen, chk) != 0) {
        snprintf(err, err_cap, "the serial port stopped accepting data");
        rom_log(io, "write failed for %s", op_name(op));
        return -1;
    }
    while (timeouts < timeout_attempts && skips < 24) {
        uint8_t frame[1400];
        int n = slip_read(io, frame, sizeof frame, timeout_ms);
        if (n == 0) {
            timeouts++;
            rom_log(io, "no serial data for %s (timeout %d/%d)", op_name(op), timeouts, timeout_attempts);
            continue;
        }
        if (n < 0) {
            snprintf(err, err_cap, "the serial port closed");
            rom_log(io, "port closed while waiting for %s", op_name(op));
            return -1;
        }
        if (n < 8 || frame[0] != 0x01) {
            skips++;
            rom_log(io, "skipped short or non-response frame (%d bytes)", n);
            continue;
        }
        if (frame[1] != op) {
            skips++;
            rom_log(io, "drained leftover reply op=0x%02X (%s), want %s", frame[1], op_name(frame[1]), op_name(op));
            continue;
        }
        uint16_t len = (uint16_t)(frame[2] | (frame[3] << 8));
        if ((size_t)n < 8u + len || len < ROM_STATUS) {
            snprintf(err, err_cap, "bootloader reply was too short");
            rom_log(io, "%s reply too short (%d bytes, len=%u)", op_name(op), n, len);
            return -1;
        }
        const uint8_t *st = frame + 8 + len - ROM_STATUS;
        if (st[0] != 0) {
            snprintf(err, err_cap, "bootloader rejected a command (status %u)", st[0]);
            rom_log(io, "%s rejected, status %u", op_name(op), st[0]);
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
        if (!s_quiet)
            rom_log(io, "%s ok, val=0x%08X, body=%u", op_name(op), val_out ? *val_out : get_u32(frame + 4), (unsigned)body);
        return 0;
    }
    snprintf(err, err_cap, "no bootloader reply");
    rom_log(io, "%s failed: no matching reply (timeouts=%d leftover_drains=%d)", op_name(op), timeouts, skips);
    return -1;
}

/* Drain the extra SYNC replies the ROM sends after a successful SYNC (typically 8 total). */
static void drain_sync_replies(rom_io *io)
{
    int got = 0;
    for (int i = 0; i < 7; i++) {
        uint8_t frame[1400];
        int n = slip_read(io, frame, sizeof frame, 100);
        if (n <= 0) break;
        got++;
        if (n >= 2)
            rom_log(io, "drained extra SYNC reply %d/7, op=0x%02X, %d bytes", got, frame[1], n);
        else
            rom_log(io, "drained extra SYNC reply %d/7, %d bytes", got, n);
    }
    if (got)
        rom_log(io, "drained %d extra SYNC replies", got);
}

/* esptool 5 ClassicReset: D0|R1|W0.1|D1|R0|W(reset_delay)|D0. No wait after the last DTR,
 * and the port is not re-opened between this pulse and the following SYNC. */
static void classic_reset(rom_io *io, unsigned reset_delay_ms)
{
    rom_log(io, "classic reset: DTR=0 RTS=1 (EN low), wait 100 ms");
    io->set_dtr_rts(io->ctx, 0, 1);
    io->sleep_ms(io->ctx, 100);
    rom_log(io, "classic reset: DTR=1 RTS=0 (out of reset, IO0 held), wait %u ms", reset_delay_ms);
    io->set_dtr_rts(io->ctx, 1, 0);
    io->sleep_ms(io->ctx, reset_delay_ms);
    rom_log(io, "classic reset: DTR=0 RTS=0 (done)");
    io->set_dtr_rts(io->ctx, 0, 0);
    io->flush_rx(io->ctx);
}

static void hard_reset(rom_io *io)
{
    s_stub = 0; /* the stub lives in RAM; a reset ends it */
    rom_log(io, "hard reset via RTS");
    io->set_dtr_rts(io->ctx, 0, 1);
    io->sleep_ms(io->ctx, 100);
    io->set_dtr_rts(io->ctx, 0, 0);
    io->sleep_ms(io->ctx, 200);
}

static int reopen_port(rom_io *io, char *err, size_t err_cap)
{
    if (!io->reopen) return 0;
    rom_log(io, "re-opening the serial port");
    io->sleep_ms(io->ctx, 400);
    if (io->reopen(io->ctx) != 0) {
        snprintf(err, err_cap, "could not re-open the serial port after a reset");
        rom_log(io, "re-open failed");
        return -1;
    }
    return 0;
}

static int sync_once(rom_io *io, char *err, size_t err_cap)
{
    uint8_t body[36];
    uint32_t val = 0;
    body[0] = 0x07;
    body[1] = 0x07;
    body[2] = 0x12;
    body[3] = 0x20;
    memset(body + 4, 0x55, 32);
    io->flush_rx(io->ctx);
    if (rom_cmd(io, ROM_SYNC, body, 36, 0, &val, NULL, 0, NULL, 100, 1, err, err_cap) != 0)
        return -1;
    drain_sync_replies(io);
    return 0;
}

/* esptool _connect_attempt: up to 5 SYNC tries at 100 ms, 50 ms apart. No reset here. */
static int wait_bootloader(rom_io *io, char *err, size_t err_cap)
{
    for (int i = 1; i <= 5; i++) {
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled while waiting for the bootloader");
            return -1;
        }
        rom_log(io, "SYNC attempt %d of 5", i);
        if (io->progress) io->progress(io->ctx, "sync", (unsigned)i, 5);
        if (sync_once(io, err, err_cap) == 0) {
            rom_log(io, "SYNC succeeded on attempt %d", i);
            return 0;
        }
        io->sleep_ms(io->ctx, 50);
    }
    snprintf(err, err_cap, "the chip did not enter the bootloader");
    rom_log(io, "SYNC failed after 5 attempts: %s", err);
    return -1;
}

/* esptool connect on Windows: cycle ClassicReset(50 ms) and ClassicReset(550 ms), 7 tries.
 * Never re-open the port between a reset and SYNC. */
static int connect_bootloader(rom_io *io, char *err, size_t err_cap)
{
    static const unsigned delays[2] = {50, 550};
    for (int attempt = 1; attempt <= 7; attempt++) {
        unsigned delay = delays[(attempt - 1) % 2];
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled while waiting for the bootloader");
            return -1;
        }
        if (io->progress) io->progress(io->ctx, "sync", (unsigned)attempt, 7);
        rom_log(io, "connect attempt %d of 7, reset_delay=%u ms", attempt, delay);
        classic_reset(io, delay);
        if (wait_bootloader(io, err, err_cap) == 0) {
            rom_log(io, "bootloader ready on connect attempt %d", attempt);
            return 0;
        }
    }
    snprintf(err, err_cap, "the chip did not enter the bootloader");
    rom_log(io, "connect failed after 7 reset/sync cycles");
    return -1;
}

/* ---------------------------------------------------------------------------
 * v1.2.3 flasher stub support. Wire format follows esptool 5.2.0:
 * loader.py run_stub/mem_begin/mem_block/mem_finish/change_baud/read_flash/
 * flash_md5sum/flash_defl_*, cmds.py write_flash. Stub replies carry their
 * status in the two bytes after the expected response data (check_command).
 * ------------------------------------------------------------------------- */

static unsigned now_ms(rom_io *io)
{
    return io->now_ms ? io->now_ms(io->ctx) : 0;
}

typedef struct {
    const char *name;
    unsigned t0;
} phase_t;

static void phase_start(rom_io *io, phase_t *p, const char *name, uint32_t total)
{
    p->name = name;
    p->t0 = now_ms(io);
    rom_log(io, "phase %s started: %u bytes", name, (unsigned)total);
}

static void phase_end(rom_io *io, phase_t *p, uint32_t bytes, const char *note)
{
    unsigned dt = now_ms(io) - p->t0;
    unsigned kbs = dt ? (unsigned)(((uint64_t)bytes * 1000u) / ((uint64_t)dt * 1024u)) : 0;
    rom_log(io, "phase %s done: %u bytes in %u.%03u s (%u KB/s)%s%s", p->name, (unsigned)bytes,
            dt / 1000u, dt % 1000u, kbs, note ? ", " : "", note ? note : "");
}

static void hex16(const uint8_t *d, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[32] = 0;
}

static int stub_cmd(rom_io *io, uint8_t op, const uint8_t *data, uint16_t dlen, uint32_t chk,
                    uint32_t *val_out, uint8_t *resp, size_t resp_len, int timeout_ms,
                    char *err, size_t err_cap)
{
    int skips = 0;
    if (!s_quiet) rom_log(io, "stub send %s (0x%02X) timeout=%d ms", op_name(op), op, timeout_ms);
    if (send_cmd(io, op, data, dlen, chk) != 0) {
        snprintf(err, err_cap, "the serial port stopped accepting data");
        rom_log(io, "write failed for %s", op_name(op));
        s_stub_io_fail = 1;
        return -1;
    }
    while (skips < 16) {
        uint8_t frame[STUB_SECTOR + 64];
        uint16_t len;
        const uint8_t *st;
        int n = slip_read(io, frame, sizeof frame, timeout_ms);
        if (n == 0) {
            snprintf(err, err_cap, "no reply from the flasher stub to %s", op_name(op));
            rom_log(io, "stub: no reply to %s within %d ms", op_name(op), timeout_ms);
            s_stub_io_fail = 1;
            return -1;
        }
        if (n < 0) {
            snprintf(err, err_cap, "the serial port closed");
            rom_log(io, "port closed while waiting for stub %s", op_name(op));
            s_stub_io_fail = 1;
            return -1;
        }
        if (n < 8 || frame[0] != 0x01 || frame[1] != op) {
            skips++;
            rom_log(io, "stub: skipped frame (%d bytes, op 0x%02X) while waiting for %s", n,
                    n >= 2 ? frame[1] : 0, op_name(op));
            continue;
        }
        len = (uint16_t)(frame[2] | (frame[3] << 8));
        if ((size_t)n < 8u + len || len < resp_len + 2u) {
            if ((size_t)n >= 10 && len >= 2 && frame[8] != 0)
                snprintf(err, err_cap, "the flasher stub rejected %s (status 0x%02X%02X)", op_name(op), frame[8], frame[9]);
            else
                snprintf(err, err_cap, "flasher stub reply to %s was too short", op_name(op));
            rom_log(io, "stub %s: %s (len=%u, %d bytes)", op_name(op), err, len, n);
            s_stub_io_fail = 1;
            return -1;
        }
        st = frame + 8 + resp_len;
        if (st[0] != 0) {
            snprintf(err, err_cap, "the flasher stub rejected %s (status 0x%02X%02X)", op_name(op), st[0], st[1]);
            rom_log(io, "stub %s rejected, status 0x%02X%02X", op_name(op), st[0], st[1]);
            s_stub_io_fail = 1;
            return -1;
        }
        if (val_out) *val_out = get_u32(frame + 4);
        if (resp && resp_len) memcpy(resp, frame + 8, resp_len);
        if (!s_quiet) rom_log(io, "stub %s ok, val=0x%08X", op_name(op), get_u32(frame + 4));
        return 0;
    }
    snprintf(err, err_cap, "flasher stub replies did not match %s", op_name(op));
    s_stub_io_fail = 1;
    return -1;
}

/* esptool run_stub: MEM_BEGIN/MEM_DATA per segment at the ROM baud, MEM_END with
 * the entry point, then the stub sends a raw SLIP frame "OHAI". */
static int stub_upload(rom_io *io, char *err, size_t err_cap)
{
    struct {
        const uint8_t *p;
        uint32_t n;
        uint32_t addr;
        const char *name;
    } seg[2] = {
        {stub_s3_text, (uint32_t)sizeof stub_s3_text, STUB_S3_TEXT_START, "text"},
        {stub_s3_data, (uint32_t)sizeof stub_s3_data, STUB_S3_DATA_START, "data"},
    };
    uint8_t *pkt = (uint8_t *)malloc(16 + STUB_RAM_BLOCK);
    uint8_t req[16];
    if (!pkt) {
        snprintf(err, err_cap, "not enough memory for the flasher stub");
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        uint32_t blocks = (seg[i].n + STUB_RAM_BLOCK - 1) / STUB_RAM_BLOCK;
        put_u32(req, seg[i].n);
        put_u32(req + 4, blocks);
        put_u32(req + 8, STUB_RAM_BLOCK);
        put_u32(req + 12, seg[i].addr);
        rom_log(io, "stub upload: %s %u bytes at 0x%08X in %u blocks", seg[i].name, (unsigned)seg[i].n,
                (unsigned)seg[i].addr, (unsigned)blocks);
        if (rom_cmd(io, ROM_MEM_BEGIN, req, 16, 0, NULL, NULL, 0, NULL, STUB_TIMEOUT_MS, 1, err, err_cap) != 0) {
            free(pkt);
            return -1;
        }
        for (uint32_t seq = 0; seq < blocks; seq++) {
            uint32_t off = seq * STUB_RAM_BLOCK;
            uint32_t n = seg[i].n - off;
            if (n > STUB_RAM_BLOCK) n = STUB_RAM_BLOCK;
            put_u32(pkt, n);
            put_u32(pkt + 4, seq);
            put_u32(pkt + 8, 0);
            put_u32(pkt + 12, 0);
            memcpy(pkt + 16, seg[i].p + off, n);
            if (rom_cmd(io, ROM_MEM_DATA, pkt, (uint16_t)(16 + n), xor_sum(seg[i].p + off, n), NULL, NULL, 0, NULL,
                        STUB_TIMEOUT_MS, 1, err, err_cap) != 0) {
                free(pkt);
                return -1;
            }
        }
    }
    free(pkt);
    /* mem_finish: <II int(entry == 0), entry>. The ROM reply may be lost when
     * the stub takes over the UART, so it is read but not required. */
    put_u32(req, 0);
    put_u32(req + 4, STUB_S3_ENTRY);
    rom_log(io, "send MEM_END, entry 0x%08X; waiting for OHAI", (unsigned)STUB_S3_ENTRY);
    if (send_cmd(io, ROM_MEM_END, req, 8, 0) != 0) {
        snprintf(err, err_cap, "the serial port stopped accepting data");
        return -1;
    }
    for (int frames = 0; frames < 12; frames++) {
        uint8_t frame[256];
        int n = slip_read(io, frame, sizeof frame, STUB_TIMEOUT_MS);
        if (n == 0) break;
        if (n < 0) {
            snprintf(err, err_cap, "the serial port closed while starting the flasher stub");
            return -1;
        }
        if (n == 4 && memcmp(frame, "OHAI", 4) == 0) return 0;
        if (n >= 8 && frame[0] == 0x01 && frame[1] == ROM_MEM_END) {
            uint16_t len = (uint16_t)(frame[2] | (frame[3] << 8));
            if (len >= ROM_STATUS && (size_t)n >= 8u + len && frame[8 + len - ROM_STATUS] != 0) {
                snprintf(err, err_cap, "the ROM refused to start the flasher stub (status %u)", frame[8 + len - ROM_STATUS]);
                rom_log(io, "MEM_END rejected, status %u", frame[8 + len - ROM_STATUS]);
                return -1;
            }
            rom_log(io, "MEM_END ok");
            continue;
        }
        rom_log(io, "stub start: skipped frame (%d bytes)", n);
    }
    snprintf(err, err_cap, "the flasher stub did not send its OHAI greeting");
    rom_log(io, "no OHAI from the flasher stub");
    return -1;
}

static int stub_spi_set(rom_io *io, char *err, size_t err_cap)
{
    uint8_t params[24];
    memset(params, 0, sizeof params);
    put_u32(params + 4, 16u * 1024u * 1024u);
    put_u32(params + 8, 64u * 1024u);
    put_u32(params + 12, 4096);
    put_u32(params + 16, 256);
    put_u32(params + 20, 0xFFFF);
    return stub_cmd(io, ROM_SPI_SET, params, 24, 0, NULL, NULL, 0, STUB_TIMEOUT_MS, err, err_cap);
}

/* Upload, greet, raise the baud (stub CHANGE_BAUD takes new and old baud), probe. */
static int stub_start(rom_io *io, char *err, size_t err_cap)
{
    unsigned t0 = now_ms(io);
    uint8_t req[8];
    uint32_t chip = 0;
    if (io->progress) io->progress(io->ctx, "stub", 0, 1);
    rom_log(io, "loading flasher stub: esp-flasher-stub v0.2.0 (esptool 5.2.0 stub_flasher/2/esp32s3.json, sha256 %s)",
            STUB_S3_JSON_SHA256);
    if (stub_upload(io, err, err_cap) != 0) return -1;
    s_stub = 1;
    rom_log(io, "flasher stub running (OHAI) %u ms after upload start", now_ms(io) - t0);
    put_u32(req, UPD_FLASH_BAUD_MAX);
    put_u32(req + 4, UPD_LINK_BAUD);
    if (stub_cmd(io, ROM_CHANGE_BAUD, req, 8, 0, NULL, NULL, 0, STUB_TIMEOUT_MS, err, err_cap) != 0) return -1;
    if (io->set_baud(io->ctx, UPD_FLASH_BAUD_MAX) != 0) {
        snprintf(err, err_cap, "the PC serial port rejected the flash baud");
        return -1;
    }
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
    if (stub_cmd(io, ROM_READ_REG, (const uint8_t *)"\x00\x10\x00\x40", 4, 0, &chip, NULL, 0, 1000, err, err_cap) != 0)
        return -1;
    if (chip != UPD_CHIP_ID_S3) {
        snprintf(err, err_cap, "flasher stub probe read chip id %u at the new baud", (unsigned)chip);
        return -1;
    }
    rom_log(io, "flasher stub answering at %u baud", (unsigned)UPD_FLASH_BAUD_MAX);
    if (stub_spi_set(io, err, err_cap) != 0) return -1;
    if (io->progress) io->progress(io->ctx, "stub", 1, 1);
    rom_log(io, "flasher stub ready %u ms after upload start", now_ms(io) - t0);
    return 0;
}

/* esptool read_flash with the stub: READ_FLASH <offset, length, 0x1000, 64>, then
 * SLIP data frames of up to 4096 bytes, each acked with <I total received>,
 * then a 16-byte MD5 of everything sent. */
static int stub_read(rom_io *io, uint32_t addr, uint8_t *dst, uint32_t len, const char *phase,
                     char *err, size_t err_cap)
{
    uint8_t req[16], digest[16], mine[16];
    uint8_t *frame;
    uint32_t got = 0, next_log = LOG_EVERY;
    md5_ctx mc;
    int q = s_quiet;
    int rc = -1;
    put_u32(req, addr);
    put_u32(req + 4, len);
    put_u32(req + 8, STUB_SECTOR);
    put_u32(req + 12, STUB_READ_INFLIGHT);
    if (io->progress && phase) io->progress(io->ctx, phase, 0, len);
    if (len >= LOG_EVERY) rom_log(io, "stub READ_FLASH 0x%06X, %u bytes", (unsigned)addr, (unsigned)len);
    s_quiet = 1;
    frame = (uint8_t *)malloc(STUB_SECTOR + 16);
    if (!frame) {
        snprintf(err, err_cap, "not enough memory for a flash read");
        goto out;
    }
    if (stub_cmd(io, STUB_READ_FLASH, req, 16, 0, NULL, NULL, 0, STUB_TIMEOUT_MS, err, err_cap) != 0) goto out;
    md5_init(&mc);
    while (got < len) {
        uint8_t ack[4];
        int n;
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled during a flash read");
            goto out;
        }
        n = slip_read(io, frame, STUB_SECTOR, STUB_TIMEOUT_MS);
        if (n <= 0) {
            snprintf(err, err_cap, "the flasher stub stopped sending flash data at %u of %u bytes",
                     (unsigned)got, (unsigned)len);
            s_stub_io_fail = 1;
            goto out;
        }
        if ((uint32_t)n > len - got || (got + (uint32_t)n < len && n < STUB_SECTOR)) {
            snprintf(err, err_cap, "corrupt flash data block (%d bytes at %u of %u)", n, (unsigned)got, (unsigned)len);
            s_stub_io_fail = 1;
            goto out;
        }
        memcpy(dst + got, frame, (size_t)n);
        md5_update(&mc, frame, (size_t)n);
        got += (uint32_t)n;
        put_u32(ack, got);
        if (slip_write(io, ack, 4) != 0) {
            snprintf(err, err_cap, "the serial port stopped accepting data");
            s_stub_io_fail = 1;
            goto out;
        }
        if (io->progress && phase) io->progress(io->ctx, phase, got, len);
        if (len >= LOG_EVERY && (got >= next_log || got == len)) {
            rom_log(io, "READ_FLASH %u / %u bytes", (unsigned)got, (unsigned)len);
            while (next_log <= got) next_log += LOG_EVERY;
        }
    }
    {
        int n = slip_read(io, frame, STUB_SECTOR, STUB_TIMEOUT_MS);
        if (n != 16) {
            snprintf(err, err_cap, "the flasher stub did not send the read digest");
            s_stub_io_fail = 1;
            goto out;
        }
        memcpy(digest, frame, 16);
    }
    md5_final(&mc, mine);
    if (memcmp(mine, digest, 16) != 0) {
        snprintf(err, err_cap, "flash read digest did not match");
        s_stub_io_fail = 1;
        goto out;
    }
    rc = 0;
out:
    s_quiet = q;
    free(frame);
    if (rc != 0) rom_log(io, "stub read 0x%06X+%u failed: %s", (unsigned)addr, (unsigned)len, err);
    return rc;
}

static uint32_t timeout_per_mb(uint32_t ms_per_mb, uint32_t size)
{
    uint64_t t = ((uint64_t)ms_per_mb * size) / (1024u * 1024u);
    return t < STUB_TIMEOUT_MS ? STUB_TIMEOUT_MS : (uint32_t)t;
}

/* SPI_FLASH_MD5 <addr, size, 0, 0>. The stub returns 16 raw digest bytes. */
static int stub_md5(rom_io *io, uint32_t addr, uint32_t len, uint8_t out[16], char *err, size_t err_cap)
{
    uint8_t req[16];
    put_u32(req, addr);
    put_u32(req + 4, len);
    put_u32(req + 8, 0);
    put_u32(req + 12, 0);
    return stub_cmd(io, STUB_MD5, req, 16, 0, NULL, out, 16, (int)timeout_per_mb(8000, len), err, err_cap);
}

static int ranges_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    if (an == 0 || bn == 0) return 0;
    if (a + an < a || b + bn < b) return 1;
    return a < b + bn && b < a + an;
}

/* App-only updates may program only the factory slot. A first-install may
 * also program the bootloader and the partition table. NVS, phy_init, and
 * the assets partition are never a legal target. Erase is the 4 KB round-up
 * the loader will actually clear. */
static int span_ok(uint32_t offset, uint32_t len, char *err, size_t err_cap)
{
    uint32_t erase = (len + 4095u) & ~4095u;
    if (len == 0 || offset + len < offset || offset + erase < offset) {
        snprintf(err, err_cap, "refusing an empty or wrapping write");
        return 0;
    }
    if (!s_layout_write) {
        if (!upd_range_is_app_slot(offset, len) || offset + erase > UPD_APP_SLOT_END) {
            snprintf(err, err_cap, "refusing a write outside the factory app slot");
            return 0;
        }
        return 1;
    }
    if (ranges_overlap(offset, erase, UPD_NVS_OFFSET, UPD_NVS_SIZE) ||
        ranges_overlap(offset, erase, 0xF000u, 0x1000u) ||
        ranges_overlap(offset, erase, UPD_ASSETS_OFFSET, UPD_ASSETS_SIZE)) {
        snprintf(err, err_cap, "refusing a write over NVS, phy_init, or the assets partition");
        return 0;
    }
    if (offset == 0 && erase <= UPD_PART_TABLE) return 1;
    if (offset == UPD_PART_TABLE && len <= UPD_PART_TABLE_LEN && offset + erase <= UPD_NVS_OFFSET) return 1;
    if (upd_range_is_app_slot(offset, len) && offset + erase <= UPD_APP_SLOT_END) return 1;
    snprintf(err, err_cap, "refusing a write outside the bootloader, partition table, and factory app");
    return 0;
}

/* esptool write_flash with compression: zlib level 9, FLASH_DEFL_BEGIN
 * <uncompressed size, blocks, 0x4000, offset, 0>, FLASH_DEFL_DATA blocks, then
 * FLASH_DEFL_END <1> (stay in the loader). The stub acks a block when it is
 * received, so the next block's timeout covers writing the previous one. */
static int stub_flash_defl(rom_io *io, uint32_t offset, const uint8_t *data, uint32_t len,
                           char *err, size_t err_cap, const char *phase, int *wrote)
{
    size_t clen = 0;
    uint8_t *comp = NULL, *check = NULL, *pkt = NULL;
    uint32_t *unc = NULL;
    uint32_t nblocks, sent = 0, next_log = LOG_EVERY, timeout = STUB_TIMEOUT_MS;
    uint8_t req[20];
    int rc = -1, q = s_quiet;
    phase_t ph;
    if (!span_ok(offset, len, err, err_cap)) return -1;
    comp = (uint8_t *)tdefl_compress_mem_to_heap(data, len, &clen,
                                                 (int)tdefl_create_comp_flags_from_zip_params(9, 15, 0));
    if (!comp || clen == 0) {
        snprintf(err, err_cap, "could not compress the image; nothing was written");
        goto out;
    }
    nblocks = (uint32_t)((clen + STUB_WRITE_SIZE - 1) / STUB_WRITE_SIZE);
    unc = (uint32_t *)calloc(nblocks, sizeof *unc);
    check = (uint8_t *)malloc(len);
    pkt = (uint8_t *)malloc(16 + STUB_WRITE_SIZE);
    if (!unc || !check || !pkt) {
        snprintf(err, err_cap, "not enough memory to prepare the write");
        goto out;
    }
    /* Inflate it back block by block, as the stub will, before touching flash.
     * This also gives each block's uncompressed size for its write timeout. */
    {
        tinfl_decompressor d;
        size_t in_pos = 0, out_pos = 0;
        int done = 0;
        tinfl_init(&d);
        for (uint32_t b = 0; b < nblocks; b++) {
            size_t bend = (size_t)(b + 1) * STUB_WRITE_SIZE;
            size_t before = out_pos;
            if (bend > clen) bend = clen;
            while (in_pos < bend && !done) {
                size_t in_bytes = bend - in_pos, out_bytes = len - out_pos;
                mz_uint32 fl = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 |
                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (bend < clen ? TINFL_FLAG_HAS_MORE_INPUT : 0);
                tinfl_status st = tinfl_decompress(&d, comp + in_pos, &in_bytes, check, check + out_pos, &out_bytes, fl);
                in_pos += in_bytes;
                out_pos += out_bytes;
                if (st < TINFL_STATUS_DONE) {
                    snprintf(err, err_cap, "compression self-check failed; nothing was written");
                    goto out;
                }
                if (st == TINFL_STATUS_DONE) done = 1;
                else if (in_bytes == 0 && out_bytes == 0) break;
            }
            unc[b] = (uint32_t)(out_pos - before);
        }
        if (!done || out_pos != len || memcmp(check, data, len) != 0) {
            snprintf(err, err_cap, "compression self-check failed; nothing was written");
            goto out;
        }
    }
    rom_log(io, "compressed %u bytes to %u (%u blocks of %u)", (unsigned)len, (unsigned)clen,
            (unsigned)nblocks, (unsigned)STUB_WRITE_SIZE);
    put_u32(req, len);
    put_u32(req + 4, nblocks);
    put_u32(req + 8, STUB_WRITE_SIZE);
    put_u32(req + 12, offset);
    put_u32(req + 16, 0);
    if (stub_cmd(io, STUB_DEFL_BEGIN, req, 20, 0, NULL, NULL, 0, STUB_TIMEOUT_MS, err, err_cap) != 0) goto out;
    if (wrote) *wrote = 1;
    if (io->progress && phase) io->progress(io->ctx, phase, 0, len);
    phase_start(io, &ph, phase && strcmp(phase, "write") == 0 ? "Write" : "Restore", len);
    s_quiet = 1;
    for (uint32_t seq = 0; seq < nblocks; seq++) {
        size_t off = (size_t)seq * STUB_WRITE_SIZE;
        uint32_t n = (uint32_t)(clen - off);
        if (n > STUB_WRITE_SIZE) n = STUB_WRITE_SIZE;
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled during the write");
            goto out;
        }
        put_u32(pkt, n);
        put_u32(pkt + 4, seq);
        put_u32(pkt + 8, 0);
        put_u32(pkt + 12, 0);
        memcpy(pkt + 16, comp + off, n);
        if (stub_cmd(io, STUB_DEFL_DATA, pkt, (uint16_t)(16 + n), xor_sum(comp + off, n), NULL, NULL, 0,
                     (int)timeout, err, err_cap) != 0) {
            rom_log(io, "FLASH_DEFL_DATA seq %u failed after %u bytes: %s", (unsigned)seq, (unsigned)sent, err);
            goto out;
        }
        timeout = timeout_per_mb(40000, unc[seq]); /* ERASE_WRITE_TIMEOUT_PER_MB */
        sent += unc[seq];
        if (io->progress && phase) io->progress(io->ctx, phase, sent, len);
        if (sent >= next_log || seq + 1 == nblocks) {
            rom_log(io, "FLASH_DEFL_DATA %u / %u bytes (seq %u of %u)", (unsigned)sent, (unsigned)len,
                    (unsigned)seq + 1, (unsigned)nblocks);
            while (next_log <= sent) next_log += LOG_EVERY;
        }
    }
    s_quiet = q;
    put_u32(req, 1); /* reboot=False -> stay in the stub */
    if (stub_cmd(io, STUB_DEFL_END, req, 4, 0, NULL, NULL, 0, (int)timeout, err, err_cap) != 0) {
        rom_log(io, "FLASH_DEFL_END failed: %s", err);
        goto out;
    }
    {
        char note[64];
        snprintf(note, sizeof note, "%u bytes on the wire", (unsigned)clen);
        phase_end(io, &ph, len, note);
    }
    rc = 0;
out:
    s_quiet = q;
    free(comp);
    free(check);
    free(pkt);
    free(unc);
    return rc;
}

static int rom_read_slow(rom_io *io, uint32_t addr, uint8_t *dst, uint32_t len,
                         const char *phase, char *err, size_t err_cap)
{
    uint32_t done = 0, next_log = LOG_EVERY;
    if (io->progress && phase) io->progress(io->ctx, phase, 0, len);
    while (done < len) {
        uint8_t req[8], block[ROM_READ_CHUNK];
        size_t got = 0;
        uint32_t chunk = len - done;
        if (chunk > ROM_READ_CHUNK) chunk = ROM_READ_CHUNK;
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled during a flash read");
            return -1;
        }
        put_u32(req, addr + done);
        put_u32(req + 4, chunk);
        if (rom_cmd(io, ROM_READ_SLOW, req, 8, 0, NULL, block, sizeof block, &got, 400, 2, err, err_cap) != 0)
            return -1;
        if (got < chunk) {
            snprintf(err, err_cap, "short read from flash");
            return -1;
        }
        memcpy(dst + done, block, chunk);
        done += chunk;
        if (io->progress && phase && (done == len || (done % 1024u) < chunk))
            io->progress(io->ctx, phase, done, len);
        if (len >= LOG_EVERY && (done >= next_log || done == len)) {
            rom_log(io, "READ_FLASH_SLOW %u / %u bytes", (unsigned)done, (unsigned)len);
            while (next_log <= done) next_log += LOG_EVERY;
        }
    }
    return 0;
}

/* Stub READ_FLASH when the stub is running, else the v1.2.2 ROM 64-byte reads.
 * Per-packet trace lines are suppressed; failures are still logged. */
static int read_exact(rom_io *io, uint32_t addr, uint8_t *dst, uint32_t len,
                      const char *phase, char *err, size_t err_cap)
{
    int q = s_quiet, rc;
    if (s_stub) return stub_read(io, addr, dst, len, phase, err, err_cap);
    if (len >= LOG_EVERY) rom_log(io, "ROM READ_FLASH_SLOW 0x%06X, %u bytes", (unsigned)addr, (unsigned)len);
    s_quiet = 1;
    rc = rom_read_slow(io, addr, dst, len, phase, err, err_cap);
    s_quiet = q;
    if (rc != 0) rom_log(io, "ROM read 0x%06X+%u failed: %s", (unsigned)addr, (unsigned)len, err);
    return rc;
}

static int read_u32_reg(rom_io *io, uint32_t addr, uint32_t *val, char *err, size_t err_cap)
{
    uint8_t req[4];
    put_u32(req, addr);
    return rom_cmd(io, ROM_READ_REG, req, 4, 0, val, NULL, 0, NULL, 400, 2, err, err_cap);
}

static int attach_flash(rom_io *io, char *err, size_t err_cap)
{
    uint8_t arg[8];
    uint8_t params[24];
    memset(arg, 0, sizeof arg);
    if (rom_cmd(io, ROM_SPI_ATTACH, arg, 8, 0, NULL, NULL, 0, NULL, 800, 2, err, err_cap) != 0)
        return -1;
    memset(params, 0, sizeof params);
    put_u32(params + 4, 16u * 1024u * 1024u);
    put_u32(params + 8, 64u * 1024u);
    put_u32(params + 12, 4096);
    put_u32(params + 16, 256);
    put_u32(params + 20, 0xFFFF);
    return rom_cmd(io, ROM_SPI_SET, params, 24, 0, NULL, NULL, 0, NULL, 800, 2, err, err_cap);
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
    if (rom_cmd(io, ROM_CHANGE_BAUD, req, 8, 0, NULL, NULL, 0, NULL, 400, 2, err, err_cap) != 0)
        return -1;
    if (io->set_baud(io->ctx, baud) != 0) {
        snprintf(err, err_cap, "the PC serial port rejected the flash baud");
        return -1;
    }
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
    return 0;
}

static int s_fallback; /* 1 if a stub start failed and the ROM path took over */

/* Enter or re-enter the ROM, attach flash, then either start the flasher stub
 * (use_stub) or raise the ROM baud exactly as v1.2.2 did. */
static int enter_flash_session(rom_io *io, int pulse, int use_stub, char *err, size_t err_cap)
{
    if (pulse) {
        if (connect_bootloader(io, err, err_cap) != 0) return -1;
    } else if (wait_bootloader(io, err, err_cap) != 0) {
        return -1;
    }
    {
        uint32_t chip = 0;
        if (read_u32_reg(io, ROM_CHIP_REG, &chip, err, err_cap) != 0) return -1;
        if (chip != UPD_CHIP_ID_S3) {
            snprintf(err, err_cap, "this is not an ESP32-S3 (id %u); nothing was written", chip);
            return -1;
        }
    }
    if (attach_flash(io, err, err_cap) != 0) return -1;
    if (use_stub && !s_stub_disabled && !io->rom_only) {
        if (stub_start(io, err, err_cap) == 0) return 0;
        rom_log(io, "FALLBACK: flasher stub unavailable (%s). Reconnecting and using the v1.2.2 ROM loader path.",
                err[0] ? err : "no reason");
        s_stub = 0;
        s_stub_disabled = 1;
        s_stub_io_fail = 0;
        s_fallback = 1;
        err[0] = 0;
        if (io->set_baud(io->ctx, UPD_LINK_BAUD) != 0) {
            snprintf(err, err_cap, "could not return the port to 115200");
            return -1;
        }
        return enter_flash_session(io, 1, 0, err, err_cap);
    }
    if (change_baud(io, UPD_FLASH_BAUD_MAX, err, err_cap) != 0) {
        /* 115200 still works. Keep going rather than inventing a higher rate. */
        err[0] = 0;
        if (io->set_baud(io->ctx, UPD_LINK_BAUD) != 0) {
            snprintf(err, err_cap, "could not keep the port at 115200");
            return -1;
        }
    }
    return 0;
}

static int flash_end_stay(rom_io *io, char *err, size_t err_cap)
{
    uint8_t endp[4];
    put_u32(endp, 1); /* stay in the bootloader for the readback, if the ROM honours it */
    if (rom_cmd(io, ROM_FLASH_END, endp, 4, 0, NULL, NULL, 0, NULL, 500, 2, err, err_cap) == 0)
        return 0;
    /* Some ROM sessions send no reply here. Try a sync before giving up. */
    err[0] = 0;
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
    if (wait_bootloader(io, err, err_cap) == 0) return 0;
    return -1;
}

/* Re-establish a bootloader session after a write, for verify. */
static int recover_session(rom_io *io, char *err, size_t err_cap)
{
    if (s_stub) {
        uint32_t chip = 0;
        if (stub_cmd(io, ROM_READ_REG, (const uint8_t *)"\x00\x10\x00\x40", 4, 0, &chip, NULL, 0, 1000, err, err_cap) == 0) {
            err[0] = 0;
            return 0;
        }
        rom_log(io, "flasher stub stopped answering (%s); reconnecting", err);
        s_stub = 0;
        io->set_baud(io->ctx, UPD_LINK_BAUD);
        if (io->reopen) {
            char ignored[160];
            if (reopen_port(io, ignored, sizeof ignored) != 0) {
                snprintf(err, err_cap, "no stub reply, and the port could not be re-opened");
                return -1;
            }
        }
        return enter_flash_session(io, 1, 1, err, err_cap);
    }
    io->sleep_ms(io->ctx, 50);
    io->flush_rx(io->ctx);
    if (wait_bootloader(io, err, err_cap) == 0) {
        if (attach_flash(io, err, err_cap) != 0) return -1;
        change_baud(io, UPD_FLASH_BAUD_MAX, err, err_cap);
        err[0] = 0;
        return 0;
    }
    if (io->reopen) {
        char ignored[160];
        if (reopen_port(io, ignored, sizeof ignored) != 0) {
            snprintf(err, err_cap, "no bootloader reply, and the port could not be re-opened");
            return -1;
        }
    }
    if (enter_flash_session(io, 1, 0, err, err_cap) == 0) return 0;
    return -1;
}

static int flash_region(rom_io *io, uint32_t offset, const uint8_t *data, uint32_t len,
                        char *err, size_t err_cap, const char *phase, int *wrote)
{
    uint32_t erase = (len + 4095u) & ~4095u;
    uint32_t blocks = (len + ROM_BLOCK - 1) / ROM_BLOCK;
    uint8_t begin[20];
    if (!span_ok(offset, len, err, err_cap)) return -1;
    put_u32(begin, erase);
    put_u32(begin + 4, blocks);
    put_u32(begin + 8, ROM_BLOCK);
    put_u32(begin + 12, offset);
    put_u32(begin + 16, 0);
    /* Sector erase of the app slot is a few seconds, not minutes. Two tries. */
    if (rom_cmd(io, ROM_FLASH_BEGIN, begin, 20, 0, NULL, NULL, 0, NULL, 8000, 2, err, err_cap) != 0)
        return -1;
    if (wrote) *wrote = 1;
    s_quiet = 1;
    for (uint32_t seq = 0; seq < blocks; seq++) {
        uint8_t pkt[16 + ROM_BLOCK];
        uint32_t off = seq * ROM_BLOCK;
        uint32_t n = len - off;
        if (n > ROM_BLOCK) n = ROM_BLOCK;
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled during the write");
            s_quiet = 0;
            return -1;
        }
        memset(pkt, 0xFF, sizeof pkt);
        put_u32(pkt, ROM_BLOCK);
        put_u32(pkt + 4, seq);
        put_u32(pkt + 8, 0);
        put_u32(pkt + 12, 0);
        memcpy(pkt + 16, data + off, n);
        if (rom_cmd(io, ROM_FLASH_DATA, pkt, 16 + ROM_BLOCK, xor_sum(pkt + 16, ROM_BLOCK),
                    NULL, NULL, 0, NULL, 2000, 2, err, err_cap) != 0) {
            s_quiet = 0;
            rom_log(io, "FLASH_DATA seq %u failed: %s", (unsigned)seq, err);
            return -1;
        }
        if (io->progress && phase && (seq % 8 == 0 || seq + 1 == blocks))
            io->progress(io->ctx, phase, off + n, len);
        if (seq % 64 == 63 || seq + 1 == blocks)
            rom_log(io, "FLASH_DATA %u / %u bytes", (unsigned)(off + n), (unsigned)len);
    }
    s_quiet = 0;
    /* A missing FLASH_END reply is not a failed write. Caller recovers, then verifies. */
    flash_end_stay(io, err, err_cap);
    err[0] = 0;
    return 0;
}

static int confirm_after_reset(rom_io *io, const char *want, rom_result *result, char *err, size_t err_cap)
{
    uint8_t head[256];
    char ver[32];
    hard_reset(io);
    if (reopen_port(io, err, err_cap) != 0) return -1;
    if (enter_flash_session(io, 1, 0, err, err_cap) != 0) return -1;
    if (read_exact(io, UPD_APP_OFFSET, head, sizeof head, "confirm", err, err_cap) != 0)
        return -1;
    if (!upd_image_version(head, sizeof head, ver, sizeof ver, err, err_cap))
        return -1;
    snprintf(result->confirmed_version, sizeof result->confirmed_version, "%s", ver);
    result->confirmed = 1;
    hard_reset(io);
    reopen_port(io, err, err_cap); /* best-effort; Link will also re-open */
    err[0] = 0;
    if (want && strcmp(ver, want) != 0) {
        snprintf(err, err_cap, "after reset the board reports %s, not %s", ver, want);
        return -1;
    }
    return 0;
}

/* Measure the installed app from its header, walking segments on the device
 * when the header alone is not enough. Same logic as v1.2.2. */
static int measure_installed(rom_io *io, uint32_t *old_len, char *err, size_t err_cap)
{
    uint8_t header[64];
    if (read_exact(io, UPD_APP_OFFSET, header, sizeof header, NULL, err, err_cap) != 0) return -1;
    if (header[0] != UPD_IMAGE_MAGIC) {
        snprintf(err, err_cap, "no application image at 0x10000; nothing was written");
        return -1;
    }
    if (!upd_image_length(header, sizeof header, old_len, err, err_cap)) {
        unsigned segs = header[1];
        uint16_t id = (uint16_t)(header[12] | (header[13] << 8));
        size_t pos = 24;
        if (id != UPD_CHIP_ID_S3 || segs == 0 || segs > 16) {
            snprintf(err, err_cap, "the application header is not CrowPanel firmware");
            return -1;
        }
        for (unsigned i = 0; i < segs; i++) {
            uint8_t sh[8];
            uint32_t sz;
            if (read_exact(io, UPD_APP_OFFSET + (uint32_t)pos, sh, 8, NULL, err, err_cap) != 0) return -1;
            sz = get_u32(sh + 4);
            if (sz > UPD_FACTORY_SIZE) {
                snprintf(err, err_cap, "the installed application is an unexpected size");
                return -1;
            }
            pos += 8u + sz;
        }
        *old_len = ((uint32_t)pos + 16u) & ~15u;
        if (header[23] == 1) *old_len += 32;
    }
    if (*old_len < 24 || *old_len > UPD_FACTORY_SIZE) {
        snprintf(err, err_cap, "could not measure the installed application");
        return -1;
    }
    err[0] = 0;
    return 0;
}

/* Write with whichever loader is running. App-only updates pass 0x10000.
 * First-install also passes the bootloader and the partition table. */
static int write_region_at(rom_io *io, uint32_t offset, const uint8_t *data, uint32_t len,
                           const char *phase, char *err, size_t err_cap, int *wrote)
{
    if (s_stub) return stub_flash_defl(io, offset, data, len, err, err_cap, phase, wrote);
    return flash_region(io, offset, data, len, err, err_cap, phase, wrote);
}

static int write_region(rom_io *io, const uint8_t *data, uint32_t len, const char *phase,
                        char *err, size_t err_cap, int *wrote)
{
    return write_region_at(io, UPD_APP_OFFSET, data, len, phase, err, err_cap, wrote);
}

/* 0 = flash matches, 1 = mismatch, -1 = could not compare.
 * Stub: SPI_FLASH_MD5 (no readback). ROM: v1.2.2 full readback. */
static int verify_region_at(rom_io *io, uint32_t offset, const uint8_t *data, uint32_t len,
                            const char *phase, char *err, size_t err_cap)
{
    if (s_stub) {
        uint8_t want[16], got[16];
        char a[33], b[33];
        md5_buf(data, len, want);
        if (io->progress && phase) io->progress(io->ctx, phase, 0, len);
        if (stub_md5(io, offset, len, got, err, err_cap) != 0) return -1;
        hex16(want, a);
        hex16(got, b);
        rom_log(io, "flash MD5 0x%06X+%u: %s, expected %s", (unsigned)offset, (unsigned)len, b, a);
        if (io->progress && phase) io->progress(io->ctx, phase, len, len);
        if (memcmp(want, got, 16) != 0) {
            snprintf(err, err_cap, "flash MD5 did not match the image at 0x%X", (unsigned)offset);
            return 1;
        }
        return 0;
    }
    {
        uint8_t *check = (uint8_t *)malloc(len);
        int cmp;
        if (!check) {
            snprintf(err, err_cap, "not enough memory to read the image back");
            return -1;
        }
        if (read_exact(io, offset, check, len, phase ? "readback" : NULL, err, err_cap) != 0) {
            free(check);
            return -1;
        }
        cmp = memcmp(check, data, len);
        free(check);
        if (cmp != 0) {
            snprintf(err, err_cap, "readback did not match the image at 0x%X", (unsigned)offset);
            return 1;
        }
        return 0;
    }
}

static int verify_region(rom_io *io, const uint8_t *data, uint32_t len, const char *phase,
                         char *err, size_t err_cap)
{
    return verify_region_at(io, UPD_APP_OFFSET, data, len, phase, err, err_cap);
}

static unsigned since(rom_io *io, unsigned t0)
{
    return now_ms(io) - t0;
}

static int s_write_attempted;

static int update_session(rom_io *io, const uint8_t *image, uint32_t image_len,
                          const char *package_version, int allow_same, int do_write, int use_stub,
                          int (*backup_write)(void *ctx, const uint8_t *data, size_t n),
                          void *backup_ctx, rom_result *result)
{
    char err[160];
    uint8_t *backup = NULL;
    uint32_t old_len = 0;
    uint8_t table[UPD_PART_TABLE_LEN];
    int data_sent = 0;
    unsigned t0;
    phase_t ph;
    err[0] = 0;
    if (io->progress) io->progress(io->ctx, "reset", 0, 1);
    rom_log(io, "starting %s, image_len=%u, do_write=%d, path=%s",
            package_version ? package_version : "?", (unsigned)image_len, do_write,
            use_stub && !io->rom_only && !s_stub_disabled ? "stub (ROM fallback)" : "ROM only");
    t0 = now_ms(io);
    if (enter_flash_session(io, 1, use_stub, err, sizeof err) != 0) goto leave;
    result->ms_connect = since(io, t0);
    rom_log(io, "connected via %s in %u ms", s_stub ? "flasher stub" : "ROM loader", result->ms_connect);
    if (read_exact(io, UPD_PART_TABLE, table, sizeof table, NULL, err, sizeof err) != 0) goto leave;
    if (!upd_partition_is_cp7(table, sizeof table, err, sizeof err)) goto leave;
    if (measure_installed(io, &old_len, err, sizeof err) != 0) goto leave;
    backup = (uint8_t *)malloc(old_len);
    if (!backup) {
        snprintf(err, sizeof err, "not enough memory to back up the application");
        goto leave;
    }
    t0 = now_ms(io);
    if (s_stub && do_write) {
        uint8_t fmd5[16], bmd5[16];
        char hx[33];
        int have = 0;
        if (io->progress) io->progress(io->ctx, "check", 0, old_len);
        if (stub_md5(io, UPD_APP_OFFSET, old_len, fmd5, err, sizeof err) != 0) goto leave;
        hex16(fmd5, hx);
        rom_log(io, "board app MD5 0x10000+%u: %s", (unsigned)old_len, hx);
        if (!io->full_backup) {
            if (old_len == image_len) {
                md5_buf(image, image_len, bmd5);
                if (memcmp(bmd5, fmd5, 16) == 0) {
                    memcpy(backup, image, old_len);
                    have = 1;
                    snprintf(result->backup_source, sizeof result->backup_source, "package file (board MD5 matched)");
                }
            }
            if (!have && io->find_backup && io->find_backup(io->ctx, old_len, fmd5, backup) == 0) {
                md5_buf(backup, old_len, bmd5);
                if (memcmp(bmd5, fmd5, 16) == 0) {
                    have = 1;
                    snprintf(result->backup_source, sizeof result->backup_source, "earlier backup on this PC (board MD5 matched)");
                }
            }
        }
        if (!have) {
            phase_start(io, &ph, "Backup", old_len);
            if (read_exact(io, UPD_APP_OFFSET, backup, old_len, "backup", err, sizeof err) != 0) goto leave;
            md5_buf(backup, old_len, bmd5);
            if (memcmp(bmd5, fmd5, 16) != 0) {
                snprintf(err, sizeof err, "backup read did not match the board's MD5; nothing was written");
                goto leave;
            }
            phase_end(io, &ph, old_len, "stub READ_FLASH, MD5 matched");
            snprintf(result->backup_source, sizeof result->backup_source, "read from the board (MD5 checked)");
        } else {
            if (io->progress) io->progress(io->ctx, "backup", old_len, old_len);
            rom_log(io, "backup taken from the %s; no flash read needed", result->backup_source);
        }
    } else {
        if (do_write) phase_start(io, &ph, "Backup", old_len);
        if (read_exact(io, UPD_APP_OFFSET, backup, old_len, do_write ? "backup" : NULL, err, sizeof err) != 0) goto leave;
        {
            uint8_t edge[64];
            uint32_t tail = old_len >= 64 ? old_len - 64 : 0;
            if (read_exact(io, UPD_APP_OFFSET, edge, 64, NULL, err, sizeof err) != 0) goto leave;
            if (memcmp(edge, backup, 64) != 0) {
                snprintf(err, sizeof err, "backup read did not repeat; nothing was written");
                goto leave;
            }
            if (old_len >= 64) {
                if (read_exact(io, UPD_APP_OFFSET + tail, edge, 64, NULL, err, sizeof err) != 0) goto leave;
                if (memcmp(edge, backup + tail, 64) != 0) {
                    snprintf(err, sizeof err, "backup tail did not repeat; nothing was written");
                    goto leave;
                }
            }
        }
        if (do_write) phase_end(io, &ph, old_len, s_stub ? "stub READ_FLASH" : "ROM READ_FLASH_SLOW");
        snprintf(result->backup_source, sizeof result->backup_source,
                 s_stub ? "read from the board (stub)" : "read from the board (ROM)");
    }
    result->ms_backup = since(io, t0);
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
        reopen_port(io, err, sizeof err);
        err[0] = 0;
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
    s_write_attempted = 1;
    result->used_stub = s_stub;
    t0 = now_ms(io);
    if (io->progress) io->progress(io->ctx, "write", 0, image_len);
    if (!s_stub) phase_start(io, &ph, "Write", image_len);
    if (write_region(io, image, image_len, "write", err, sizeof err, &result->wrote) != 0) {
        /* The write itself failed. A rollback is only useful if we can still talk. */
        data_sent = 0;
        goto maybe_restore;
    }
    if (!s_stub) phase_end(io, &ph, image_len, "ROM FLASH_DATA, uncompressed");
    result->ms_write = since(io, t0);
    data_sent = 1;
    result->wrote = 1;
    t0 = now_ms(io);
    if (!s_stub && recover_session(io, err, sizeof err) != 0) {
        snprintf(result->error, sizeof result->error,
                 "the write finished, but verify could not run (%s). The previous app was not rolled back, "
                 "because verify never ran. Check About. If the screen is blank, write the backup at 0x10000 "
                 "with esptool at 115200. This is not an atomic update.",
                 err[0] ? err : "no bootloader reply");
        hard_reset(io);
        reopen_port(io, err, sizeof err);
        free(backup);
        return -1;
    }
    {
        int v;
        phase_start(io, &ph, "Verify", image_len);
        v = verify_region(io, image, image_len, "verify", err, sizeof err);
        if (v < 0 && s_stub_disabled == 0 && result->used_stub) {
            /* The stub may have stopped. Reconnect (stub, or ROM if the stub fails) and try once more. */
            rom_log(io, "verify could not run (%s); reconnecting once", err);
            if (recover_session(io, err, sizeof err) == 0)
                v = verify_region(io, image, image_len, "verify", err, sizeof err);
        }
        if (v < 0) goto verify_unavailable;
        result->verify_ran = 1;
        if (v == 1) goto maybe_restore;
        phase_end(io, &ph, image_len, s_stub ? "SPI_FLASH_MD5" : "ROM readback");
        result->verified = 1;
    }
    result->ms_verify = since(io, t0);
    err[0] = 0;
    t0 = now_ms(io);
    if (confirm_after_reset(io, package_version, result, err, sizeof err) != 0) {
        result->ms_confirm = since(io, t0);
        /* Verify already matched. A failed confirm is a warning, not a rollback. */
        snprintf(result->error, sizeof result->error,
                 "updated to %s. %s matched, but the version could not be confirmed after reset (%s). Check About.",
                 package_version, result->used_stub ? "Flash MD5" : "Readback", err[0] ? err : "no reply");
        err[0] = 0;
        free(backup);
        return 0;
    }
    result->ms_confirm = since(io, t0);
    if (result->confirmed)
        snprintf(result->error, sizeof result->error, "updated to %s and confirmed after reset", package_version);
    else
        snprintf(result->error, sizeof result->error, "updated to %s", package_version);
    free(backup);
    return 0;

verify_unavailable:
    snprintf(result->error, sizeof result->error,
             "the write finished, but verify could not run (%s). The previous app was not rolled back, "
             "because verify never ran. Check About. If the screen is blank, write the backup at 0x10000 "
             "with esptool at 115200. This is not an atomic update.",
             err[0] ? err : "no bootloader reply");
    hard_reset(io);
    reopen_port(io, err, sizeof err);
    free(backup);
    return -1;

maybe_restore:
    /* Only roll back when a live session can still take commands, and only if
     * we know the new image is bad or the write did not finish. */
    if (data_sent && !result->verify_ran) {
        goto verify_unavailable;
    }
    rom_log(io, "ROLLBACK: %s", err[0] ? err : "write or verify failed");
    if (recover_session(io, err, sizeof err) != 0) {
        snprintf(result->error, sizeof result->error,
                 "the write did not finish, and the bootloader is no longer answering (%s). "
                 "The previous app was not rolled back. Use the backup file with a 115200-baud "
                 "app-only write at 0x10000. This is not an atomic update.",
                 err[0] ? err : "no bootloader reply");
        hard_reset(io);
        reopen_port(io, err, sizeof err);
        free(backup);
        return -1;
    }
    if (backup && write_region(io, backup, old_len, NULL, err, sizeof err, NULL) == 0 &&
        (s_stub || recover_session(io, err, sizeof err) == 0) &&
        verify_region(io, backup, old_len, NULL, err, sizeof err) == 0) {
        result->restored = 1;
        rom_log(io, "ROLLBACK: previous app written back and verified");
        snprintf(result->error, sizeof result->error,
                 "the new image did not verify; the previous app was written back. NVS was not erased. %s",
                 err[0] ? err : "verify mismatch");
        hard_reset(io);
        reopen_port(io, err, sizeof err);
        free(backup);
        return -1;
    }
    snprintf(result->error, sizeof result->error,
             "write failed and the previous app was not verified. NVS was not targeted, but the app may not boot. "
             "Use the backup file with a 115200-baud app-only write at 0x10000. This is not an atomic update. %s",
             err[0] ? err : "no bootloader reply");
    hard_reset(io);
    reopen_port(io, err, sizeof err);
    free(backup);
    return -1;

leave_reset:
    hard_reset(io);
    {
        char ignored[160];
        reopen_port(io, ignored, sizeof ignored);
    }
leave:
    fail(result, err[0] ? err : "update stopped");
    free(backup);
    return -1;
}

static int check_io(rom_io *io, rom_result *result)
{
    if (!io || !io->write || !io->read || !io->set_baud || !io->set_dtr_rts || !io->flush_rx || !io->sleep_ms) {
        fail(result, "internal error: missing serial operations");
        return -1;
    }
    return 0;
}

static void reset_state(void)
{
    s_stub = 0;
    s_stub_disabled = 0;
    s_stub_io_fail = 0;
    s_quiet = 0;
    s_fallback = 0;
    s_write_attempted = 0;
    s_layout_write = 0;
}

int rom_update_app(rom_io *io, const uint8_t *image, uint32_t image_len,
                   const char *package_version, int allow_same, int do_write,
                   int (*backup_write)(void *ctx, const uint8_t *data, size_t n),
                   void *backup_ctx, rom_result *result)
{
    char err[160];
    uint32_t new_len = 0;
    int rc;
    if (!result) return -1;
    memset(result, 0, sizeof *result);
    err[0] = 0;
    reset_state();
    if (check_io(io, result) != 0) return -1;
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
    rc = update_session(io, image, image_len, package_version, allow_same, do_write, 1,
                        backup_write, backup_ctx, result);
    if (rc != 0 && s_stub_io_fail && !s_write_attempted && !result->wrote &&
        !(io->cancelled && io->cancelled(io->ctx))) {
        char first[384];
        snprintf(first, sizeof first, "%s", result->error);
        rom_log(io, "FALLBACK: the flasher stub failed before anything was written (%s). "
                    "Running the whole update again on the v1.2.2 ROM path.", first);
        hard_reset(io);
        reopen_port(io, err, sizeof err);
        io->set_baud(io->ctx, UPD_LINK_BAUD);
        memset(result, 0, sizeof *result);
        s_stub = 0;
        s_stub_disabled = 1;
        s_stub_io_fail = 0;
        rc = update_session(io, image, image_len, package_version, allow_same, do_write, 0,
                            backup_write, backup_ctx, result);
        s_fallback = 1;
    }
    result->stub_fallback = s_fallback;
    return rc;
}

int rom_probe(rom_io *io, rom_probe_result *pr)
{
    char err[160];
    uint8_t table[UPD_PART_TABLE_LEN], head[256];
    uint32_t len = 0;
    unsigned t0;
    int rc = -1;
    if (!pr) return -1;
    memset(pr, 0, sizeof *pr);
    err[0] = 0;
    reset_state();
    if (!io || !io->write || !io->read || !io->set_baud || !io->set_dtr_rts || !io->flush_rx || !io->sleep_ms) {
        snprintf(pr->error, sizeof pr->error, "internal error: missing serial operations");
        return -1;
    }
    rom_log(io, "probe: read-only. Nothing is written.");
    t0 = now_ms(io);
    if (enter_flash_session(io, 1, 1, err, sizeof err) != 0) goto out;
    pr->ms_connect = since(io, t0);
    pr->chip_id = UPD_CHIP_ID_S3;
    pr->stub = s_stub;
    pr->stub_fallback = s_fallback;
    pr->baud = s_stub ? UPD_FLASH_BAUD_MAX : 0;
    if (read_exact(io, UPD_PART_TABLE, table, sizeof table, NULL, err, sizeof err) != 0) goto out;
    pr->layout_cp7 = upd_partition_is_cp7(table, sizeof table, err, sizeof err);
    err[0] = 0;
    if (measure_installed(io, &len, err, sizeof err) != 0) goto out;
    pr->app_len = len;
    if (read_exact(io, UPD_APP_OFFSET, head, sizeof head, NULL, err, sizeof err) != 0) goto out;
    if (!upd_image_version(head, sizeof head, pr->version, sizeof pr->version, err, sizeof err)) {
        pr->version[0] = 0;
        err[0] = 0;
    }
    t0 = now_ms(io);
    if (s_stub) {
        uint8_t md[16];
        if (stub_md5(io, UPD_APP_OFFSET, len, md, err, sizeof err) != 0) goto out;
        hex16(md, pr->app_md5);
    } else {
        uint8_t *buf = (uint8_t *)malloc(len), md[16];
        if (!buf) {
            snprintf(err, sizeof err, "not enough memory");
            goto out;
        }
        if (read_exact(io, UPD_APP_OFFSET, buf, len, "backup", err, sizeof err) != 0) {
            free(buf);
            goto out;
        }
        md5_buf(buf, len, md);
        free(buf);
        hex16(md, pr->app_md5);
    }
    pr->ms_md5 = since(io, t0);
    rom_log(io, "probe: chip ESP32-S3, %s, layout %s, app %u bytes, version %s, MD5 %s",
            pr->stub ? "flasher stub" : "ROM only", pr->layout_cp7 ? "cp7" : "NOT cp7", (unsigned)pr->app_len,
            pr->version[0] ? pr->version : "?", pr->app_md5);
    rc = 0;
out:
    if (rc != 0) snprintf(pr->error, sizeof pr->error, "%s", err[0] ? err : "probe failed");
    hard_reset(io);
    {
        char ignored[160];
        reopen_port(io, ignored, sizeof ignored);
    }
    return rc;
}

#define ROM_BACKUP_CHUNK 0x10000u

static int image_is_esp(const uint8_t *p, uint32_t len, char *err, size_t cap)
{
    uint32_t total = 0;
    if (!p || len < 24 || p[0] != UPD_IMAGE_MAGIC) {
        snprintf(err, cap, "image is not an ESP32 image");
        return 0;
    }
    if ((uint16_t)(p[12] | (p[13] << 8)) != UPD_CHIP_ID_S3) {
        snprintf(err, cap, "image is not for an ESP32-S3");
        return 0;
    }
    if (!upd_image_length(p, len, &total, err, cap) || total != len) {
        if (err && cap && !err[0]) snprintf(err, cap, "image length does not match its header");
        return 0;
    }
    return 1;
}

static const char *phase_for(uint32_t offset, int verify)
{
    if (offset == 0) return verify ? "verify-boot" : "write-boot";
    if (offset == UPD_PART_TABLE) return verify ? "verify-part" : "write-part";
    return verify ? "verify-app" : "write-app";
}

static uint32_t erase_span(uint32_t len)
{
    return (len + 4095u) & ~4095u;
}

static int backup_flash(rom_io *io, uint32_t len,
                        int (*backup_write)(void *, uint32_t, const uint8_t *, uint32_t),
                        void *ctx, char *err, size_t err_cap)
{
    uint8_t *buf = (uint8_t *)malloc(ROM_BACKUP_CHUNK);
    uint32_t off = 0;
    if (!buf) {
        snprintf(err, err_cap, "not enough memory to back up the flash");
        return -1;
    }
    if (io->progress) io->progress(io->ctx, "backup", 0, len);
    while (off < len) {
        uint32_t n = len - off;
        if (n > ROM_BACKUP_CHUNK) n = ROM_BACKUP_CHUNK;
        if (io->cancelled && io->cancelled(io->ctx)) {
            snprintf(err, err_cap, "cancelled during the backup; nothing was written");
            free(buf);
            return -1;
        }
        if (read_exact(io, off, buf, n, NULL, err, err_cap) != 0) {
            free(buf);
            return -1;
        }
        if (!backup_write || backup_write(ctx, off, buf, n) != 0) {
            snprintf(err, err_cap, "could not save the backup on this PC; nothing was written");
            free(buf);
            return -1;
        }
        off += n;
        if (io->progress) io->progress(io->ctx, "backup", off, len);
        if ((off % (LOG_EVERY * 4u) == 0) || off == len)
            rom_log(io, "full backup %u / %u bytes", (unsigned)off, (unsigned)len);
    }
    free(buf);
    return 0;
}

static int backup_matches_edge(rom_io *io, uint32_t len,
                               int (*backup_read)(void *, uint32_t, uint8_t *, uint32_t),
                               void *ctx, char *err, size_t err_cap)
{
    uint8_t fileb[64], chipb[64];
    uint32_t tail;
    if (backup_read(ctx, 0, fileb, 64) != 0) {
        snprintf(err, err_cap, "could not re-read the backup; nothing was written");
        return -1;
    }
    if (read_exact(io, 0, chipb, 64, NULL, err, err_cap) != 0) return -1;
    if (memcmp(fileb, chipb, 64) != 0) {
        snprintf(err, err_cap, "backup head did not match the chip; nothing was written");
        return -1;
    }
    tail = len - 64;
    if (backup_read(ctx, tail, fileb, 64) != 0) {
        snprintf(err, err_cap, "could not re-read the backup tail; nothing was written");
        return -1;
    }
    if (read_exact(io, tail, chipb, 64, NULL, err, err_cap) != 0) return -1;
    if (memcmp(fileb, chipb, 64) != 0) {
        snprintf(err, err_cap, "backup tail did not match the chip; nothing was written");
        return -1;
    }
    return 0;
}

static int restore_regions(rom_io *io, const rom_install_image *ordered[3], const int began[3],
                           int (*backup_read)(void *, uint32_t, uint8_t *, uint32_t),
                           void *ctx, char *err, size_t err_cap)
{
    int i;
    for (i = 2; i >= 0; i--) {
        uint8_t *buf;
        int dummy = 0;
        const rom_install_image *im;
        if (!began[i]) continue;
        im = ordered[i];
        buf = (uint8_t *)malloc(im->len);
        if (!buf) {
            snprintf(err, err_cap, "not enough memory to roll back");
            return -1;
        }
        if (backup_read(ctx, im->offset, buf, im->len) != 0) {
            free(buf);
            snprintf(err, err_cap, "could not read the backup for %s", im->name ? im->name : "a region");
            return -1;
        }
        rom_log(io, "ROLLBACK: writing %s back at 0x%X (%u bytes)", im->name ? im->name : "?",
                (unsigned)im->offset, (unsigned)im->len);
        if (write_region_at(io, im->offset, buf, im->len, "restore", err, err_cap, &dummy) != 0) {
            free(buf);
            return -1;
        }
        if (!s_stub && recover_session(io, err, err_cap) != 0) {
            free(buf);
            return -1;
        }
        if (verify_region_at(io, im->offset, buf, im->len, NULL, err, err_cap) != 0) {
            free(buf);
            return -1;
        }
        free(buf);
    }
    return 0;
}

static void finish_reset(rom_io *io)
{
    char ignored[160];
    hard_reset(io);
    reopen_port(io, ignored, sizeof ignored);
}

static int reconnect_loader(rom_io *io, char *err, size_t err_cap)
{
    if (recover_session(io, err, err_cap) == 0) return 0;
    return enter_flash_session(io, 1, !s_stub_disabled && !io->rom_only, err, err_cap);
}

static int install_session(rom_io *io, const rom_install_image *ordered[3], uint32_t backup_len,
                           const char *expect_version, int do_write, int use_stub,
                           int (*backup_write)(void *, uint32_t, const uint8_t *, uint32_t),
                           int (*backup_read)(void *, uint32_t, uint8_t *, uint32_t),
                           void *backup_ctx, rom_install_result *result)
{
    char err[220];
    int began[3] = {0, 0, 0};
    int i, any = 0;
    unsigned t0;
    err[0] = 0;
    if (io->progress) io->progress(io->ctx, "reset", 0, 1);
    rom_log(io, "first-install %s, backup_len=%u, do_write=%d, path=%s. NVS, phy_init, and assets are not write targets.",
            expect_version, (unsigned)backup_len, do_write,
            use_stub && !io->rom_only && !s_stub_disabled ? "stub (ROM fallback)" : "ROM only");
    t0 = now_ms(io);
    if (enter_flash_session(io, 1, use_stub, err, sizeof err) != 0) goto leave;
    result->ms_connect = since(io, t0);
    result->used_stub = s_stub;
    rom_log(io, "connected via %s in %u ms", s_stub ? "flasher stub" : "ROM loader", result->ms_connect);
    t0 = now_ms(io);
    if (backup_flash(io, backup_len, backup_write, backup_ctx, err, sizeof err) != 0) goto leave;
    if (backup_matches_edge(io, backup_len, backup_read, backup_ctx, err, sizeof err) != 0) goto leave;
    result->ms_backup = since(io, t0);
    result->backed_up = 1;
    result->backup_bytes = backup_len;
    rom_log(io, "full flash backup saved (%u bytes) before any write", (unsigned)backup_len);
    if (!do_write) {
        finish_reset(io);
        snprintf(result->error, sizeof result->error,
                 "backup of %u bytes saved; nothing was written", (unsigned)backup_len);
        return 0;
    }
    if (io->cancelled && io->cancelled(io->ctx)) {
        snprintf(err, sizeof err, "cancelled before writing");
        goto leave_reset;
    }
    s_write_attempted = 1;
    t0 = now_ms(io);
    for (i = 0; i < 3; i++) {
        int region_wrote = 0;
        const rom_install_image *im = ordered[i];
        rom_log(io, "writing %s at 0x%X (%u bytes)", im->name ? im->name : "?",
                (unsigned)im->offset, (unsigned)im->len);
        if (write_region_at(io, im->offset, im->data, im->len, phase_for(im->offset, 0),
                            err, sizeof err, &region_wrote) != 0) {
            if (region_wrote) began[i] = 1;
            if (region_wrote) result->wrote = 1;
            goto rollback;
        }
        began[i] = 1;
        result->wrote = 1;
    }
    result->ms_write = since(io, t0);
    t0 = now_ms(io);
    for (i = 0; i < 3; i++) {
        const rom_install_image *im = ordered[i];
        int v = verify_region_at(io, im->offset, im->data, im->len, phase_for(im->offset, 1), err, sizeof err);
        if (v < 0 && s_stub) {
            rom_log(io, "verify could not run (%s); reconnecting once", err[0] ? err : "no reply");
            if (recover_session(io, err, sizeof err) == 0)
                v = verify_region_at(io, im->offset, im->data, im->len, phase_for(im->offset, 1), err, sizeof err);
        }
        if (v > 0) result->verify_ran = 1;
        if (v != 0) goto rollback;
    }
    result->verify_ran = 1;
    result->verified = 1;
    result->ms_verify = since(io, t0);
    err[0] = 0;
    t0 = now_ms(io);
    {
        rom_result tmp;
        memset(&tmp, 0, sizeof tmp);
        if (io->progress) io->progress(io->ctx, "confirm", 0, 1);
        if (confirm_after_reset(io, expect_version, &tmp, err, sizeof err) != 0) {
            char why[220];
            result->ms_confirm = since(io, t0);
            result->confirmed = tmp.confirmed;
            snprintf(result->confirmed_version, sizeof result->confirmed_version, "%s", tmp.confirmed_version);
            snprintf(why, sizeof why, "%s", err[0] ? err : "version was not confirmed");
            rom_log(io, "ROLLBACK: version confirm failed (%s)", why);
            if (enter_flash_session(io, 1, !s_stub_disabled && !io->rom_only, err, sizeof err) != 0) {
                snprintf(result->error, sizeof result->error,
                         "did not confirm %s (%.120s), and rollback could not reconnect. "
                         "Write the full-flash backup at 0x0 with esptool at 115200. Do not erase the chip.",
                         expect_version, why);
                finish_reset(io);
                return -1;
            }
            snprintf(err, sizeof err, "%s", why);
            goto rollback_connected;
        }
        result->ms_confirm = since(io, t0);
        result->confirmed = tmp.confirmed;
        snprintf(result->confirmed_version, sizeof result->confirmed_version, "%s", tmp.confirmed_version);
    }
    snprintf(result->error, sizeof result->error,
             "installed %s and confirmed after reset. Bootloader, partition table, and app were written and checked. NVS was not written.",
             expect_version);
    return 0;

rollback:
    rom_log(io, "ROLLBACK: %s", err[0] ? err : "write or verify failed");
    if (reconnect_loader(io, err, sizeof err) != 0) {
        snprintf(result->error, sizeof result->error,
                 "the write failed and the bootloader is no longer answering (%s). "
                 "The backup file is on this PC. Write it back at 0x0 with esptool at 115200. "
                 "Do not erase the chip. NVS was not a target of this installer.",
                 err[0] ? err : "no reply");
        finish_reset(io);
        return -1;
    }
rollback_connected:
    for (i = 0; i < 3; i++) any += began[i];
    if (!any) {
        snprintf(result->error, sizeof result->error, "nothing was programmed. %s", err[0] ? err : "stopped");
        finish_reset(io);
        return -1;
    }
    if (restore_regions(io, ordered, began, backup_read, backup_ctx, err, sizeof err) == 0) {
        result->restored = 1;
        rom_log(io, "ROLLBACK: started regions restored from the backup and checked. NVS was not written.");
        snprintf(result->error, sizeof result->error,
                 "install failed and the backup was written back over the regions that had been started. NVS was not written. %s",
                 err[0] ? err : "verify mismatch");
        finish_reset(io);
        return -1;
    }
    snprintf(result->error, sizeof result->error,
             "install failed and the rollback did not verify. Write the full-flash backup at 0x0, baud 115200. "
             "Do not erase the chip. %s",
             err[0] ? err : "no reply");
    finish_reset(io);
    return -1;

leave_reset:
    finish_reset(io);
leave:
    snprintf(result->error, sizeof result->error, "%s", err[0] ? err : "install stopped");
    if (result->backed_up) finish_reset(io);
    return -1;
}

static int classify_images(const rom_install_image *images, int nimages, const char *expect_version,
                           const rom_install_image *ordered[3], uint32_t backup_len,
                           char *err, size_t err_cap)
{
    const rom_install_image *boot = NULL, *part = NULL, *app = NULL;
    char ver[32];
    int i;
    if (!images || nimages != 3 || !expect_version || !expect_version[0]) {
        snprintf(err, err_cap, "first-install needs a bootloader, a partition table, and an app");
        return -1;
    }
    if (!backup_len || (backup_len & 4095u) || backup_len < 64 || backup_len > ROM_FLASH_SIZE) {
        snprintf(err, err_cap, "backup length must be a 4 KB multiple up to 16 MB");
        return -1;
    }
    for (i = 0; i < nimages; i++) {
        if (images[i].offset == 0) boot = &images[i];
        else if (images[i].offset == UPD_PART_TABLE) part = &images[i];
        else if (images[i].offset == UPD_APP_OFFSET) app = &images[i];
        else {
            snprintf(err, err_cap, "refusing a first-install region at 0x%X", (unsigned)images[i].offset);
            return -1;
        }
    }
    if (!boot || !part || !app) {
        snprintf(err, err_cap, "first-install needs the bootloader, the partition table, and the app");
        return -1;
    }
    if (!image_is_esp(boot->data, boot->len, err, err_cap) || erase_span(boot->len) > UPD_PART_TABLE) {
        if (!err[0]) snprintf(err, err_cap, "bootloader does not fit below 0x8000");
        return -1;
    }
    if (!part->data || part->len < 96 || part->len > UPD_PART_TABLE_LEN ||
        erase_span(part->len) > 0x1000 ||
        !upd_partition_is_cp7(part->data, part->len, err, err_cap)) {
        if (!err[0]) snprintf(err, err_cap, "partition table is not the cp7 layout");
        return -1;
    }
    if (!image_is_esp(app->data, app->len, err, err_cap) ||
        !upd_range_is_app_slot(UPD_APP_OFFSET, app->len) ||
        UPD_APP_OFFSET + erase_span(app->len) > UPD_APP_SLOT_END) {
        if (!err[0]) snprintf(err, err_cap, "app image is not a factory-slot image");
        return -1;
    }
    if (!upd_image_version(app->data, app->len, ver, sizeof ver, err, err_cap) || strcmp(ver, expect_version) != 0) {
        snprintf(err, err_cap, "app image version does not match %s", expect_version);
        return -1;
    }
    if (backup_len < UPD_APP_OFFSET + erase_span(app->len)) {
        snprintf(err, err_cap, "backup does not cover the app region");
        return -1;
    }
    ordered[0] = boot;
    ordered[1] = part;
    ordered[2] = app;
    return 0;
}

int rom_first_install(rom_io *io, const rom_install_image *images, int nimages,
                      uint32_t backup_len, const char *expect_version, int do_write,
                      int (*backup_write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t n),
                      int (*backup_read)(void *ctx, uint32_t offset, uint8_t *dst, uint32_t n),
                      void *backup_ctx, rom_install_result *result)
{
    char err[220];
    const rom_install_image *ordered[3];
    rom_result gate;
    int rc;
    if (!result) return -1;
    memset(result, 0, sizeof *result);
    result->nvs_written = 0;
    err[0] = 0;
    if (classify_images(images, nimages, expect_version, ordered, backup_len, err, sizeof err) != 0) {
        snprintf(result->error, sizeof result->error, "%s", err);
        return -1;
    }
    if (!backup_write || !backup_read) {
        snprintf(result->error, sizeof result->error, "a backup file is required before anything is written");
        return -1;
    }
    reset_state();
    s_layout_write = 1;
    if (check_io(io, &gate) != 0) {
        snprintf(result->error, sizeof result->error, "%s", gate.error);
        s_layout_write = 0;
        return -1;
    }
    rc = install_session(io, ordered, backup_len, expect_version, do_write, 1,
                         backup_write, backup_read, backup_ctx, result);
    if (rc != 0 && s_stub_io_fail && !s_write_attempted && !result->wrote &&
        !(io->cancelled && io->cancelled(io->ctx))) {
        char first[384];
        snprintf(first, sizeof first, "%s", result->error);
        rom_log(io, "FALLBACK: the flasher stub failed before anything was written (%s). "
                    "Running the whole first-install again on the ROM loader path.", first);
        finish_reset(io);
        io->set_baud(io->ctx, UPD_LINK_BAUD);
        memset(result, 0, sizeof *result);
        result->nvs_written = 0;
        s_stub = 0;
        s_stub_disabled = 1;
        s_stub_io_fail = 0;
        s_fallback = 0;
        s_layout_write = 1;
        rc = install_session(io, ordered, backup_len, expect_version, do_write, 0,
                             backup_write, backup_read, backup_ctx, result);
        s_fallback = 1;
    }
    result->stub_fallback = s_fallback;
    result->nvs_written = 0;
    s_layout_write = 0;
    return rc;
}
