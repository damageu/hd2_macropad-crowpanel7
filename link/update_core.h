#ifndef HD2_UPDATE_CORE_H
#define HD2_UPDATE_CORE_H
/*
 * Portable checks for the CrowPanel app-only updater.
 * A manifest cannot name a write address, a baud rate, a command, or a URL.
 * Those are fixed here. Stable downloads are built only from the public
 * GitHub release URL of damageu/hd2_macropad-crowpanel7.
 */
#include <stddef.h>
#include <stdint.h>

#define UPD_APP_OFFSET     0x10000u
#define UPD_APP_SLOT_END   0xA10000u
#define UPD_PART_TABLE     0x8000u
#define UPD_PART_TABLE_LEN 0xC00u
#define UPD_NVS_OFFSET     0x9000u
#define UPD_NVS_SIZE       0x6000u
#define UPD_ASSETS_OFFSET  0xA10000u
#define UPD_ASSETS_SIZE    0x400000u
#define UPD_FACTORY_SIZE   0xA00000u
#define UPD_FLASH_BAUD_MAX 460800u
#define UPD_LINK_BAUD      115200u
#define UPD_CHIP_ID_S3     9
#define UPD_IMAGE_MAGIC    0xE9
#define UPD_APP_DESC_MAGIC 0xABCD5432u

#define UPD_PRODUCT "hd2-macropad-crowpanel7"
#define UPD_CHIP    "esp32s3"
#define UPD_REPO    "damageu/hd2_macropad-crowpanel7"
#define UPD_MANIFEST_NAME "crowpanel-update.json"

typedef struct {
    int major, minor, patch, cp;
} upd_ver;

typedef struct {
    int schema;
    char channel[16];
    char version[32];
    char product[48];
    char chip[16];
    uint32_t app_offset;
    uint32_t app_slot_end;
    char image_name[80];
    char sha256_hex[65];
    uint32_t image_size;
    char notes[2001];
} upd_manifest;

typedef struct {
    char tag[32];
    char manifest_name[80];
} upd_release_pick;

enum {
    UPD_ALLOW = 0,
    UPD_SAME = 1,
    UPD_DOWNGRADE = 2,
    UPD_NO_DEVICE_VER = 3
};

int upd_ver_parse(const char *s, upd_ver *out);
int upd_ver_cmp(upd_ver a, upd_ver b);

/* Strict schema-1 object. Unknown keys, commands, and URLs are rejected. */
int upd_manifest_parse(const char *json, size_t n, upd_manifest *out, char *err, size_t err_cap);

/* channel is "stable" or "test". */
int upd_manifest_constants_ok(const upd_manifest *m, const char *channel, char *err, size_t err_cap);

/* Filename only. Version inside the name must equal m->version. */
int upd_image_name_ok(const upd_manifest *m, char *err, size_t err_cap);

int upd_sha256_file_ok(const upd_manifest *m, const uint8_t *image, size_t n, char *err, size_t err_cap);

/* Walk an ESP32-S3 image. On success *total is the file length that must be flashed. */
int upd_image_length(const uint8_t *image, size_t n, uint32_t *total, char *err, size_t err_cap);

/* Exactly one vX.Y.Z-cp7.N string, copied to ver_out. */
int upd_image_version(const uint8_t *image, size_t n, char *ver_out, size_t ver_cap, char *err, size_t err_cap);

/* Stable GitHub releases document. Picks the newest non-draft, non-prerelease
 * tag that publishes crowpanel-update.json at the canonical release URL.
 * Does not follow or return any other URL. */
int upd_pick_stable_release(const char *json, size_t n, upd_release_pick *out, char *err, size_t err_cap);

/* Exact release asset URL. tag and filename are checked again. */
int upd_release_url(const char *tag, const char *filename, char *out, size_t out_cap);

/* Host of an https URL after a redirect. Rejects userinfo and odd ports. */
int upd_https_host_ok(const char *url);

int upd_partition_is_cp7(const uint8_t *table, size_t n, char *err, size_t err_cap);

/* 1 if [offset, offset+len) is entirely inside the factory app slot. */
int upd_range_is_app_slot(uint32_t offset, uint32_t len);

/* Compare a device version string with the package version.
 * allow_same is only for an explicit test-package reinstall. */
int upd_decide(const char *device_ver, const upd_manifest *pkg, int allow_same, char *err, size_t err_cap);

/* 1 if release_ver is a published cp7 package that should be offered: newer than
 * device_ver when that is known, and not equal to skipped_ver. An empty
 * device_ver still offers the release once, until it is skipped. */
int upd_should_offer(const char *device_ver, const char *release_ver, const char *skipped_ver);
#endif
