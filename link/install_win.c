/*
 * First-install window for a stock CrowPanel Advance 7.0.
 * Same exe as HD2 Macropad Link. Reuses the v1.2.3 flasher (stub, 460800 max,
 * compressed writes, MD5, ROM fallback). This is not the app-only updater:
 * it reads the whole 16 MB flash first, then writes bootloader, partition
 * table, and app. It never mass-erases and it never writes NVS.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "updater_ui.h"
#include "romflash.h"
#include "update_core.h"
#include "link_session.h"
#include "sha256.h"
#include "md5.h"
#include "backup_io.h"

extern const GUID GUID_DEVINTERFACE_COMPORT_LOCAL;

#define ID_PORT     3101
#define ID_STATUS   3102
#define ID_LOG      3103
#define ID_GO       3104
#define ID_PROBE    3105
#define ID_FIND     3106
#define ID_SEND     3107
#define ID_FOLDER   3108
#define ID_ALREADY  3109
#define ID_CLOSE    3110
#define ID_BOOT     101
#define ID_PART     102
#define ID_APP      103
#define ID_ASSETS   104

#define FW_VER "v1.9.1-cp7.2"
#define SHA_APP "c12dd56c41f1b0d531e12e3eceb8930ff6c0ea46f78bfdf23f3e4c706562aa63"
#define SHA_PART "bdf044742fcec45de4d49e8062eb06096901498831e3641732e9be1ed8cb5a5e"
#define SHA_BOOT "cdd4216c20e9ec2762286c17899a9fbe064147fbe73cdf24f7c14afd68772bba"
#define SHA_ASSETS "d4b9bb6a322f6e2480b16544b75971010aa6b7b1445fe67a5fedc6cf3c965c10"
#define MD5_ASSETS "56ad5746c2f2940c636553dcdcae383e"

static const wchar_t *EXPLAIN =
    L"The USB-C cable must carry data as well as power. A charge-only "
    L"(power-only) cable will not work. The panel may light up, but Windows "
    L"will not see the CH340 COM port, and this installer cannot talk to the "
    L"board. Use a cable you know can transfer files, or one sold as a data "
    L"cable. If no port appears, try another cable and another USB port.\r\n\r\n"
    L"Install HD2 Macropad v1.9.1-cp7.2 on an Elecrow CrowPanel Advance 7.0 "
    L"(SKU DIS02170A, ESP32-S3, 16 MB flash, 8 MB PSRAM). Use this for a stock "
    L"Elecrow board, or a board that is not already running this port. A board "
    L"that already runs HD2 should use Check for firmware update, which keeps "
    L"NVS presets and writes only the app.\r\n\r\n"
    L"Back up and install does four things. It finds the CH340 USB-serial port "
    L"(it does not assume COM5). It reads the entire 16 MB flash to a file on "
    L"this PC before it changes the chip. Baud is 460800. 921600 is never used.\r\n\r\n"
    L"How long to expect. The writes in a full first install have not been timed, "
    L"because that run stopped after the backup. On the maintainer PC a 16 MB "
    L"backup at 460800 with the stub took about 6 minutes 40 seconds (400.9 "
    L"seconds in the installer, 398.7 seconds with esptool). That is the long "
    L"step. A slower PC, or a fallback to the slower built-in loader (the log "
    L"says FALLBACK), can take longer.\r\n\r\n"
    L"When to wait. While the phase name, the byte count, or the percent is "
    L"moving, wait. Do not unplug the board and do not close this window, "
    L"especially during a write. Finding the CH340 port and talking to the "
    L"bootloader should succeed within about a minute. The 16 MB backup can stay "
    L"on that line for many minutes, but the byte count must keep increasing. "
    L"If the same progress line has not changed for 3 minutes during backup, "
    L"write, or verify, treat it as stuck. This window will say so and point at "
    L"Send the log for checking. A finished failure is not a hang: the window "
    L"says it failed. Use Send the log. If a write failed and rollback did not "
    L"finish, do not keep retrying. Troubleshooting is in "
    L"docs\\crowpanel\\README.md.\r\n\r\n"
    L"It then writes four regions only: the bootloader at 0x0, the cp7 partition "
    L"table at 0x8000, the app at 0x10000, and the sound FAT at 0xA10000. "
    L"It does not mass-erase the chip. It does not write NVS at 0x9000 or phy_init "
    L"at 0xF000. A stock board's NVS is not HD2 presets, and this installer leaves "
    L"those bytes in place. A rollback writes the backup back over every region it "
    L"changed, including sounds, and does not rewrite NVS. After the board boots, "
    L"open Config and set Connectivity to USB. To hear the sounds, use a PH2.0-2P "
    L"speaker on the SPK header and set the DIP switches to S1=0 and S0=0.\r\n\r\n"
    L"If a step fails, this window keeps a log and can open a GitHub issue with a "
    L"short summary. Drag the log file onto that issue. Do not paste a home-folder path.";

typedef struct {
    wchar_t path[MAX_PATH];
    hd2_bk io;
} bk_t;

typedef struct inst_ui inst_ui;

typedef struct {
    inst_ui *ui;
    int do_write;
    int probe_only;
    int cli;
    int log_stdout;
    int rom_only;
    rom_install_image images[4];
    rom_install_result ir;
    rom_probe_result pr;
    bk_t bk;
    int rc;
    wchar_t log_path[MAX_PATH];
    unsigned ms_total;
} inst_job;

struct inst_ui {
    HWND hwnd;
    HWND status;
    HWND port_lbl;
    HWND log_edit;
    wchar_t port[16];
    int running;
    int have_log;
    int last_rc;
    volatile LONG cancel;
    inst_job job;
    uint8_t *boot, *part, *app, *assets;
    size_t boot_n, part_n, app_n, assets_n;
    int package_ok;
    char package_err[200];
};

static void widen(const char *src, wchar_t *dst, int cap)
{
    if (!src) src = "";
    if (cap < 1) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap)) dst[0] = 0;
}

static int reg_offered(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof v;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, L"InstallOffered", NULL, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 0;
    RegCloseKey(k);
    return v != 0;
}

static void reg_set_offered(void)
{
    HKEY k;
    DWORD v = 1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"InstallOffered", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

int link_install_should_prompt(void)
{
    return !reg_offered();
}

static int enum_ch340(wchar_t ports[][16], int max)
{
    int n = 0;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT_LOCAL, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVINFO_DATA dd;
    if (di == INVALID_HANDLE_VALUE) return 0;
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd) && n < max; i++) {
        wchar_t hwid[512] = {0};
        HKEY k;
        wchar_t name[16] = {0};
        DWORD sz = sizeof name - 2, type = 0;
        SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_HARDWAREID, NULL, (PBYTE)hwid, sizeof hwid - 2, NULL);
        _wcsupr(hwid);
        if (!wcsstr(hwid, L"VID_1A86")) continue;
        k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        if (RegQueryValueExW(k, L"PortName", NULL, &type, (BYTE *)name, &sz) == ERROR_SUCCESS && !wcsncmp(name, L"COM", 3))
            lstrcpynW(ports[n++], name, 16);
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return n;
}

static int load_rc(int id, uint8_t **out, size_t *n, char *err, size_t cap)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    DWORD sz;
    HGLOBAL g;
    void *p;
    *out = NULL;
    *n = 0;
    if (!r) {
        snprintf(err, cap, "firmware resource %d is missing from this exe", id);
        return 0;
    }
    sz = SizeofResource(NULL, r);
    g = LoadResource(NULL, r);
    p = g ? LockResource(g) : NULL;
    if (!p || sz == 0) {
        snprintf(err, cap, "firmware resource %d could not be read", id);
        return 0;
    }
    *out = (uint8_t *)malloc(sz);
    if (!*out) {
        snprintf(err, cap, "not enough memory for firmware resource %d", id);
        return 0;
    }
    memcpy(*out, p, sz);
    *n = sz;
    return 1;
}

static int hash_ok(const uint8_t *p, size_t n, const char *want, char *err, size_t cap, const char *what)
{
    char hex[80];
    if (sha256_hex(p, n, hex, sizeof hex) != 0 || _stricmp(hex, want) != 0) {
        snprintf(err, cap, "bundled %s hash does not match the tested package", what);
        return 0;
    }
    return 1;
}

static int md5_hex_is(const uint8_t *p, size_t n, const char *want)
{
    uint8_t d[16];
    char hex[33];
    int i;
    md5_buf(p, n, d);
    for (i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return _stricmp(hex, want) == 0;
}

static int package_ok(inst_ui *ui)
{
    char ver[32];
    uint32_t total = 0;
    ui->package_err[0] = 0;
    if (!load_rc(ID_BOOT, &ui->boot, &ui->boot_n, ui->package_err, sizeof ui->package_err) ||
        !load_rc(ID_PART, &ui->part, &ui->part_n, ui->package_err, sizeof ui->package_err) ||
        !load_rc(ID_APP, &ui->app, &ui->app_n, ui->package_err, sizeof ui->package_err) ||
        !load_rc(ID_ASSETS, &ui->assets, &ui->assets_n, ui->package_err, sizeof ui->package_err))
        return 0;
    if (!hash_ok(ui->boot, ui->boot_n, SHA_BOOT, ui->package_err, sizeof ui->package_err, "bootloader") ||
        !hash_ok(ui->part, ui->part_n, SHA_PART, ui->package_err, sizeof ui->package_err, "partition table") ||
        !hash_ok(ui->app, ui->app_n, SHA_APP, ui->package_err, sizeof ui->package_err, "app") ||
        !hash_ok(ui->assets, ui->assets_n, SHA_ASSETS, ui->package_err, sizeof ui->package_err, "assets"))
        return 0;
    if (ui->boot[0] != 0xE9 || ui->boot[12] != 9 || ui->boot_n > 0x7000 ||
        !upd_image_length(ui->boot, ui->boot_n, &total, ui->package_err, sizeof ui->package_err) ||
        total != ui->boot_n) {
        if (!ui->package_err[0]) snprintf(ui->package_err, sizeof ui->package_err, "bundled bootloader is not a usable ESP32-S3 image");
        return 0;
    }
    if (ui->part_n != UPD_PART_TABLE_LEN ||
        !upd_partition_is_cp7(ui->part, ui->part_n, ui->package_err, sizeof ui->package_err))
        return 0;
    if (ui->app_n != 2462928 ||
        !upd_image_length(ui->app, ui->app_n, &total, ui->package_err, sizeof ui->package_err) ||
        total != ui->app_n ||
        !upd_image_version(ui->app, ui->app_n, ver, sizeof ver, ui->package_err, sizeof ui->package_err) ||
        strcmp(ver, FW_VER) != 0) {
        if (!ui->package_err[0]) snprintf(ui->package_err, sizeof ui->package_err, "bundled app is not %s", FW_VER);
        return 0;
    }
    if (ui->assets_n != UPD_ASSETS_SIZE || !md5_hex_is(ui->assets, ui->assets_n, MD5_ASSETS)) {
        snprintf(ui->package_err, sizeof ui->package_err, "bundled assets image is not the 4 MiB sound partition");
        return 0;
    }
    return 1;
}

static int bk_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    inst_job *job = ctx;
    bk_t *b = &job->bk;
    if (off == 0) {
        FILE *f;
        if (b->io.f) {
            fclose(b->io.f);
            b->io.f = NULL;
        }
        /* "wb+" so the head/tail check and a rollback can read the file back.
         * "wb" is write-only and fread fails after a complete backup. */
        f = _wfopen(b->path, L"wb+");
        if (!f || hd2_bk_begin(&b->io, f) != 0) {
            if (f) fclose(f);
            hd2_bk_reset(&b->io);
            return -1;
        }
    }
    return hd2_bk_write(&b->io, off, data, n);
}

static int bk_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)
{
    inst_job *job = ctx;
    return hd2_bk_read(&job->bk.io, off, dst, n);
}

static void bk_finish(inst_job *job)
{
    bk_t *b = &job->bk;
    wchar_t side[MAX_PATH];
    FILE *sf;
    if (!b->io.f && b->io.got == 0) return;
    _snwprintf(side, MAX_PATH, L"%s.sha256", b->path);
    sf = _wfopen(side, L"wb");
    if (sf) {
        hd2_bk_sidecar(&b->io, sf, "flash-full.bin");
        fclose(sf);
    }
    if (b->io.f) {
        fclose(b->io.f);
        b->io.f = NULL;
    }
}

static void bk_prepare(inst_job *job)
{
    wchar_t dir[MAX_PATH];
    SYSTEMTIME st;
    memset(&job->bk, 0, sizeof job->bk);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    _snwprintf(job->bk.path, MAX_PATH, L"%s\\HD2MacropadLink", dir);
    CreateDirectoryW(job->bk.path, NULL);
    _snwprintf(job->bk.path, MAX_PATH, L"%s\\HD2MacropadLink\\backups", dir);
    CreateDirectoryW(job->bk.path, NULL);
    GetLocalTime(&st);
    _snwprintf(job->bk.path, MAX_PATH, L"%s\\HD2MacropadLink\\backups\\flash-full-%04u%02u%02u-%02u%02u%02u.bin",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static int install_body(void *io_v, void *ctx)
{
    rom_io *io = io_v;
    inst_job *job = ctx;
    char line[600];
    int rc;
    if (job->probe_only) {
        if (io->log) io->log(io->ctx, "check connection: read-only probe, nothing is written");
        rc = rom_probe(io, &job->pr);
        snprintf(line, sizeof line, "probe rc=%d stub=%d fallback=%d layout_cp7=%d version=%s md5=%s error=%s",
                 rc, job->pr.stub, job->pr.stub_fallback, job->pr.layout_cp7,
                 job->pr.version[0] ? job->pr.version : "?", job->pr.app_md5[0] ? job->pr.app_md5 : "?",
                 job->pr.error[0] ? job->pr.error : "");
        if (io->log) io->log(io->ctx, line);
        return rc;
    }
    if (io->log) io->log(io->ctx, "first-install: full 16 MB backup, then bootloader 0x0, partitions 0x8000, app 0x10000, sounds 0xA10000. NVS is not written. baud cap 460800.");
    rc = rom_first_install(io, job->images, 4, ROM_FLASH_SIZE, FW_VER, job->do_write,
                           bk_write, bk_read, job, &job->ir);
    snprintf(line, sizeof line,
             "install rc=%d wrote=%d verified=%d restored=%d confirmed=%d version=%s nvs_written=%d stub=%d fallback=%d backup_bytes=%u error=%s",
             rc, job->ir.wrote, job->ir.verified, job->ir.restored, job->ir.confirmed,
             job->ir.confirmed_version[0] ? job->ir.confirmed_version : "?",
             job->ir.nvs_written, job->ir.used_stub, job->ir.stub_fallback,
             (unsigned)job->ir.backup_bytes, job->ir.error[0] ? job->ir.error : "");
    if (io->log) io->log(io->ctx, line);
    return rc;
}

static int run_job(inst_ui *ui, const wchar_t *port, int do_write, int probe, int cli, int log_stdout, int rom_only)
{
    inst_job *job = &ui->job;
    memset(&job->ir, 0, sizeof job->ir);
    memset(&job->pr, 0, sizeof job->pr);
    job->ui = ui;
    job->do_write = do_write;
    job->probe_only = probe;
    job->cli = cli;
    job->log_stdout = log_stdout;
    job->rom_only = rom_only;
    job->log_path[0] = 0;
    job->images[0] = (rom_install_image){0, ui->boot, (uint32_t)ui->boot_n, "bootloader"};
    job->images[1] = (rom_install_image){UPD_PART_TABLE, ui->part, (uint32_t)ui->part_n, "partitions"};
    job->images[2] = (rom_install_image){UPD_APP_OFFSET, ui->app, (uint32_t)ui->app_n, "app"};
    job->images[3] = (rom_install_image){UPD_ASSETS_OFFSET, ui->assets, (uint32_t)ui->assets_n, "assets"};
    if (!probe) bk_prepare(job);
    job->rc = link_run_rom_body(port, ui->status, &ui->cancel, cli, log_stdout, rom_only, probe ? 0 : 1,
                                L"install", install_body, job, job->log_path, MAX_PATH, &job->ms_total);
    if (!probe) bk_finish(job);
    if (job->rc == -3) {
        char port8[16];
        WideCharToMultiByte(CP_UTF8, 0, port, -1, port8, (int)sizeof port8, NULL, NULL);
        snprintf(job->ir.error, sizeof job->ir.error,
                 "%s is busy or missing. Close Link and any other serial program. Nothing was written.",
                 port8[0] ? port8 : "the port");
        job->rc = -1;
    }
    return job->rc;
}

static void set_status(inst_ui *ui, const wchar_t *text)
{
    if (ui->status) SetWindowTextW(ui->status, text);
}

static void show_log_path(inst_ui *ui)
{
    if (ui->log_edit) SetWindowTextW(ui->log_edit, ui->job.log_path[0] ? ui->job.log_path : L"");
    ui->have_log = ui->job.log_path[0] != 0;
    EnableWindow(GetDlgItem(ui->hwnd, ID_FOLDER), ui->have_log);
    EnableWindow(GetDlgItem(ui->hwnd, ID_SEND), ui->have_log && ui->last_rc != 0);
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    size_t i;
    for (i = 0; in && in[i] && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void copy_text(const wchar_t *text)
{
    size_t n;
    HGLOBAL g;
    wchar_t *p;
    if (!text || !OpenClipboard(NULL)) return;
    EmptyClipboard();
    n = wcslen(text) + 1;
    g = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
    if (g) {
        p = GlobalLock(g);
        if (p) {
            memcpy(p, text, n * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

static void send_log(inst_ui *ui)
{
    char err[200], body[900], ebody[1400], etitle[240], url[1800];
    wchar_t wurl[1900], args[MAX_PATH + 32];
    const char *src = ui->job.probe_only ? (ui->job.pr.error[0] ? ui->job.pr.error : "connection check failed")
                                         : (ui->job.ir.error[0] ? ui->job.ir.error : "install failed");
    char port8[16];
    snprintf(err, sizeof err, "%.160s", src);
    for (char *p = err; *p; p++) if (*p == '\n' || *p == '\r' || *p == '"') *p = ' ';
    WideCharToMultiByte(CP_UTF8, 0, ui->port, -1, port8, (int)sizeof port8, NULL, NULL);
    snprintf(body, sizeof body,
             "First-install log for checking. No secrets and no home-folder path.\n\n"
             "Link: 1.2.6 (updater core 1.2.3)\n"
             "Firmware package: %s\n"
             "Chip: esp32s3\n"
             "Baud cap: 460800 (not 921600)\n"
             "Port: %s\n"
             "Exit code: %d\n"
             "NVS written: no\n"
             "Regions attempted: bootloader 0x0, partitions 0x8000, app 0x10000, sounds 0xA10000\n"
             "Confirmed version: %s\n"
             "Stub: %d  ROM fallback: %d\n"
             "Last error: %s\n\n"
             "Please drag the log file onto this issue. Its name starts with install-. "
             "The installer opened the folder and copied the full path to the clipboard for you. "
             "Do not paste that path into this issue.\n",
             FW_VER, port8[0] ? port8 : "?", ui->last_rc,
             ui->job.ir.confirmed_version[0] ? ui->job.ir.confirmed_version : "?",
             ui->job.ir.used_stub, ui->job.ir.stub_fallback, err);
    url_encode("[CrowPanel] First-install failed", etitle, sizeof etitle);
    url_encode(body, ebody, sizeof ebody);
    snprintf(url, sizeof url, "https://github.com/damageu/hd2_macropad-crowpanel7/issues/new?title=%s&body=%s", etitle, ebody);
    if (strlen(url) > 1800) {
        snprintf(url, sizeof url, "https://github.com/damageu/hd2_macropad-crowpanel7/issues/new?title=%s", etitle);
    }
    widen(url, wurl, 1900);
    ShellExecuteW(ui->hwnd, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
    if (ui->job.log_path[0]) {
        _snwprintf(args, MAX_PATH + 32, L"/select,\"%s\"", ui->job.log_path);
        ShellExecuteW(ui->hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        copy_text(ui->job.log_path);
    }
    set_status(ui, L"A GitHub issue draft is open. The log folder is open and the log path is on the clipboard. Drag that file onto the issue. Do not paste your user-folder path into the text.");
}

static void apply_result(inst_ui *ui)
{
    wchar_t msg[1200], errw[500], bk[MAX_PATH];
    const char *err = ui->job.probe_only ? ui->job.pr.error : ui->job.ir.error;
    ui->last_rc = (ui->job.rc == 0) ? 0 : 1;
    widen(err, errw, 500);
    if (ui->job.bk.path[0]) lstrcpynW(bk, ui->job.bk.path, MAX_PATH);
    else bk[0] = 0;
    if (ui->job.probe_only && ui->job.rc == 0) {
        wchar_t ver[40];
        widen(ui->job.pr.version[0] ? ui->job.pr.version : "?", ver, 40);
        _snwprintf(msg, 1200,
                   L"Connection OK on %s. Layout %s. Version %s. Nothing was written. %s",
                   ui->port, ui->job.pr.layout_cp7 ? L"cp7" : L"not cp7",
                   ver,
                   ui->job.pr.layout_cp7
                       ? L"If this pad already runs HD2, use Check for firmware update instead of a first install."
                       : L"A stock or unknown layout is what Back up and install is for.");
        ui->last_rc = 0;
    } else if (!ui->job.probe_only && ui->job.rc == 0 && ui->job.do_write) {
        _snwprintf(msg, 1200,
                   L"Installed %hs and confirmed it after reset. NVS was not written. Sounds were written. Set Config, Connectivity to USB. A PH2.0-2P speaker on SPK, with DIP S1=0 and S0=0, is what makes them audible.\nBackup:\n%s",
                   FW_VER, bk);
        reg_set_offered();
    } else if (!ui->job.probe_only && ui->job.rc == 0) {
        _snwprintf(msg, 1200, L"Backup saved. Nothing was written.\n%s", bk);
    } else {
        _snwprintf(msg, 1200, L"%s\nBackup, if one was saved:\n%s\nUse Send the log for checking. Nothing was mass-erased.",
                   errw[0] ? errw : L"The install failed.", bk[0] ? bk : L"(none)");
        ui->last_rc = 1;
    }
    set_status(ui, msg);
    show_log_path(ui);
}

static void find_port(inst_ui *ui)
{
    wchar_t err[300];
    link_resume_after_update();
    ui->port[0] = 0;
    if (!link_pause_for_update(ui->port, 16, err, 300)) {
        set_status(ui, err[0] ? err : L"No CH340 serial device is connected. The USB-C cable must carry data, not only power. A charge-only cable can light the panel and still show no COM port. Try a cable known to transfer files, or another USB port.");
        SetWindowTextW(ui->port_lbl, L"Port: none yet");
        EnableWindow(GetDlgItem(ui->hwnd, ID_GO), FALSE);
        EnableWindow(GetDlgItem(ui->hwnd, ID_PROBE), FALSE);
        return;
    }
    _snwprintf(err, 300, L"Port: %s (CH340, not fixed to COM5)", ui->port);
    SetWindowTextW(ui->port_lbl, err);
    EnableWindow(GetDlgItem(ui->hwnd, ID_GO), ui->package_ok);
    EnableWindow(GetDlgItem(ui->hwnd, ID_PROBE), TRUE);
    if (ui->package_ok)
        set_status(ui, L"Port found. The 16 MB backup is the long step: about 6 minutes 40 seconds on the maintainer PC at 460800. Writing and checking have not been timed yet. Wait while the count moves. Do not unplug the panel.");
    else {
        wchar_t w[300];
        widen(ui->package_err, w, 300);
        set_status(ui, w);
    }
}

/* The flasher blocks the calling thread. Run it beside the message loop. */
typedef struct {
    inst_ui *ui;
    int do_write;
    int probe;
    int rc;
} work_t;

static DWORD WINAPI work_thread(LPVOID arg)
{
    work_t *w = arg;
    w->rc = run_job(w->ui, w->ui->port, w->do_write, w->probe, 0, 0, 0);
    return 0;
}

static void start_work_async(inst_ui *ui, int do_write, int probe)
{
    work_t w;
    HANDLE th;
    MSG msg;
    if (ui->running || !ui->port[0]) return;
    if (!probe && !ui->package_ok) return;
    ui->running = 1;
    ui->cancel = 0;
    EnableWindow(GetDlgItem(ui->hwnd, ID_GO), FALSE);
    EnableWindow(GetDlgItem(ui->hwnd, ID_PROBE), FALSE);
    EnableWindow(GetDlgItem(ui->hwnd, ID_FIND), FALSE);
    set_status(ui, probe ? L"Checking the connection. Nothing will be written. Finding the port and the bootloader should succeed within about a minute."
                         : L"Backup: whole 16 MB flash. About 6 minutes 40 seconds on the maintainer PC. Wait while the count moves. Do not unplug the panel.");
    w.ui = ui;
    w.do_write = do_write;
    w.probe = probe;
    w.rc = -1;
    th = CreateThread(NULL, 0, work_thread, &w, 0, NULL);
    {
        wchar_t seen[384];
        DWORD since = GetTickCount();
        int stalled = 0;
        seen[0] = 0;
        while (th && WaitForSingleObject(th, 50) == WAIT_TIMEOUT) {
            wchar_t cur[384];
            int banner;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_COMMAND && LOWORD(msg.wParam) == ID_CLOSE)
                    InterlockedExchange(&ui->cancel, 1);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!IsWindow(ui->hwnd) || !ui->status) continue;
            GetWindowTextW(ui->status, cur, 384);
            banner = wcsncmp(cur, L"This looks stuck.", 16) == 0;
            if (!banner && wcscmp(cur, seen) != 0) {
                lstrcpynW(seen, cur, 384);
                since = GetTickCount();
                stalled = 0;
            } else if (!banner && !stalled) {
                DWORD limit = wcsstr(seen, L"KB") ? 180000u : 60000u;
                if (GetTickCount() - since >= limit) {
                    stalled = 1;
                    set_status(ui, L"This looks stuck. The progress line has not changed (about a minute with no byte count, or 3 minutes during backup, write, or verify). Do not unplug if a write might still be running. Troubleshooting is in docs\\crowpanel\\README.md. Use Send the log for checking. If a write failed and rollback did not finish, do not keep retrying.");
                    if (ui->job.log_path[0]) {
                        SetWindowTextW(ui->log_edit, ui->job.log_path);
                        EnableWindow(GetDlgItem(ui->hwnd, ID_FOLDER), TRUE);
                        EnableWindow(GetDlgItem(ui->hwnd, ID_SEND), TRUE);
                    }
                }
            }
        }
    }
    if (th) CloseHandle(th);
    ui->job.rc = w.rc;
    ui->running = 0;
    EnableWindow(GetDlgItem(ui->hwnd, ID_FIND), TRUE);
    if (IsWindow(ui->hwnd)) {
        EnableWindow(GetDlgItem(ui->hwnd, ID_GO), ui->package_ok && ui->port[0]);
        EnableWindow(GetDlgItem(ui->hwnd, ID_PROBE), ui->port[0] != 0);
        apply_result(ui);
    }
}

static LRESULT CALLBACK inst_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    inst_ui *ui = (inst_ui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (!ui) return 0;
        if (ui->running) {
            if (LOWORD(wp) == ID_CLOSE) InterlockedExchange(&ui->cancel, 1);
            return 0;
        }
        if (LOWORD(wp) == ID_GO) start_work_async(ui, 1, 0);
        else if (LOWORD(wp) == ID_PROBE) start_work_async(ui, 0, 1);
        else if (LOWORD(wp) == ID_FIND) find_port(ui);
        else if (LOWORD(wp) == ID_SEND) send_log(ui);
        else if (LOWORD(wp) == ID_FOLDER && ui->job.log_path[0]) {
            wchar_t args[MAX_PATH + 32];
            _snwprintf(args, MAX_PATH + 32, L"/select,\"%s\"", ui->job.log_path);
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == ID_ALREADY) {
            reg_set_offered();
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == ID_CLOSE || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (ui && ui->running) {
            InterlockedExchange(&ui->cancel, 1);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void link_install_show(HWND owner, int first_run)
{
    static int registered;
    inst_ui ui;
    WNDCLASSW wc;
    HWND w, edit, go, probe, find, send, folder, already, closeb;
    HFONT font;
    MSG msg;
    const int width = 640, height = 640;
    (void)owner;
    (void)first_run;
    memset(&ui, 0, sizeof ui);
    ui.package_ok = package_ok(&ui);
    if (!registered) {
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = inst_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"HD2MacropadInstall";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc);
        registered = 1;
    }
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HD2MacropadInstall",
                        L"Install HD2 Macropad firmware",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        60, 40, width, height, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ui.hwnd = w;
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)&ui);
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    CreateWindowExW(0, L"STATIC",
                    L"Use a USB-C data cable, not charge-only. The panel can light up and still have no COM port.",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 10, width - 40, 36, w, NULL, NULL, NULL);
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", EXPLAIN,
                           WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                           16, 46, width - 40, 250, w, NULL, NULL, NULL);
    ui.port_lbl = CreateWindowExW(0, L"STATIC", L"Port: looking for a CH340...",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 306, width - 40, 20, w, (HMENU)(INT_PTR)ID_PORT, NULL, NULL);
    ui.status = CreateWindowExW(0, L"STATIC", L"Finding the USB-serial port.",
                                WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 330, width - 40, 88, w, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
    ui.log_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                                  16, 424, width - 40, 22, w, (HMENU)(INT_PTR)ID_LOG, NULL, NULL);
    go = CreateWindowExW(0, L"BUTTON", L"Back up and install", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                         16, 458, 160, 28, w, (HMENU)(INT_PTR)ID_GO, NULL, NULL);
    probe = CreateWindowExW(0, L"BUTTON", L"Check connection", WS_CHILD | WS_VISIBLE,
                            184, 458, 140, 28, w, (HMENU)(INT_PTR)ID_PROBE, NULL, NULL);
    find = CreateWindowExW(0, L"BUTTON", L"Find CH340 port", WS_CHILD | WS_VISIBLE,
                           332, 458, 140, 28, w, (HMENU)(INT_PTR)ID_FIND, NULL, NULL);
    send = CreateWindowExW(0, L"BUTTON", L"Send the log for checking", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                           16, 496, 200, 28, w, (HMENU)(INT_PTR)ID_SEND, NULL, NULL);
    folder = CreateWindowExW(0, L"BUTTON", L"Open log folder", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                             224, 496, 140, 28, w, (HMENU)(INT_PTR)ID_FOLDER, NULL, NULL);
    already = CreateWindowExW(0, L"BUTTON", L"This board already runs HD2", WS_CHILD | WS_VISIBLE,
                              16, 534, 210, 28, w, (HMENU)(INT_PTR)ID_ALREADY, NULL, NULL);
    closeb = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                             width - 130, 534, 90, 28, w, (HMENU)(INT_PTR)ID_CLOSE, NULL, NULL);
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ui.port_lbl, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ui.status, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ui.log_edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(go, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(probe, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(find, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(send, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(folder, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(already, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(closeb, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(w, SW_SHOWNORMAL);
    SetForegroundWindow(w);
    find_port(&ui);
    while (IsWindow(w) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    link_resume_after_update();
    free(ui.boot);
    free(ui.part);
    free(ui.app);
    free(ui.assets);
}

static int cli_port_ok(const wchar_t *port)
{
    if (!port || wcsncmp(port, L"COM", 3) != 0 || wcslen(port) < 4 || wcslen(port) > 6) return 0;
    for (const wchar_t *p = port + 3; *p; p++) if (*p < L'0' || *p > L'9') return 0;
    return 1;
}

static void one_line(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '"' || c == '\n' || c == '\r') c = '\'';
        out[o++] = c;
    }
    out[o] = 0;
}

int link_install_cli(const wchar_t *cmdline)
{
    int argc = 0, saw = 0, bad = 0, yes = 0, dry = 0, log_stdout = 0, rom_only = 0;
    wchar_t **argv, portbuf[16], ports[8][16];
    const wchar_t *port = NULL, *result_file = NULL;
    inst_ui ui;
    HANDLE mtx;
    int code, n;
    char errq[300], logp[MAX_PATH * 3];
    if (!cmdline) return -1;
    argv = CommandLineToArgvW(cmdline, &argc);
    if (!argv) return -1;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--install")) saw = 1;
        else if (!wcscmp(a, L"--port") && i + 1 < argc) port = argv[++i];
        else if (!wcscmp(a, L"--yes")) yes = 1;
        else if (!wcscmp(a, L"--dry-run")) dry = 1;
        else if (!wcscmp(a, L"--log-stdout")) log_stdout = 1;
        else if (!wcscmp(a, L"--rom-only")) rom_only = 1;
        else if (!wcscmp(a, L"--result-file") && i + 1 < argc) result_file = argv[++i];
        else if (!wcscmp(a, L"--update") || !wcscmp(a, L"--probe") || !wcscmp(a, L"--reinstall") || !wcscmp(a, L"--full-backup"))
            bad = 1;
        else bad = 1;
    }
    if (!saw) {
        LocalFree(argv);
        return -1;
    }
    link_cli_prepare();
    if (result_file) link_cli_result_open(result_file);
    link_cli_line("HD2 Macropad Link v1.2.6 first-install");
    if (bad || (yes && dry)) {
        link_cli_result("RESULT rc=2 error=\"usage: --install [--port COMn] --yes [--dry-run] [--log-stdout] [--rom-only] [--result-file f]. --port may be omitted when exactly one CH340 is connected\"");
        code = 2;
        goto done;
    }
    if (!yes && !dry) {
        link_cli_result("RESULT rc=2 error=\"writing needs --yes; --dry-run only reads the 16 MB backup\"");
        code = 2;
        goto done;
    }
    mtx = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\HD2MacropadLink");
    if (mtx) {
        CloseHandle(mtx);
        link_cli_result("RESULT rc=3 error=\"HD2 Macropad Link is running in the tray. Quit it first (right-click, Exit).\"");
        code = 3;
        goto done;
    }
    if (!port) {
        n = enum_ch340(ports, 8);
        if (n == 1) port = ports[0];
        else if (n == 0) {
            link_cli_result("RESULT rc=1 error=\"no CH340 serial device is connected. The USB-C cable must carry data, not only power. A charge-only cable can light the panel and still show no COM port. Nothing was written.\"");
            code = 1;
            goto done;
        } else {
            link_cli_result("RESULT rc=2 error=\"more than one CH340 is connected. Pass --port COMn. Nothing was written.\"");
            code = 2;
            goto done;
        }
    }
    if (!cli_port_ok(port)) {
        link_cli_result("RESULT rc=2 error=\"--port must look like COM3\"");
        code = 2;
        goto done;
    }
    lstrcpynW(portbuf, port, 16);
    memset(&ui, 0, sizeof ui);
    ui.package_ok = package_ok(&ui);
    if (!ui.package_ok) {
        one_line(ui.package_err, errq, sizeof errq);
        link_cli_result("RESULT rc=4 error=\"%s\"", errq);
        free(ui.boot); free(ui.part); free(ui.app); free(ui.assets);
        code = 4;
        goto done;
    }
    ui.cancel = 0;
    code = run_job(&ui, portbuf, dry ? 0 : 1, 0, 1, log_stdout, rom_only);
    {
        char bk[MAX_PATH * 3] = "";
        WideCharToMultiByte(CP_UTF8, 0, ui.job.log_path, -1, logp, (int)sizeof logp, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, ui.job.bk.path, -1, bk, (int)sizeof bk, NULL, NULL);
        one_line(ui.job.ir.error, errq, sizeof errq);
        if (code == 0) {
            link_cli_result("RESULT rc=0 wrote=%d verified=%d confirmed=%d version=%s nvs_written=0 stub=%d stub_fallback=%d backup=\"%s\" log=\"%s\" message=\"%s\"",
                            ui.job.ir.wrote, ui.job.ir.verified, ui.job.ir.confirmed,
                            ui.job.ir.confirmed_version[0] ? ui.job.ir.confirmed_version : "?",
                            ui.job.ir.used_stub, ui.job.ir.stub_fallback, bk, logp, errq);
            code = 0;
        } else if (code == -3) {
            link_cli_result("RESULT rc=3 error=\"port busy or missing\" log=\"%s\"", logp);
            code = 3;
        } else {
            link_cli_result("RESULT rc=1 wrote=%d verified=%d restored=%d confirmed=%d nvs_written=0 stub=%d stub_fallback=%d backup=\"%s\" log=\"%s\" error=\"%s\"",
                            ui.job.ir.wrote, ui.job.ir.verified, ui.job.ir.restored, ui.job.ir.confirmed,
                            ui.job.ir.used_stub, ui.job.ir.stub_fallback, bk, logp, errq);
            code = 1;
        }
    }
    free(ui.boot);
    free(ui.part);
    free(ui.app);
    free(ui.assets);
done:
    link_cli_result_close();
    LocalFree(argv);
    return code;
}
