#ifndef HD2_ROMFLASH_H
#define HD2_ROMFLASH_H
/*
 * ESP32-S3 serial protocol for one app-slot write.
 * v1.2.3: after SYNC, the esptool flasher stub (esp-flasher-stub v0.2.0,
 * Apache-2.0 OR MIT) is loaded into RAM for fast reads, compressed writes and
 * on-chip MD5. If the stub cannot be started, the v1.2.2 ROM-only path runs.
 * There is no erase-chip operation in this file. The only flash offset
 * it will program is the cp7 factory app at 0x10000.
 *
 * A power loss or a USB disconnect during the write is not atomic and is
 * not guaranteed to be recoverable. Callers must keep the backup bytes.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct rom_io {
    void *ctx;
    int (*write)(void *ctx, const uint8_t *data, size_t n);          /* 0 ok */
    int (*read)(void *ctx, uint8_t *data, size_t n, int timeout_ms); /* bytes, 0 timeout, -1 error */
    int (*set_baud)(void *ctx, uint32_t baud);                       /* 0 ok */
    void (*set_dtr_rts)(void *ctx, int dtr, int rts);
    void (*flush_rx)(void *ctx);
    void (*sleep_ms)(void *ctx, unsigned ms);
    void (*progress)(void *ctx, const char *phase, unsigned done, unsigned total);
    int (*cancelled)(void *ctx);
    /* Optional. Close and re-open the COM port after a reset. 0 ok.
     * Must not be called between a download-mode pulse and the following SYNC. */
    int (*reopen)(void *ctx);
    /* Optional. One already-formatted protocol trace line, no newline. */
    void (*log)(void *ctx, const char *msg);
    /* Optional (v1.2.3). Monotonic milliseconds, for per-phase timing in the log. */
    unsigned (*now_ms)(void *ctx);
    /* Optional (v1.2.3). Look for a local copy of the board's current app:
     * exactly len bytes whose MD5 equals md5. Fill out and return 0 if found.
     * romflash re-checks the MD5 of whatever is returned. */
    int (*find_backup)(void *ctx, uint32_t len, const uint8_t md5[16], uint8_t *out);
    /* v1.2.3. 1 = always read the full backup from the board, never a local copy. */
    int full_backup;
    /* v1.2.3. 1 = do not try the flasher stub; use only the v1.2.2 ROM path. */
    int rom_only;
} rom_io;

typedef struct {
    char error[384];
    char device_version[32];
    int wrote;       /* 1 if FLASH_BEGIN for the new image was accepted */
    int restored;    /* 1 if a failed write was replaced with the backup */
    int identified;  /* 1 if the cp7 layout and a cp7 version were read */
    int verify_ran;  /* 1 if the new image was actually read back and compared */
    int verified;    /* 1 if that comparison matched */
    int confirmed;   /* 1 if the version was re-read after reset */
    char confirmed_version[32];
    int used_stub;      /* 1 if the write session ran on the esptool flasher stub */
    int stub_fallback;  /* 1 if the stub failed and the v1.2.2 ROM path was used */
    char backup_source[64]; /* where the backup bytes came from */
    unsigned ms_connect, ms_backup, ms_write, ms_verify, ms_confirm; /* phase durations, if now_ms is set */
} rom_result;

typedef struct {
    char error[200];
    int chip_id;       /* 9 = ESP32-S3 */
    int stub;          /* 1 if the flasher stub was running */
    int stub_fallback; /* 1 if the stub failed and ROM was used */
    unsigned baud;
    int layout_cp7;
    uint32_t app_len;
    char version[32];
    char app_md5[33];
    unsigned ms_connect, ms_md5;
} rom_probe_result;

/* Read-only: connect, load the stub, read layout, app length, version and the
 * MD5 of 0x10000 over the app length, then hard-reset back to the app. */
int rom_probe(rom_io *io, rom_probe_result *pr);

/* Whole-chip size of the CrowPanel Advance 7.0 module (16 MB). */
#define ROM_FLASH_SIZE 0x01000000u

/* One image in a factory first-install. Allowed offsets are only:
 * bootloader at 0x0, the cp7 partition table at 0x8000, and the factory
 * app at 0x10000. NVS, phy_init, and the assets partition are not in this
 * list and are refused. */
typedef struct {
    uint32_t offset;
    const uint8_t *data;
    uint32_t len;
    const char *name;
} rom_install_image;

typedef struct {
    char error[384];
    int backed_up;     /* 1 if every backup byte was handed to backup_write */
    uint32_t backup_bytes;
    int wrote;         /* 1 if any region write was accepted */
    int restored;      /* 1 if every started region was written back from the backup and verified */
    int verify_ran;
    int verified;
    int confirmed;
    char confirmed_version[32];
    int used_stub;
    int stub_fallback;
    int nvs_written;   /* always 0: this path never programs 0x9000 */
    unsigned ms_connect, ms_backup, ms_write, ms_verify, ms_confirm;
} rom_install_result;

/* Factory first-install. Not an app-only update.
 * Reads backup_len bytes from address 0 and gives them to backup_write
 * before any erase or program. backup_write is called with increasing
 * offsets; offset 0 starts the file over (a stub failure retries from 0).
 * backup_read is required. It re-checks the saved head and tail before any
 * program, and it supplies the bytes for a rollback.
 * do_write 0: backup, then reset. No FLASH_BEGIN and no FLASH_DEFL.
 * On a failed write, verify, or version confirm, regions that were started
 * are restored from the backup. NVS is not written on the way in or back.
 * There is no chip-erase command on this path.
 * A stub I/O failure before the first program logs FALLBACK and runs the
 * same steps on the ROM loader. */
int rom_first_install(rom_io *io, const rom_install_image *images, int nimages,
                      uint32_t backup_len, const char *expect_version, int do_write,
                      int (*backup_write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t n),
                      int (*backup_read)(void *ctx, uint32_t offset, uint8_t *dst, uint32_t n),
                      void *backup_ctx, rom_install_result *result);

/*
 * do_write 0: enter the bootloader, check the chip and partition table,
 * read the running version, then reset back to the app. No FLASH_BEGIN.
 * do_write 1: also back up the running app, write the new image, and read
 * it back. allow_same permits rewriting the same version (test reinstall).
 * backup_write is called once with the complete old image before any write.
 *
 * A mid-write failure with a live bootloader session may roll the old app
 * back. If the write finished but a later verify command gets no reply,
 * rollback is not attempted: the new image may already be on the chip.
 */
int rom_update_app(rom_io *io, const uint8_t *image, uint32_t image_len,
                   const char *package_version, int allow_same, int do_write,
                   int (*backup_write)(void *ctx, const uint8_t *data, size_t n),
                   void *backup_ctx, rom_result *result);
#endif
