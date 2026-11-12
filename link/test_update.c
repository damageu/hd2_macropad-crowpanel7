#include "update_core.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static const char *MAN =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"channel\": \"test\",\n"
    "  \"version\": \"v1.9.1-cp7.2\",\n"
    "  \"product\": \"hd2-macropad-crowpanel7\",\n"
    "  \"chip\": \"esp32s3\",\n"
    "  \"app_offset\": 65536,\n"
    "  \"app_slot_end\": 10551296,\n"
    "  \"image_name\": \"hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin\",\n"
    "  \"image_sha256\": \"%s\",\n"
    "  \"image_size\": %u,\n"
    "  \"notes\": \"Local test package. Not a published release.\\n\"\n"
    "}\n";

int main(void)
{
    char err[160], hex[65], url[256];
    upd_manifest m;
    upd_ver a, b;
    uint8_t dig[32];
    char body[1200];

    expect(upd_ver_parse("v1.8.6-cp7.2", &a), "parse old");
    expect(upd_ver_parse("v1.9.1-cp7.2", &b), "parse new");
    expect(upd_ver_cmp(b, a) > 0, "1.9.1 is newer than 1.8.6");
    expect(!upd_ver_parse("v1.9.1", &a), "reject bare semver");
    expect(!upd_ver_parse("v1.9.1-cp7.2-extra", &a), "reject suffix");
    expect(!upd_ver_parse("v1.9.1-2-g689c67b-dirty", &a), "reject git describe");

    sha256_buf("abc", 3, dig);
    sha256_hex("abc", 3, hex, sizeof hex);
    expect(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 abc");
    expect(sha256_hex_eq(hex, dig), "hex eq");

    snprintf(body, sizeof body, MAN, hex, 3u);
    expect(upd_manifest_parse(body, strlen(body), &m, err, sizeof err), err);
    expect(upd_manifest_constants_ok(&m, "test", err, sizeof err), err);
    expect(!upd_manifest_constants_ok(&m, "stable", err, sizeof err), "test manifest is not stable");
    expect(upd_image_name_ok(&m, err, sizeof err), err);
    expect(upd_sha256_file_ok(&m, (const uint8_t *)"abc", 3, err, sizeof err), err);
    expect(!upd_sha256_file_ok(&m, (const uint8_t *)"abd", 3, err, sizeof err), "bad hash rejected");

    expect(!upd_manifest_parse("{\"schema\":1,\"command\":\"erase\"}", 32, &m, err, sizeof err), "unknown field");
    snprintf(body, sizeof body, MAN, hex, 3u);
    expect(upd_manifest_parse(body, strlen(body), &m, err, sizeof err), "reparse");
    m.app_offset = 0;
    expect(!upd_manifest_constants_ok(&m, "test", err, sizeof err), "offset 0 rejected");

    strcpy(m.image_name, "../hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin");
    m.app_offset = UPD_APP_OFFSET;
    expect(!upd_image_name_ok(&m, err, sizeof err), "path rejected");

    expect(upd_decide("v1.8.6-cp7.2", &m, 0, err, sizeof err) == UPD_ALLOW, "upgrade allowed");
    /* m.version is still v1.9.1-cp7.2 */
    expect(upd_decide("v1.9.1-cp7.2", &m, 0, err, sizeof err) == UPD_SAME, "same blocked");
    expect(upd_decide("v1.9.1-cp7.2", &m, 1, err, sizeof err) == UPD_SAME, "same allowed only when asked");
    expect(upd_decide("v1.9.2-cp7.2", &m, 1, err, sizeof err) == UPD_DOWNGRADE, "downgrade blocked even for test");
    expect(upd_decide("not-cp7", &m, 1, err, sizeof err) == UPD_NO_DEVICE_VER, "unknown board blocked");

    expect(upd_release_url("v1.9.1-cp7.2", "crowpanel-update.json", url, sizeof url), "url build");
    expect(strcmp(url, "https://github.com/damageu/hd2_macropad-crowpanel7/releases/download/v1.9.1-cp7.2/crowpanel-update.json") == 0, "canonical url");
    expect(!upd_release_url("v1.9.1-cp7.2", "../x", url, sizeof url), "bad filename");
    expect(upd_https_host_ok("https://github.com/damageu/hd2_macropad-crowpanel7/releases/download/v1.9.1-cp7.2/a.bin"), "github host");
    expect(upd_https_host_ok("https://release-assets.githubusercontent.com/abc"), "asset host");
    expect(upd_https_host_ok("https://objects.githubusercontent.com/abc"), "objects host");
    expect(!upd_https_host_ok("https://evil.example/a.bin"), "evil host");
    expect(!upd_https_host_ok("http://github.com/a"), "http rejected");
    expect(!upd_https_host_ok("https://user:pass@github.com/a"), "userinfo rejected");
    expect(!upd_https_host_ok("https://github.com.evil.example/a"), "suffix host rejected");

    {
        const char *list =
            "["
            "{\"tag_name\":\"v9.0.0-cp7.9\",\"draft\":false,\"prerelease\":true,\"assets\":["
            "{\"name\":\"crowpanel-update.json\",\"browser_download_url\":\"https://github.com/damageu/hd2_macropad-crowpanel7/releases/download/v9.0.0-cp7.9/crowpanel-update.json\"}"
            "]},"
            "{\"tag_name\":\"v1.9.1-cp7.2\",\"draft\":false,\"prerelease\":false,\"name\":\"ignored\","
            "\"assets\":["
            "{\"name\":\"crowpanel-update.json\",\"browser_download_url\":\"https://evil.example/crowpanel-update.json\"},"
            "{\"name\":\"crowpanel-update.json\",\"browser_download_url\":\"https://github.com/damageu/hd2_macropad-crowpanel7/releases/download/v1.9.1-cp7.2/crowpanel-update.json\"}"
            "]}"
            "]";
        upd_release_pick pick;
        expect(upd_pick_stable_release(list, strlen(list), &pick, err, sizeof err), err);
        expect(strcmp(pick.tag, "v1.9.1-cp7.2") == 0, "skipped prerelease and evil url");
    }
    {
        const char *list =
            "[{\"tag_name\":\"v1.9.1-cp7.2\",\"draft\":false,\"prerelease\":false,\"assets\":["
            "{\"name\":\"crowpanel-update.json\",\"browser_download_url\":\"https://evil.example/crowpanel-update.json\"}"
            "]}]";
        upd_release_pick pick;
        expect(!upd_pick_stable_release(list, strlen(list), &pick, err, sizeof err), "evil-only asset rejected");
    }

    {
        uint8_t table[96];
        memset(table, 0xFF, sizeof table);
        memset(table, 0, 96);
        table[0] = 0xAA; table[1] = 0x50; table[2] = 0x01; table[3] = 0x02;
        table[4] = 0x00; table[5] = 0x90; /* 0x9000 little endian */
        table[8] = 0x00; table[9] = 0x60;
        memcpy(table + 12, "nvs", 3);
        table[32] = 0xAA; table[33] = 0x50; table[34] = 0x00;
        table[36] = 0x00; table[37] = 0x00; table[38] = 0x01; /* 0x10000 */
        table[40] = 0x00; table[41] = 0x00; table[42] = 0xA0; /* 0xA00000 */
        memcpy(table + 44, "factory", 7);
        table[64] = 0xAA; table[65] = 0x50; table[66] = 0x01; table[67] = 0x81;
        table[68] = 0x00; table[69] = 0x00; table[70] = 0xA1; /* 0xA10000 */
        table[72] = 0x00; table[73] = 0x00; table[74] = 0x40; /* 0x400000 */
        memcpy(table + 76, "assets", 6);
        table[95] = 0xFF;
        expect(upd_partition_is_cp7(table, sizeof table, err, sizeof err), err);
        expect(UPD_ASSETS_OFFSET == 0xA10000u && UPD_ASSETS_SIZE == 0x400000u, "assets slot is 4 MiB at 0xA10000");
        expect(!upd_range_is_app_slot(UPD_ASSETS_OFFSET, 0x400000u), "assets is not an app-only write");
        table[36] = 0x00; table[37] = 0x00; table[38] = 0x02;
        expect(!upd_partition_is_cp7(table, sizeof table, err, sizeof err), "wrong factory offset");
    }
    expect(upd_range_is_app_slot(0x10000, 100), "app range");
    expect(!upd_range_is_app_slot(0, 100), "bootloader range");
    expect(!upd_range_is_app_slot(0x9000, 100), "nvs range");
    expect(upd_range_is_app_slot(0x10000, UPD_FACTORY_SIZE), "full factory slot");
    expect(!upd_range_is_app_slot(0x10000, UPD_FACTORY_SIZE + 1), "past assets");

    expect(upd_should_offer("v1.8.6-cp7.2", "v1.9.1-cp7.2", NULL), "newer than board");
    expect(!upd_should_offer("v1.9.1-cp7.2", "v1.9.1-cp7.2", NULL), "same as board");
    expect(!upd_should_offer("v1.9.1-cp7.2", "v1.8.6-cp7.2", NULL), "older than board");
    expect(upd_should_offer("", "v1.9.1-cp7.2", NULL), "unknown board still offers");
    expect(upd_should_offer(NULL, "v1.9.1-cp7.2", ""), "null board still offers");
    expect(!upd_should_offer("", "v1.9.1-cp7.2", "v1.9.1-cp7.2"), "skipped this version");
    expect(upd_should_offer("v1.8.6-cp7.2", "v1.9.2-cp7.2", "v1.9.1-cp7.2"), "newer than skipped");
    expect(!upd_should_offer("", "v1.9.1", NULL), "reject non-cp7 release tag");

    if (g_fail) {
        fprintf(stderr, "%d failures\n", g_fail);
        return 1;
    }
    puts("test_update ok");
    return 0;
}
