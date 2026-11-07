#include "update_core.h"
#include "sha256.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg)
{
    if (!err || cap == 0) return;
    snprintf(err, cap, "%s", msg);
}

int upd_ver_parse(const char *s, upd_ver *out)
{
    int maj = 0, min = 0, pat = 0, cp = 0;
    int n = 0;
    if (!s || s[0] != 'v') return 0;
    if (sscanf(s, "v%d.%d.%d-cp7.%d%n", &maj, &min, &pat, &cp, &n) != 4) return 0;
    if (n <= 0 || s[n] != 0) return 0;
    if (maj < 0 || min < 0 || pat < 0 || cp < 0) return 0;
    if (maj > 255 || min > 255 || pat > 255 || cp > 255) return 0;
    if (out) {
        out->major = maj;
        out->minor = min;
        out->patch = pat;
        out->cp = cp;
    }
    return 1;
}

int upd_ver_cmp(upd_ver a, upd_ver b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.cp != b.cp) return a.cp < b.cp ? -1 : 1;
    return 0;
}

/* ---------------------------------------------------------------- JSON */

typedef struct {
    const char *p, *end;
    char err[80];
} js_t;

static void js_skip_ws(js_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static int js_hex4(js_t *j, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (j->p >= j->end) return 0;
        char c = *j->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v += (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v += (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v += (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static int js_string(js_t *j, char *buf, size_t cap)
{
    js_skip_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        snprintf(j->err, sizeof j->err, "expected string");
        return 0;
    }
    j->p++;
    size_t n = 0;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') {
            if (n >= cap) {
                snprintf(j->err, sizeof j->err, "string too long");
                return 0;
            }
            buf[n] = 0;
            return 1;
        }
        if (c == '\\') {
            if (j->p >= j->end) break;
            char e = *j->p++;
            if (e == '"' || e == '\\' || e == '/') c = (unsigned char)e;
            else if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u') {
                unsigned u;
                if (!js_hex4(j, &u) || u > 0x7f) {
                    snprintf(j->err, sizeof j->err, "unsupported escape");
                    return 0;
                }
                c = (unsigned char)u;
            } else {
                snprintf(j->err, sizeof j->err, "bad escape");
                return 0;
            }
        } else if (c < 0x20) {
            snprintf(j->err, sizeof j->err, "control character in string");
            return 0;
        }
        if (n + 1 >= cap) {
            snprintf(j->err, sizeof j->err, "string too long");
            return 0;
        }
        buf[n++] = (char)c;
    }
    snprintf(j->err, sizeof j->err, "unterminated string");
    return 0;
}

static int js_skip(js_t *j, int depth);

static int js_skip_string_raw(js_t *j)
{
    char dump[8];
    /* Consume a string without storing it, still enforcing escape rules. */
    js_skip_ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') return 1;
        if (c == '\\') {
            if (j->p >= j->end) return 0;
            char e = *j->p++;
            if (e == 'u') {
                unsigned u;
                if (!js_hex4(j, &u)) return 0;
                (void)u;
            } else if (e != '"' && e != '\\' && e != '/' && e != 'n' && e != 'r' && e != 't' && e != 'b' && e != 'f') {
                return 0;
            }
        } else if (c < 0x20) {
            return 0;
        }
        (void)dump;
    }
    return 0;
}

static int js_skip(js_t *j, int depth)
{
    if (depth > 8) return 0;
    js_skip_ws(j);
    if (j->p >= j->end) return 0;
    char c = *j->p;
    if (c == '"') return js_skip_string_raw(j);
    if (c == '{') {
        j->p++;
        js_skip_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return 1; }
        for (;;) {
            if (!js_skip_string_raw(j)) return 0;
            js_skip_ws(j);
            if (j->p >= j->end || *j->p != ':') return 0;
            j->p++;
            if (!js_skip(j, depth + 1)) return 0;
            js_skip_ws(j);
            if (j->p >= j->end) return 0;
            if (*j->p == '}') { j->p++; return 1; }
            if (*j->p != ',') return 0;
            j->p++;
        }
    }
    if (c == '[') {
        j->p++;
        js_skip_ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return 1; }
        for (;;) {
            if (!js_skip(j, depth + 1)) return 0;
            js_skip_ws(j);
            if (j->p >= j->end) return 0;
            if (*j->p == ']') { j->p++; return 1; }
            if (*j->p != ',') return 0;
            j->p++;
        }
    }
    if (c == 't' && j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) { j->p += 4; return 1; }
    if (c == 'f' && j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) { j->p += 5; return 1; }
    if (c == 'n' && j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) { j->p += 4; return 1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-') j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') return 0;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
        if (j->p < j->end && *j->p == '.') return 0;
        if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) return 0;
        return 1;
    }
    return 0;
}

static int js_u32(js_t *j, uint32_t *out)
{
    js_skip_ws(j);
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
        snprintf(j->err, sizeof j->err, "expected number");
        return 0;
    }
    uint64_t v = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        v = v * 10u + (uint64_t)(*j->p - '0');
        if (v > 0xffffffffu) {
            snprintf(j->err, sizeof j->err, "number too large");
            return 0;
        }
        j->p++;
    }
    if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) {
        snprintf(j->err, sizeof j->err, "integers only");
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

static int js_bool(js_t *j, int *out)
{
    js_skip_ws(j);
    if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) { j->p += 4; *out = 1; return 1; }
    if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) { j->p += 5; *out = 0; return 1; }
    snprintf(j->err, sizeof j->err, "expected boolean");
    return 0;
}

static int key_is(const char *k, const char *want) { return strcmp(k, want) == 0; }

int upd_manifest_parse(const char *json, size_t n, upd_manifest *out, char *err, size_t err_cap)
{
    if (!json || !out || n == 0 || n > 16384) {
        set_err(err, err_cap, "manifest is missing or too large");
        return 0;
    }
    memset(out, 0, sizeof *out);
    js_t j = {json, json + n, {0}};
    int seen[16] = {0};
    js_skip_ws(&j);
    if (j.p >= j.end || *j.p != '{') {
        set_err(err, err_cap, "manifest must be one JSON object");
        return 0;
    }
    j.p++;
    js_skip_ws(&j);
    if (j.p < j.end && *j.p == '}') {
        set_err(err, err_cap, "manifest is empty");
        return 0;
    }
    while (j.p < j.end) {
        char key[32];
        if (!js_string(&j, key, sizeof key)) {
            set_err(err, err_cap, j.err[0] ? j.err : "bad manifest key");
            return 0;
        }
        js_skip_ws(&j);
        if (j.p >= j.end || *j.p != ':') {
            set_err(err, err_cap, "manifest is missing a colon");
            return 0;
        }
        j.p++;
        int id = -1;
        if (key_is(key, "schema")) id = 0;
        else if (key_is(key, "channel")) id = 1;
        else if (key_is(key, "version")) id = 2;
        else if (key_is(key, "product")) id = 3;
        else if (key_is(key, "chip")) id = 4;
        else if (key_is(key, "app_offset")) id = 5;
        else if (key_is(key, "app_slot_end")) id = 6;
        else if (key_is(key, "image_name")) id = 7;
        else if (key_is(key, "image_sha256")) id = 8;
        else if (key_is(key, "image_size")) id = 9;
        else if (key_is(key, "notes")) id = 10;
        else {
            set_err(err, err_cap, "manifest has an unknown field");
            return 0;
        }
        if (seen[id]) {
            set_err(err, err_cap, "manifest repeats a field");
            return 0;
        }
        seen[id] = 1;
        if (id == 0) {
            uint32_t v;
            if (!js_u32(&j, &v) || v != 1) {
                set_err(err, err_cap, "manifest schema must be 1");
                return 0;
            }
            out->schema = 1;
        } else if (id == 1) {
            if (!js_string(&j, out->channel, sizeof out->channel)) goto badstr;
        } else if (id == 2) {
            if (!js_string(&j, out->version, sizeof out->version)) goto badstr;
        } else if (id == 3) {
            if (!js_string(&j, out->product, sizeof out->product)) goto badstr;
        } else if (id == 4) {
            if (!js_string(&j, out->chip, sizeof out->chip)) goto badstr;
        } else if (id == 5) {
            if (!js_u32(&j, &out->app_offset)) goto badnum;
        } else if (id == 6) {
            if (!js_u32(&j, &out->app_slot_end)) goto badnum;
        } else if (id == 7) {
            if (!js_string(&j, out->image_name, sizeof out->image_name)) goto badstr;
        } else if (id == 8) {
            if (!js_string(&j, out->sha256_hex, sizeof out->sha256_hex)) goto badstr;
        } else if (id == 9) {
            if (!js_u32(&j, &out->image_size) || out->image_size == 0) {
                set_err(err, err_cap, "image_size is invalid");
                return 0;
            }
        } else if (id == 10) {
            if (!js_string(&j, out->notes, sizeof out->notes)) goto badstr;
        }
        js_skip_ws(&j);
        if (j.p < j.end && *j.p == ',') { j.p++; continue; }
        if (j.p < j.end && *j.p == '}') { j.p++; break; }
        set_err(err, err_cap, "manifest object did not end cleanly");
        return 0;
    }
    js_skip_ws(&j);
    if (j.p != j.end) {
        set_err(err, err_cap, "extra data after the manifest");
        return 0;
    }
    for (int i = 0; i <= 10; i++) {
        if (!seen[i]) {
            set_err(err, err_cap, "manifest is missing a required field");
            return 0;
        }
    }
    return 1;
badstr:
    set_err(err, err_cap, j.err[0] ? j.err : "bad manifest string");
    return 0;
badnum:
    set_err(err, err_cap, j.err[0] ? j.err : "bad manifest number");
    return 0;
}

int upd_manifest_constants_ok(const upd_manifest *m, const char *channel, char *err, size_t err_cap)
{
    upd_ver v;
    if (!m || m->schema != 1) {
        set_err(err, err_cap, "unsupported manifest schema");
        return 0;
    }
    if (!channel || strcmp(m->channel, channel) != 0) {
        set_err(err, err_cap, "wrong update channel for this action");
        return 0;
    }
    if (strcmp(m->product, UPD_PRODUCT) != 0 || strcmp(m->chip, UPD_CHIP) != 0) {
        set_err(err, err_cap, "package is not the CrowPanel ESP32-S3 firmware");
        return 0;
    }
    if (m->app_offset != UPD_APP_OFFSET || m->app_slot_end != UPD_APP_SLOT_END) {
        set_err(err, err_cap, "package does not target the cp7 app slot");
        return 0;
    }
    if (!upd_ver_parse(m->version, &v)) {
        set_err(err, err_cap, "version must look like v1.9.1-cp7.2");
        return 0;
    }
    if (strlen(m->sha256_hex) != 64) {
        set_err(err, err_cap, "image_sha256 must be 64 hex characters");
        return 0;
    }
    for (const char *p = m->sha256_hex; *p; p++) {
        if (!isxdigit((unsigned char)*p)) {
            set_err(err, err_cap, "image_sha256 must be hex");
            return 0;
        }
    }
    if (m->image_size > UPD_FACTORY_SIZE) {
        set_err(err, err_cap, "image is larger than the factory slot");
        return 0;
    }
    return 1;
}

int upd_image_name_ok(const upd_manifest *m, char *err, size_t err_cap)
{
    char expect[80];
    const char *n = m->image_name;
    if (strchr(n, '/') || strchr(n, '\\') || strchr(n, ':') || strstr(n, "..")) {
        set_err(err, err_cap, "image name must be a file name, not a path");
        return 0;
    }
    snprintf(expect, sizeof expect, "hd2_macropad_%s_app_0x10000.bin", m->version);
    if (strcmp(n, expect) != 0) {
        set_err(err, err_cap, "image name does not match the manifest version");
        return 0;
    }
    return 1;
}

int upd_sha256_file_ok(const upd_manifest *m, const uint8_t *image, size_t n, char *err, size_t err_cap)
{
    uint8_t dig[32];
    if (!image || n != m->image_size) {
        set_err(err, err_cap, "image size does not match the manifest");
        return 0;
    }
    sha256_buf(image, n, dig);
    if (!sha256_hex_eq(m->sha256_hex, dig)) {
        set_err(err, err_cap, "image sha256 does not match the manifest");
        return 0;
    }
    return 1;
}

int upd_image_length(const uint8_t *image, size_t n, uint32_t *total, char *err, size_t err_cap)
{
    if (!image || n < 24 || image[0] != UPD_IMAGE_MAGIC) {
        set_err(err, err_cap, "not an ESP32 application image");
        return 0;
    }
    unsigned segs = image[1];
    if (segs == 0 || segs > 16) {
        set_err(err, err_cap, "application image has a bad segment count");
        return 0;
    }
    uint16_t chip = (uint16_t)(image[12] | (image[13] << 8));
    if (chip != UPD_CHIP_ID_S3) {
        set_err(err, err_cap, "application image is not for the ESP32-S3");
        return 0;
    }
    int hash = image[23] == 1;
    size_t pos = 24;
    for (unsigned i = 0; i < segs; i++) {
        if (pos + 8 > n) {
            set_err(err, err_cap, "application image is truncated");
            return 0;
        }
        uint32_t sz = (uint32_t)image[pos + 4] | ((uint32_t)image[pos + 5] << 8) |
                      ((uint32_t)image[pos + 6] << 16) | ((uint32_t)image[pos + 7] << 24);
        if (sz > UPD_FACTORY_SIZE || pos + 8 + sz > n) {
            set_err(err, err_cap, "application segment does not fit the file");
            return 0;
        }
        pos += 8 + sz;
    }
    uint32_t padded = ((uint32_t)pos + 16u) & ~15u;
    uint32_t end = padded + (hash ? 32u : 0u);
    if (end != n) {
        set_err(err, err_cap, "application image length is not the whole file");
        return 0;
    }
    if (total) *total = end;
    return 1;
}

static int is_ver_boundary(unsigned char c)
{
    return !(isalnum(c) || c == '.' || c == '-' || c == '_');
}

int upd_image_version(const uint8_t *image, size_t n, char *ver_out, size_t ver_cap, char *err, size_t err_cap)
{
    char found[32] = {0};
    int count = 0;
    if (!image) {
        set_err(err, err_cap, "no image");
        return 0;
    }
    for (size_t i = 0; i + 8 < n; i++) {
        if (image[i] != 'v') continue;
        if (i > 0 && !is_ver_boundary(image[i - 1])) continue;
        char tmp[32];
        size_t k = 0;
        while (i + k < n && k + 1 < sizeof tmp && !is_ver_boundary(image[i + k])) {
            tmp[k] = (char)image[i + k];
            k++;
        }
        tmp[k] = 0;
        upd_ver v;
        if (!upd_ver_parse(tmp, &v)) continue;
        if (count == 0) {
            memcpy(found, tmp, k + 1);
            count = 1;
        } else if (strcmp(found, tmp) != 0) {
            set_err(err, err_cap, "image contains more than one cp7 version");
            return 0;
        }
    }
    if (count != 1) {
        set_err(err, err_cap, "image has no vX.Y.Z-cp7.N version");
        return 0;
    }
    if (strlen(found) + 1 > ver_cap) {
        set_err(err, err_cap, "version does not fit");
        return 0;
    }
    memcpy(ver_out, found, strlen(found) + 1);
    return 1;
}

static int canonical_asset_url(const char *tag, const char *name, const char *url)
{
    char built[256];
    if (!upd_release_url(tag, name, built, sizeof built)) return 0;
    return strcmp(built, url) == 0;
}

int upd_release_url(const char *tag, const char *filename, char *out, size_t out_cap)
{
    upd_ver v;
    if (!upd_ver_parse(tag, &v) || !filename || !filename[0] || strchr(filename, '/') || strchr(filename, '\\'))
        return 0;
    int n = snprintf(out, out_cap,
                     "https://github.com/%s/releases/download/%s/%s",
                     UPD_REPO, tag, filename);
    return n > 0 && (size_t)n < out_cap;
}

int upd_https_host_ok(const char *url)
{
    static const char *ok[] = {
        "github.com",
        "release-assets.githubusercontent.com",
        "objects.githubusercontent.com",
        "github-releases.githubusercontent.com",
        "api.github.com"
    };
    const char *p;
    char host[128];
    size_t hlen;
    if (!url || strncmp(url, "https://", 8) != 0) return 0;
    p = url + 8;
    if (strchr(p, '@')) return 0;
    hlen = 0;
    while (p[hlen] && p[hlen] != '/' && p[hlen] != ':' && p[hlen] != '?' && hlen + 1 < sizeof host) {
        host[hlen] = (char)tolower((unsigned char)p[hlen]);
        hlen++;
    }
    host[hlen] = 0;
    if (p[hlen] == ':') {
        if (strncmp(p + hlen, ":443", 4) != 0) return 0;
        if (p[hlen + 4] != '/' && p[hlen + 4] != 0 && p[hlen + 4] != '?') return 0;
    }
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (strcmp(host, ok[i]) == 0) return 1;
    }
    return 0;
}

int upd_pick_stable_release(const char *json, size_t n, upd_release_pick *out, char *err, size_t err_cap)
{
    if (!json || n == 0 || n > (1u << 20)) {
        set_err(err, err_cap, "GitHub release list is missing or too large");
        return 0;
    }
    js_t j = {json, json + n, {0}};
    js_skip_ws(&j);
    if (j.p >= j.end || *j.p != '[') {
        set_err(err, err_cap, "GitHub release list was not an array");
        return 0;
    }
    j.p++;
    while (j.p < j.end) {
        char tag[40] = {0};
        char manifest_urls[4][256];
        int n_manifest_urls = 0;
        int draft = 0, pre = 0, saw_draft = 0, saw_pre = 0, saw_tag = 0;
        memset(manifest_urls, 0, sizeof manifest_urls);
        js_skip_ws(&j);
        if (j.p < j.end && *j.p == ']') {
            set_err(err, err_cap, "no stable CrowPanel release is published");
            return 0;
        }
        if (j.p >= j.end || *j.p != '{') {
            set_err(err, err_cap, "bad release list");
            return 0;
        }
        j.p++;
        while (j.p < j.end) {
            char key[40];
            js_skip_ws(&j);
            if (j.p < j.end && *j.p == '}') { j.p++; break; }
            if (!js_string(&j, key, sizeof key)) {
                set_err(err, err_cap, "bad release field");
                return 0;
            }
            js_skip_ws(&j);
            if (j.p >= j.end || *j.p != ':') {
                set_err(err, err_cap, "bad release field");
                return 0;
            }
            j.p++;
            if (strcmp(key, "tag_name") == 0) {
                if (!js_string(&j, tag, sizeof tag)) {
                    set_err(err, err_cap, "bad release tag");
                    return 0;
                }
                saw_tag = 1;
            } else if (strcmp(key, "draft") == 0) {
                if (!js_bool(&j, &draft)) return 0;
                saw_draft = 1;
            } else if (strcmp(key, "prerelease") == 0) {
                if (!js_bool(&j, &pre)) return 0;
                saw_pre = 1;
            } else if (strcmp(key, "assets") == 0) {
                js_skip_ws(&j);
                if (j.p >= j.end || *j.p != '[') {
                    set_err(err, err_cap, "release assets were not a list");
                    return 0;
                }
                j.p++;
                while (j.p < j.end) {
                    char name[80] = {0}, url[256] = {0};
                    js_skip_ws(&j);
                    if (j.p < j.end && *j.p == ']') { j.p++; break; }
                    if (j.p >= j.end || *j.p != '{') {
                        set_err(err, err_cap, "bad release asset");
                        return 0;
                    }
                    j.p++;
                    while (j.p < j.end) {
                        char ak[40];
                        js_skip_ws(&j);
                        if (j.p < j.end && *j.p == '}') { j.p++; break; }
                        if (!js_string(&j, ak, sizeof ak)) return 0;
                        js_skip_ws(&j);
                        if (j.p >= j.end || *j.p != ':') return 0;
                        j.p++;
                        if (strcmp(ak, "name") == 0) {
                            if (!js_string(&j, name, sizeof name)) return 0;
                        } else if (strcmp(ak, "browser_download_url") == 0) {
                            if (!js_string(&j, url, sizeof url)) return 0;
                        } else if (!js_skip(&j, 0)) {
                            set_err(err, err_cap, "could not skip a release field");
                            return 0;
                        }
                        js_skip_ws(&j);
                        if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                        if (j.p < j.end && *j.p == '}') { j.p++; break; }
                        set_err(err, err_cap, "bad release asset");
                        return 0;
                    }
                    if (strcmp(name, UPD_MANIFEST_NAME) == 0 && url[0] && n_manifest_urls < 4) {
                        snprintf(manifest_urls[n_manifest_urls], sizeof manifest_urls[0], "%s", url);
                        n_manifest_urls++;
                    }
                    js_skip_ws(&j);
                    if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                    if (j.p < j.end && *j.p == ']') { j.p++; break; }
                }
            } else if (!js_skip(&j, 0)) {
                set_err(err, err_cap, "could not read the release list");
                return 0;
            }
            js_skip_ws(&j);
            if (j.p < j.end && *j.p == ',') { j.p++; continue; }
            if (j.p < j.end && *j.p == '}') { j.p++; break; }
        }
        upd_ver tv;
        int manifest_ok = 0;
        for (int ui = 0; ui < n_manifest_urls; ui++) {
            if (canonical_asset_url(tag, UPD_MANIFEST_NAME, manifest_urls[ui])) manifest_ok = 1;
        }
        if (saw_tag && saw_draft && saw_pre && !draft && !pre && manifest_ok && upd_ver_parse(tag, &tv)) {
            snprintf(out->tag, sizeof out->tag, "%s", tag);
            snprintf(out->manifest_name, sizeof out->manifest_name, "%s", UPD_MANIFEST_NAME);
            return 1;
        }
        js_skip_ws(&j);
        if (j.p < j.end && *j.p == ',') { j.p++; continue; }
        if (j.p < j.end && *j.p == ']') {
            set_err(err, err_cap, "no stable CrowPanel release is published");
            return 0;
        }
    }
    set_err(err, err_cap, "no stable CrowPanel release is published");
    return 0;
}

static int part_entry(const uint8_t *e, char *name, uint8_t *type, uint8_t *subtype, uint32_t *off, uint32_t *sz)
{
    if (e[0] != 0xAA || e[1] != 0x50) return 0;
    *type = e[2];
    *subtype = e[3];
    *off = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
    *sz = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    memcpy(name, e + 12, 16);
    name[16] = 0;
    return 1;
}

int upd_partition_is_cp7(const uint8_t *table, size_t n, char *err, size_t err_cap)
{
    int nvs = 0, factory = 0, assets = 0;
    if (!table || n < 96) {
        set_err(err, err_cap, "partition table is too small");
        return 0;
    }
    for (size_t i = 0; i + 32 <= n && i < 0xC00; i += 32) {
        char name[17];
        uint8_t type, sub;
        uint32_t off, sz;
        if (table[i] == 0xFF) break;
        if (!part_entry(table + i, name, &type, &sub, &off, &sz)) {
            if (i == 0) {
                set_err(err, err_cap, "partition table magic is not present");
                return 0;
            }
            break;
        }
        if (strcmp(name, "nvs") == 0 && type == 0x01 && off == UPD_NVS_OFFSET && sz == UPD_NVS_SIZE) nvs = 1;
        if (strcmp(name, "factory") == 0 && type == 0x00 && off == UPD_APP_OFFSET && sz == UPD_FACTORY_SIZE) factory = 1;
        if (strcmp(name, "assets") == 0 && type == 0x01 && off == UPD_ASSETS_OFFSET && sz == UPD_ASSETS_SIZE) assets = 1;
    }
    if (!nvs || !factory || !assets) {
        set_err(err, err_cap, "flash layout is not the cp7 app slot; refusing to write");
        return 0;
    }
    return 1;
}

int upd_range_is_app_slot(uint32_t offset, uint32_t len)
{
    if (len == 0 || offset != UPD_APP_OFFSET) return 0;
    if (len > UPD_FACTORY_SIZE) return 0;
    if (offset + len < offset) return 0;
    return offset + len <= UPD_APP_SLOT_END;
}

int upd_decide(const char *device_ver, const upd_manifest *pkg, int allow_same, char *err, size_t err_cap)
{
    upd_ver dev, want;
    if (!device_ver || !upd_ver_parse(device_ver, &dev)) {
        set_err(err, err_cap, "the board does not look like cp7 firmware; refusing to write");
        return UPD_NO_DEVICE_VER;
    }
    if (!upd_ver_parse(pkg->version, &want)) {
        set_err(err, err_cap, "package version is invalid");
        return UPD_NO_DEVICE_VER;
    }
    int c = upd_ver_cmp(want, dev);
    if (c > 0) return UPD_ALLOW;
    if (c == 0) {
        if (allow_same) return UPD_SAME;
        set_err(err, err_cap, "the board is already on this version");
        return UPD_SAME;
    }
    set_err(err, err_cap, "refusing to downgrade the firmware");
    return UPD_DOWNGRADE;
}
