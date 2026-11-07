#ifndef HD2_ROMFLASH_H
#define HD2_ROMFLASH_H
/*
 * ESP32-S3 ROM serial protocol for one app-slot write.
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
} rom_io;

typedef struct {
    char error[384];
    char device_version[32];
    int wrote;       /* 1 if FLASH_BEGIN was sent */
    int restored;    /* 1 if a failed write was replaced with the backup */
    int identified;  /* 1 if the cp7 layout and a cp7 version were read */
} rom_result;

/*
 * do_write 0: enter the bootloader, check the chip and partition table,
 * read the running version, then reset back to the app. No FLASH_BEGIN.
 * do_write 1: also back up the running app, write the new image, and read
 * it back. allow_same permits rewriting the same version (test reinstall).
 * backup_write is called once with the complete old image before any write.
 */
int rom_update_app(rom_io *io, const uint8_t *image, uint32_t image_len,
                   const char *package_version, int allow_same, int do_write,
                   int (*backup_write)(void *ctx, const uint8_t *data, size_t n),
                   void *backup_ctx, rom_result *result);
#endif
