/*
 * Explicit firmware update for HD2 Macropad Link.
 * Stable checks use the public GitHub releases API with no token.
 * Nothing is written until the user presses Update. There is no timer.
 * The Link executable is not replaced by this code.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winhttp.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "updater_ui.h"
#include "update_core.h"
#include "romflash.h"
#include "link_session.h"
#include "sha256.h"
#include "md5.h"
#include <stdarg.h>
#include <shellapi.h>

#define DLG_UPDATE 2001
#define DLG_DRY    2002
#define DLG_CANCEL 2003
#define DLG_SAME   2004
#define DLG_TEXT   2005
#define DLG_STATUS 2006
#define DLG_INSTALL 2007
#define DLG_LATER   2008
#define DLG_SKIP    2009

static volatile LONG g_busy;

typedef struct {
    const wchar_t *title;
    wchar_t *body;
    int test_channel;
    int choice;
    int allow_same;
} prompt_t;

typedef struct {
    HANDLE port;
    wchar_t name[16];
    volatile LONG *cancel;
    HWND status;
    FILE *logf;
    wchar_t log_path[MAX_PATH];
    /* v1.2.3: buffered reads. One ReadFile per burst, not per byte. */
    uint8_t rbuf[65536];
    DWORD rpos, rlen;
    int cur_to;
    DWORD comm_errors;
    /* v1.2.3: local backup lookup and headless mode */
    wchar_t backup_dir[MAX_PATH];
    int cli;          /* print progress lines to stdout */
    int log_stdout;   /* echo protocol log lines to stdout */
    int full_chip;    /* first-install: 16 MB backup wording and layout phases */
    wchar_t log_kind[16];
    char last_phase[16];
    unsigned last_tenth;
} ser_t;

/* ---- headless output (v1.2.3 CLI) ---- */
static HANDLE g_out = NULL;

static void cli_out(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    int n;
    DWORD w;
    if (!g_out || g_out == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_out, buf, (DWORD)n, &w, NULL);
}

static int ser_write(void *ctx, const uint8_t *data, size_t n)
{
    ser_t *s = ctx;
    size_t off = 0;
    while (off < n) {
        DWORD w = 0;
        DWORD chunk = (DWORD)((n - off) > 4096 ? 4096 : (n - off));
        if (!WriteFile(s->port, data + off, chunk, &w, NULL) || w == 0) return -1;
        off += w;
    }
    return 0;
}

static int ser_set_timeout(ser_t *s, int timeout_ms)
{
    COMMTIMEOUTS to;
    if (s->cur_to == timeout_ms) return 0;
    /* Do not use the MAXDWORD/MAXDWORD/n special case. CH340 drivers often
     * ignore it or treat MAXDWORD as a multi-minute (or unbounded) wait. */
    ZeroMemory(&to, sizeof to);
    if (timeout_ms <= 0) {
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = 0;
    } else {
        to.ReadIntervalTimeout = 0;
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
    }
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 5000;
    if (!SetCommTimeouts(s->port, &to)) return -1;
    s->cur_to = timeout_ms;
    return 0;
}

/* Bytes already in the driver queue are fetched in one ReadFile (it returns
 * at once because they are there). Only an empty queue waits, for one byte,
 * up to timeout_ms. Same semantics for the caller as v1.2.2. */
static int ser_fill(ser_t *s, int timeout_ms)
{
    COMSTAT cs;
    DWORD errs = 0, got = 0;
    s->rpos = s->rlen = 0;
    ZeroMemory(&cs, sizeof cs);
    if (ClearCommError(s->port, &errs, &cs)) s->comm_errors |= errs;
    if (cs.cbInQue > 0) {
        DWORD want = cs.cbInQue < sizeof s->rbuf ? cs.cbInQue : (DWORD)sizeof s->rbuf;
        if (ser_set_timeout(s, timeout_ms > 0 ? timeout_ms : 50) != 0) return -1;
        if (!ReadFile(s->port, s->rbuf, want, &got, NULL)) return -1;
        s->rlen = got;
        return (int)got;
    }
    if (ser_set_timeout(s, timeout_ms) != 0) return -1;
    if (!ReadFile(s->port, s->rbuf, 1, &got, NULL)) return -1;
    s->rlen = got;
    if (got == 1) {
        ZeroMemory(&cs, sizeof cs);
        if (ClearCommError(s->port, &errs, &cs)) s->comm_errors |= errs;
        if (cs.cbInQue > 0) {
            DWORD more = 0, want = cs.cbInQue < sizeof s->rbuf - 1 ? cs.cbInQue : (DWORD)sizeof s->rbuf - 1;
            if (ReadFile(s->port, s->rbuf + 1, want, &more, NULL)) s->rlen += more;
        }
    }
    return (int)s->rlen;
}

static int ser_read(void *ctx, uint8_t *data, size_t n, int timeout_ms)
{
    ser_t *s = ctx;
    size_t have;
    if (s->rpos >= s->rlen) {
        int r = ser_fill(s, timeout_ms);
        if (r <= 0) return r;
    }
    have = s->rlen - s->rpos;
    if (have > n) have = n;
    memcpy(data, s->rbuf + s->rpos, have);
    s->rpos += (DWORD)have;
    return (int)have;
}

static int ser_baud(void *ctx, uint32_t baud)
{
    ser_t *s = ctx;
    DCB dcb;
    if (baud > UPD_FLASH_BAUD_MAX || baud < 9600) return -1;
    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(s->port, &dcb)) return -1;
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    return SetCommState(s->port, &dcb) ? 0 : -1;
}

static void ser_lines(void *ctx, int dtr, int rts)
{
    ser_t *s = ctx;
    EscapeCommFunction(s->port, dtr ? SETDTR : CLRDTR);
    EscapeCommFunction(s->port, rts ? SETRTS : CLRRTS);
    /* Ask Windows to send both line states together. */
    EscapeCommFunction(s->port, dtr ? SETDTR : CLRDTR);
}

static void ser_flush(void *ctx)
{
    ser_t *s = ctx;
    PurgeComm(s->port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s->rpos = s->rlen = 0;
}

static unsigned ser_now(void *ctx)
{
    (void)ctx;
    return (unsigned)GetTickCount64();
}

static void ser_sleep(void *ctx, unsigned ms)
{
    (void)ctx;
    Sleep(ms);
}

static void ser_progress(void *ctx, const char *phase, unsigned done, unsigned total)
{
    ser_t *s = ctx;
    wchar_t msg[384];
    const wchar_t *label = L"Working";
    const char *cli_label = phase;
    if (strcmp(phase, "sync") == 0) {
        if (s->full_chip)
            _snwprintf(msg, 384,
                       L"Waiting for the bootloader, attempt %u of %u. This should keep moving, and it should succeed within about a minute. If it stops, check the cable, the CH340 driver, and that no other program has the port.",
                       done, total);
        else
            _snwprintf(msg, 384, L"Waiting for the bootloader, attempt %u of %u. Do not unplug the macropad.",
                       done, total);
        if (s->status) SetWindowTextW(s->status, msg);
        if (s->cli && strcmp(s->last_phase, "sync") != 0) cli_out("PROGRESS phase=Connect");
        snprintf(s->last_phase, sizeof s->last_phase, "%s", phase);
        return;
    }
    if (strcmp(phase, "backup") == 0) {
        label = s->full_chip
            ? L"Backup: whole 16 MB flash. About 6 minutes 40 seconds on the maintainer PC. Wait while the count moves"
            : L"Backup: saving the current app";
        cli_label = "Backup";
    } else if (strcmp(phase, "write") == 0) { label = L"Write: installing the new app"; cli_label = "Write"; }
    else if (strcmp(phase, "write-boot") == 0) {
        label = s->full_chip ? L"Install: bootloader. Expected a few minutes for all writes together. Wait while the count moves" : L"Install: writing the bootloader at 0x0";
        cli_label = "WriteBoot";
    }
    else if (strcmp(phase, "write-part") == 0) {
        label = s->full_chip ? L"Install: partition table. Expected a few minutes for all writes together. Wait while the count moves" : L"Install: writing the partition table at 0x8000";
        cli_label = "WritePart";
    }
    else if (strcmp(phase, "write-app") == 0) {
        label = s->full_chip ? L"Install: app. Expected a few minutes for all writes together. Wait while the count moves" : L"Install: writing the app at 0x10000";
        cli_label = "WriteApp";
    }
    else if (strcmp(phase, "write-assets") == 0) {
        label = s->full_chip ? L"Install: sounds. Expected a few minutes for all writes together. Wait while the count moves" : L"Install: writing sounds at 0xA10000";
        cli_label = "WriteAssets";
    }
    else if (strcmp(phase, "verify") == 0) { label = L"Verify: checking the flash MD5"; cli_label = "Verify"; }
    else if (strcmp(phase, "verify-boot") == 0) {
        label = s->full_chip ? L"Verify: bootloader. Expected a few minutes for the checks. Wait while this moves" : L"Verify: bootloader MD5";
        cli_label = "VerifyBoot";
    }
    else if (strcmp(phase, "verify-part") == 0) {
        label = s->full_chip ? L"Verify: partition table. Expected a few minutes for the checks. Wait while this moves" : L"Verify: partition table MD5";
        cli_label = "VerifyPart";
    }
    else if (strcmp(phase, "verify-app") == 0) {
        label = s->full_chip ? L"Verify: app. Expected a few minutes for the checks. Wait while this moves" : L"Verify: app MD5";
        cli_label = "VerifyApp";
    }
    else if (strcmp(phase, "verify-assets") == 0) {
        label = s->full_chip ? L"Verify: sounds. Expected a few minutes for the checks. Wait while this moves" : L"Verify: sound partition MD5";
        cli_label = "VerifyAssets";
    }
    else if (strcmp(phase, "restore") == 0) {
        label = s->full_chip ? L"Rollback: writing the backup back. This can take several minutes. Wait while the count moves" : L"Rollback: writing the backup back";
        cli_label = "Rollback";
    }
    else if (strcmp(phase, "readback") == 0) { label = L"Verify: reading the new app back"; cli_label = "Verify"; }
    else if (strcmp(phase, "check") == 0) { label = L"Checking the app on the board"; cli_label = "Check"; }
    else if (strcmp(phase, "stub") == 0) { label = L"Loading the fast flasher"; cli_label = "Stub"; }
    else if (strcmp(phase, "reset") == 0) { label = L"Restarting into the bootloader"; cli_label = "Reset"; }
    else if (strcmp(phase, "confirm") == 0) { label = L"Confirming the version after reset"; cli_label = "Confirm"; }
    if (total > 1) {
        unsigned pct = (unsigned)(((unsigned long long)done * 100u) / total);
        _snwprintf(msg, 384, L"%s\n%u of %u KB (%u%%). Do not unplug the macropad.",
                   label, done / 1024u, total / 1024u, pct);
    } else {
        _snwprintf(msg, 384, L"%s. Do not unplug the macropad.", label);
    }
    if (s->status) SetWindowTextW(s->status, msg);
    if (s->cli) {
        unsigned tenth = total ? (unsigned)(((unsigned long long)done * 10u) / total) : 0;
        if (strcmp(s->last_phase, phase) != 0 || tenth != s->last_tenth) {
            if (total > 1)
                cli_out("PROGRESS phase=%s %u/%u %u%%", cli_label, done, total,
                        (unsigned)(((unsigned long long)done * 100u) / total));
            else
                cli_out("PROGRESS phase=%s", cli_label);
            s->last_tenth = tenth;
        }
    }
    snprintf(s->last_phase, sizeof s->last_phase, "%s", phase);
}

static HANDLE open_flash_port(const wchar_t *name);

static int ser_cancelled(void *ctx)
{
    ser_t *s = ctx;
    return s->cancel && *s->cancel;
}

static void ser_log(void *ctx, const char *msg)
{
    ser_t *s = ctx;
    SYSTEMTIME st;
    if (!s || !msg) return;
    if (!s->logf) {
        if (s->log_stdout) cli_out("LOG %s", msg);
        return;
    }
    GetLocalTime(&st);
    fprintf(s->logf, "%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fflush(s->logf);
    if (s->log_stdout)
        cli_out("LOG %02u:%02u:%02u.%03u %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
}

static int open_update_log(ser_t *s)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME st;
    const wchar_t *kind;
    if (!s) return -1;
    kind = s->log_kind[0] ? s->log_kind : L"update";
    s->logf = NULL;
    s->log_path[0] = 0;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return -1;
    GetLocalTime(&st);
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink\\logs", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink\\logs\\%s-%04u%02u%02u-%02u%02u%02u.log",
               dir, kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    s->logf = _wfopen(path, L"w");
    if (!s->logf) return -1;
    lstrcpynW(s->log_path, path, MAX_PATH);
    return 0;
}

static int ser_reopen(void *ctx)
{
    ser_t *s = ctx;
    HANDLE h;
    if (s->port && s->port != INVALID_HANDLE_VALUE) {
        CloseHandle(s->port);
        s->port = INVALID_HANDLE_VALUE;
    }
    h = open_flash_port(s->name);
    s->rpos = s->rlen = 0;
    s->cur_to = -99;
    if (h == INVALID_HANDLE_VALUE) return -1;
    s->port = h;
    return 0;
}

static HANDLE open_flash_port(const wchar_t *name)
{
    wchar_t path[32];
    _snwprintf(path, 32, L"\\\\.\\%s", name);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DCB dcb;
    COMMTIMEOUTS to;
    if (h == INVALID_HANDLE_VALUE) return h;
    SetupComm(h, 65536, 65536); /* room for a 4 KB stub READ_FLASH packet and more */
    EscapeCommFunction(h, CLRRTS);
    EscapeCommFunction(h, CLRDTR);
    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = UPD_LINK_BAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    ZeroMemory(&to, sizeof to);
    to.ReadIntervalTimeout = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 50;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

static int crack_https(const char *url, wchar_t *host, size_t host_cap, wchar_t *path, size_t path_cap)
{
    const char *p, *slash, *colon;
    size_t host_len, i;
    if (!upd_https_host_ok(url)) return 0;
    p = url + 8;
    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;
    if (colon) host_len = (size_t)(colon - p);
    else if (slash) host_len = (size_t)(slash - p);
    else host_len = strlen(p);
    if (host_len == 0 || host_len + 1 > host_cap) return 0;
    for (i = 0; i < host_len; i++) host[i] = (wchar_t)(unsigned char)p[i];
    host[host_len] = 0;
    if (!slash) {
        if (path_cap < 2) return 0;
        path[0] = L'/';
        path[1] = 0;
        return 1;
    }
    if (strlen(slash) + 1 > path_cap) return 0;
    for (i = 0; slash[i]; i++) path[i] = (wchar_t)(unsigned char)slash[i];
    path[i] = 0;
    return 1;
}

static int winhttp_get(const char *url, uint8_t **out, size_t *out_len, size_t max_len, char *err, size_t err_cap)
{
    HINTERNET ses;
    char current[512];
    int hops;
    *out = NULL;
    *out_len = 0;
    if (!url || strlen(url) >= sizeof current) {
        snprintf(err, err_cap, "download address was rejected");
        return 0;
    }
    snprintf(current, sizeof current, "%s", url);
    ses = WinHttpOpen(L"HD2MacropadLink/1.2.7", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        snprintf(err, err_cap, "Windows HTTP could not start");
        return 0;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 120000);
    for (hops = 0; hops < 5; hops++) {
        wchar_t host[160], path[400];
        HINTERNET con = NULL, req = NULL;
        DWORD status = 0, sz = sizeof status, opt = WINHTTP_DISABLE_REDIRECTS;
        wchar_t location[512];
        DWORD loc_bytes = 0;
        uint8_t *body = NULL;
        size_t body_n = 0;
        if (!crack_https(current, host, 160, path, 400)) {
            snprintf(err, err_cap, "refusing a download outside GitHub");
            WinHttpCloseHandle(ses);
            return 0;
        }
        con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) break;
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) {
            WinHttpCloseHandle(con);
            break;
        }
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof opt);
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, NULL) ||
            !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            snprintf(err, err_cap, "download failed before a response");
            WinHttpCloseHandle(ses);
            return 0;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            loc_bytes = sizeof location;
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location, &loc_bytes, WINHTTP_NO_HEADER_INDEX)) {
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(con);
                snprintf(err, err_cap, "download redirect had no location");
                WinHttpCloseHandle(ses);
                return 0;
            }
            WideCharToMultiByte(CP_UTF8, 0, location, -1, current, (int)sizeof current, NULL, NULL);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            continue;
        }
        if (status == 403 || status == 429) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            snprintf(err, err_cap, "GitHub refused the request (HTTP %lu). Nothing was written.", status);
            return 0;
        }
        if (status != 200) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            snprintf(err, err_cap, "download returned HTTP %lu. Nothing was written.", status);
            return 0;
        }
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (avail == 0) break;
            if (body_n + avail > max_len) {
                free(body);
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(con);
                WinHttpCloseHandle(ses);
                snprintf(err, err_cap, "download is larger than allowed");
                return 0;
            }
            {
                uint8_t *next = (uint8_t *)realloc(body, body_n + avail);
                if (!next) {
                    free(body);
                    WinHttpCloseHandle(req);
                    WinHttpCloseHandle(con);
                    WinHttpCloseHandle(ses);
                    snprintf(err, err_cap, "out of memory during download");
                    return 0;
                }
                body = next;
            }
            if (!WinHttpReadData(req, body + body_n, avail, &got)) {
                free(body);
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(con);
                WinHttpCloseHandle(ses);
                snprintf(err, err_cap, "download stopped early");
                return 0;
            }
            body_n += got;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        *out = body;
        *out_len = body_n;
        return 1;
    }
    WinHttpCloseHandle(ses);
    snprintf(err, err_cap, "too many download redirects");
    return 0;
}

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    prompt_t *p = (prompt_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (!p) return 0;
        if (LOWORD(wp) == DLG_UPDATE) {
            HWND box = GetDlgItem(hwnd, DLG_SAME);
            p->allow_same = box && SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
            p->choice = 1;
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == DLG_DRY) {
            p->choice = 2;
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == DLG_CANCEL || LOWORD(wp) == IDCANCEL) {
            p->choice = 0;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (p) p->choice = 0;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int show_prompt(HWND owner, prompt_t *p)
{
    WNDCLASSW wc;
    HWND w, edit, same = NULL, update, dry, cancel;
    HFONT font;
    RECT wa;
    MSG msg;
    int width = 560, height = p->test_channel ? 460 : 430;
    (void)owner;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HD2MacropadUpdatePrompt";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, p->title,
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        wa.left + 40, wa.top + 40, width, height,
                        NULL, NULL, wc.hInstance, NULL);
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);
    CreateWindowExW(0, L"STATIC", p->test_channel
                    ? L"LOCAL TEST PACKAGE. This is not a published release and will not be offered by Check for firmware update."
                    : L"Stable channel. The macropad is not changed until you press Update.",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, 12, width - 40, 36, w, NULL, wc.hInstance, NULL);
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->body,
                           WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                           16, 52, width - 40, 240, w, (HMENU)(INT_PTR)DLG_TEXT, wc.hInstance, NULL);
    if (p->test_channel) {
        same = CreateWindowExW(0, L"BUTTON", L"Reinstall even if the board already shows this version",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               16, 304, width - 40, 24, w, (HMENU)(INT_PTR)DLG_SAME, wc.hInstance, NULL);
    }
    CreateWindowExW(0, L"STATIC",
                    L"Update restarts the macropad. Leave the game first. It writes only the app at 0x10000 and does not erase presets. A USB power loss during the write is not an atomic update.",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, p->test_channel ? 332 : 304, width - 40, 48, w, NULL, wc.hInstance, NULL);
    update = CreateWindowExW(0, L"BUTTON", L"Update", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                             16, height - 78, 120, 28, w, (HMENU)(INT_PTR)DLG_UPDATE, wc.hInstance, NULL);
    dry = CreateWindowExW(0, L"BUTTON", L"Check board only", WS_CHILD | WS_VISIBLE,
                          148, height - 78, 150, 28, w, (HMENU)(INT_PTR)DLG_DRY, wc.hInstance, NULL);
    cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                             width - 140, height - 78, 100, 28, w, (HMENU)(INT_PTR)DLG_CANCEL, wc.hInstance, NULL);
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(update, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(dry, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    if (same) SendMessageW(same, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(w, SW_SHOWNORMAL);
    SetForegroundWindow(w);
    while (IsWindow(w) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return p->choice;
}

typedef struct {
    HWND status;
    volatile LONG cancel;
    const uint8_t *image;
    uint32_t image_len;
    const char *version;
    int allow_same;
    int do_write;
    wchar_t port[16];
    wchar_t backup_dir[MAX_PATH];
    wchar_t backup_path[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    rom_result rr;
    int rc;
    /* v1.2.3 */
    int cli, log_stdout, full_backup, rom_only;
    unsigned ms_total;
    wchar_t installed_path[MAX_PATH];
} job_t;

static int backup_cb(void *ctx, const uint8_t *data, size_t n)
{
    job_t *job = ctx;
    FILE *f;
    uint8_t dig[32];
    char hex[80];
    wchar_t side[MAX_PATH];
    FILE *sf;
    SYSTEMTIME st;
    const char *ver = job->rr.device_version[0] ? job->rr.device_version : "unknown";
    if (n == 0 || n > UPD_FACTORY_SIZE) return -1;
    if (!job->backup_dir[0]) return -1;
    GetLocalTime(&st);
    _snwprintf(job->backup_path, MAX_PATH, L"%s\\app-%hs-%04u%02u%02u-%02u%02u%02u.bin",
               job->backup_dir, ver, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    f = _wfopen(job->backup_path, L"wb");
    if (!f) return -1;
    if (fwrite(data, 1, n, f) != n || fflush(f) != 0) {
        fclose(f);
        return -1;
    }
    if (_ftelli64(f) != (__int64)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    sha256_buf(data, n, dig);
    sha256_hex(data, n, hex, sizeof hex);
    _snwprintf(side, MAX_PATH, L"%s.sha256", job->backup_path);
    sf = _wfopen(side, L"wb");
    if (!sf) return -1;
    fprintf(sf, "%s  app-backup.bin\n", hex);
    if (fclose(sf) != 0) return -1;
    (void)dig;
    return 0;
}

static DWORD reg_dword(const wchar_t *name)
{
    HKEY k;
    DWORD v = 0, sz = sizeof v;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 0;
        RegCloseKey(k);
    }
    return v;
}

/* Look in the backups folder for a file of exactly len bytes with this MD5.
 * Earlier backups and installed-*.bin copies both count. romflash re-checks. */
static int ser_find_backup(void *ctx, uint32_t len, const uint8_t md5[16], uint8_t *out)
{
    ser_t *s = ctx;
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = -1, seen = 0;
    if (!s->backup_dir[0]) return -1;
    _snwprintf(pattern, MAX_PATH, L"%s\\*.bin", s->backup_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        FILE *f;
        uint8_t d[16];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow != len) continue;
        if (++seen > 64) break;
        _snwprintf(path, MAX_PATH, L"%s\\%s", s->backup_dir, fd.cFileName);
        f = _wfopen(path, L"rb");
        if (!f) continue;
        if (fread(out, 1, len, f) == len) {
            md5_buf(out, len, d);
            if (memcmp(d, md5, 16) == 0) {
                char name8[MAX_PATH];
                char line[MAX_PATH + 64];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name8, (int)sizeof name8, NULL, NULL);
                snprintf(line, sizeof line, "local backup match: %s", name8);
                ser_log(s, line);
                found = 0;
            }
        }
        fclose(f);
    } while (found != 0 && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* After a verified install, keep a copy of what is now on the board, so the next
 * update can take its backup from this PC (checked by MD5) instead of reading 2.4 MB. */
static void save_installed_copy(job_t *job)
{
    wchar_t tmp[MAX_PATH];
    FILE *f;
    if (!job->backup_dir[0] || !job->version) return;
    _snwprintf(job->installed_path, MAX_PATH, L"%s\\installed-%hs.bin", job->backup_dir, job->version);
    _snwprintf(tmp, MAX_PATH, L"%s.tmp", job->installed_path);
    f = _wfopen(tmp, L"wb");
    if (!f) {
        job->installed_path[0] = 0;
        return;
    }
    if (fwrite(job->image, 1, job->image_len, f) != job->image_len || fclose(f) != 0 ||
        !MoveFileExW(tmp, job->installed_path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        job->installed_path[0] = 0;
    }
}

static void ser_setup(ser_t *ser, rom_io *io, HANDLE h, const wchar_t *port, volatile LONG *cancel, HWND status)
{
    ZeroMemory(ser, sizeof *ser);
    ser->port = h;
    ser->cur_to = -99;
    lstrcpynW(ser->name, port, 16);
    ser->cancel = cancel;
    ser->status = status;
    ZeroMemory(io, sizeof *io);
    io->ctx = ser;
    io->write = ser_write;
    io->read = ser_read;
    io->set_baud = ser_baud;
    io->set_dtr_rts = ser_lines;
    io->flush_rx = ser_flush;
    io->sleep_ms = ser_sleep;
    io->progress = ser_progress;
    io->cancelled = ser_cancelled;
    io->reopen = ser_reopen;
    io->log = ser_log;
    io->now_ms = ser_now;
}

static void ser_close(ser_t *ser)
{
    if (ser->logf) {
        fclose(ser->logf);
        ser->logf = NULL;
    }
    if (ser->port && ser->port != INVALID_HANDLE_VALUE) {
        EscapeCommFunction(ser->port, CLRRTS);
        EscapeCommFunction(ser->port, CLRDTR);
        CloseHandle(ser->port);
        ser->port = INVALID_HANDLE_VALUE;
    }
}

static DWORD WINAPI job_thread(LPVOID arg)
{
    job_t *job = arg;
    static ser_t ser; /* 64 KB read buffer; one update runs at a time (g_busy) */
    rom_io io;
    unsigned t0 = (unsigned)GetTickCount64();
    HANDLE h = open_flash_port(job->port);
    if (h == INVALID_HANDLE_VALUE) {
        char port8[16];
        WideCharToMultiByte(CP_UTF8, 0, job->port, -1, port8, (int)sizeof port8, NULL, NULL);
        snprintf(job->rr.error, sizeof job->rr.error,
                 "%s is busy or missing. Close anything else using it. Nothing was written.", port8);
        job->rc = -1;
        return 0;
    }
    ser_setup(&ser, &io, h, job->port, &job->cancel, job->status);
    ser.cli = job->cli;
    ser.log_stdout = job->log_stdout;
    lstrcpynW(ser.backup_dir, job->backup_dir, MAX_PATH);
    open_update_log(&ser);
    lstrcpynW(job->log_path, ser.log_path, MAX_PATH);
    io.find_backup = ser_find_backup;
    io.full_backup = job->full_backup || reg_dword(L"FullBackup") != 0;
    io.rom_only = job->rom_only || reg_dword(L"RomOnly") != 0;
    {
        char port8[16];
        char line[200];
        WideCharToMultiByte(CP_UTF8, 0, job->port, -1, port8, (int)sizeof port8, NULL, NULL);
        ser_log(&ser, "HD2 Macropad Link v1.2.7 updater log");
        snprintf(line, sizeof line, "port=%s do_write=%d allow_same=%d package=%s full_backup=%d rom_only=%d mode=%s",
                 port8, job->do_write, job->allow_same, job->version ? job->version : "", io.full_backup, io.rom_only,
                 job->cli ? "headless" : "dialog");
        ser_log(&ser, line);
    }
    job->rc = rom_update_app(&io, job->image, job->image_len, job->version, job->allow_same, job->do_write,
                             job->do_write ? backup_cb : NULL, job, &job->rr);
    job->ms_total = (unsigned)GetTickCount64() - t0;
    if (job->rc == 0 && job->do_write && job->rr.verified) save_installed_copy(job);
    {
        char line[700];
        snprintf(line, sizeof line,
                 "finished rc=%d wrote=%d verified=%d verify_ran=%d restored=%d confirmed=%d used_stub=%d stub_fallback=%d "
                 "backup_source=\"%s\" ms_connect=%u ms_backup=%u ms_write=%u ms_verify=%u ms_confirm=%u ms_total=%u "
                 "comm_errors=0x%lX error=%s",
                 job->rc, job->rr.wrote, job->rr.verified, job->rr.verify_ran, job->rr.restored, job->rr.confirmed,
                 job->rr.used_stub, job->rr.stub_fallback, job->rr.backup_source, job->rr.ms_connect, job->rr.ms_backup,
                 job->rr.ms_write, job->rr.ms_verify, job->rr.ms_confirm, job->ms_total, (unsigned long)ser.comm_errors,
                 job->rr.error[0] ? job->rr.error : "(none)");
        ser_log(&ser, line);
    }
    ser_close(&ser);
    return 0;
}

static void run_job(HWND owner, job_t *job)
{
    WNDCLASSW wc;
    HWND w, status, cancel;
    HFONT font;
    HANDLE th;
    MSG msg;
    (void)owner;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HD2MacropadUpdateRun";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"HD2 Macropad firmware update",
                        WS_OVERLAPPED | WS_CAPTION,
                        80, 80, 520, 180, NULL, NULL, wc.hInstance, NULL);
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    status = CreateWindowExW(0, L"STATIC", L"Starting. The macropad will restart.",
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             16, 16, 480, 64, w, (HMENU)(INT_PTR)DLG_STATUS, wc.hInstance, NULL);
    cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                             390, 96, 100, 28, w, (HMENU)(INT_PTR)DLG_CANCEL, wc.hInstance, NULL);
    SendMessageW(status, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    job->status = status;
    ShowWindow(w, SW_SHOWNORMAL);
    th = CreateThread(NULL, 0, job_thread, job, 0, NULL);
    while (th && WaitForSingleObject(th, 50) == WAIT_TIMEOUT) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_COMMAND && LOWORD(msg.wParam) == DLG_CANCEL)
                InterlockedExchange(&job->cancel, 1);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (th) CloseHandle(th);
    DestroyWindow(w);
}

static int load_file(const wchar_t *path, uint8_t **out, size_t *out_len, size_t max_len, char *err, size_t err_cap)
{
    FILE *f = _wfopen(path, L"rb");
    long sz;
    *out = NULL;
    *out_len = 0;
    if (!f) {
        snprintf(err, err_cap, "could not open the file");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || (size_t)sz > max_len) {
        fclose(f);
        snprintf(err, err_cap, "file is missing or too large");
        return 0;
    }
    rewind(f);
    *out = (uint8_t *)malloc((size_t)sz + 1);
    if (!*out || fread(*out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out);
        *out = NULL;
        fclose(f);
        snprintf(err, err_cap, "could not read the file");
        return 0;
    }
    (*out)[sz] = 0;
    *out_len = (size_t)sz;
    fclose(f);
    return 1;
}

static int prepare_image(const upd_manifest *m, const uint8_t *image, size_t n, char *err, size_t err_cap)
{
    char ver[32];
    uint32_t total = 0;
    if (!upd_sha256_file_ok(m, image, n, err, err_cap)) return 0;
    if (!upd_image_length(image, n, &total, err, err_cap)) return 0;
    if (!upd_image_version(image, n, ver, sizeof ver, err, err_cap)) return 0;
    if (strcmp(ver, m->version) != 0) {
        snprintf(err, err_cap, "the file version is %s, not %s", ver, m->version);
        return 0;
    }
    return 1;
}

static void widen(const char *src, wchar_t *dst, size_t cap)
{
    if (!src) src = "";
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
}

static void finish_dialog(const wchar_t *backup, const wchar_t *log_path, const rom_result *rr, int rc)
{
    wchar_t msg[1600];
    wchar_t errw[700];
    const wchar_t *log = (log_path && log_path[0]) ? log_path : L"(no log file)";
    {
        char e2[700];
        snprintf(e2, sizeof e2, "%s\n\nLoader: %s. Backup from: %s.", rr->error,
                 rr->used_stub ? "fast flasher stub" : (rr->stub_fallback ? "ROM (stub unavailable, fallback)" : "ROM"),
                 rr->backup_source[0] ? rr->backup_source : "(none)");
        widen(e2, errw, 700);
    }
    if (rc == 0) {
        _snwprintf(msg, 1600,
                   L"%s\n\nBackup:\n%s\n\nLog:\n%s\n\nThe link will look for the macropad again. Set Connectivity to USB if the icon does not return. Presets were not erased.",
                   errw, backup[0] ? backup : L"(no backup, board was not written)", log);
    } else {
        _snwprintf(msg, 1600,
                   L"%s\n\nBackup, if one was saved:\n%s\n\nLog:\n%s\n\nRecovery, if the screen stays blank: hold BOOT, tap RESET, release BOOT, then write that backup at 0x10000 with esptool at 115200. Do not erase the chip. A power loss during the write is not automatically recoverable.",
                   errw, backup[0] ? backup : L"(none)", log);
    }
    MessageBoxW(NULL, msg, L"HD2 Macropad firmware update", rc == 0 ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
}

static void execute(HWND owner, const upd_manifest *m, const uint8_t *image, size_t image_len, int test_channel)
{
    prompt_t prompt;
    wchar_t body[2600];
    wchar_t notes[2100];
    wchar_t port[16], perr[200];
    job_t job;
    int choice;
    {
        wchar_t ver[40], channel[20], product[48], sha[80];
        widen(m->version, ver, 40);
        widen(m->channel, channel, 20);
        widen(m->product, product, 48);
        widen(m->sha256_hex, sha, 80);
        widen(m->notes, notes, 2100);
        _snwprintf(body, 2600,
                   L"Package %s\r\nChannel: %s\r\nProduct: %s\r\nApp offset 0x10000, slot ends at 0xA10000.\r\nSHA-256 %s\r\n\r\n%s\r\n\r\nCheck board only restarts the macropad and does not write.\r\nOriginal HD2 Macropad by Erik Lembke. This does not change that.",
                   ver, channel, product, sha, notes);
    }
    ZeroMemory(&prompt, sizeof prompt);
    prompt.title = test_channel ? L"Install local test package" : L"CrowPanel firmware update";
    prompt.body = body;
    prompt.test_channel = test_channel;
    choice = show_prompt(owner, &prompt);
    if (choice == 0) return;
    if (!link_pause_for_update(port, 16, perr, 200)) {
        MessageBoxW(owner, perr, L"HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        return;
    }
    ZeroMemory(&job, sizeof job);
    job.image = image;
    job.image_len = (uint32_t)image_len;
    job.version = m->version;
    job.allow_same = test_channel && prompt.allow_same;
    job.do_write = choice == 1;
    lstrcpynW(job.port, port, 16);
    if (job.do_write) {
        wchar_t dir[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            link_resume_after_update();
            MessageBoxW(owner, L"LOCALAPPDATA is not set, so a backup path could not be chosen. Nothing was written.",
                        L"HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
            return;
        }
        _snwprintf(job.backup_dir, MAX_PATH, L"%s\\HD2MacropadLink", dir);
        CreateDirectoryW(job.backup_dir, NULL);
        _snwprintf(job.backup_dir, MAX_PATH, L"%s\\HD2MacropadLink\\backups", dir);
        CreateDirectoryW(job.backup_dir, NULL);
        job.backup_path[0] = 0;
    }
    run_job(owner, &job);
    link_resume_after_update();
    if (job.rr.device_version[0]) {
        /* Persist so the next startup check can compare against a known board version
         * without resetting the pad into the bootloader. */
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, NULL, 0,
                            KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
            wchar_t ver[40];
            widen(job.rr.device_version, ver, 40);
            RegSetValueExW(k, L"LastDeviceVersion", 0, REG_SZ, (const BYTE *)ver,
                           (DWORD)((wcslen(ver) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }
    finish_dialog(job.backup_path, job.log_path, &job.rr, job.rc);
}

void link_updater_check_stable(HWND owner)
{
    uint8_t *list = NULL, *manifest_bytes = NULL, *image = NULL;
    size_t list_n = 0, man_n = 0, image_n = 0;
    char err[200], url[320];
    upd_release_pick pick;
    upd_manifest man;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    if (!winhttp_get("https://api.github.com/repos/" UPD_REPO "/releases?per_page=20",
                     &list, &list_n, 1u << 20, err, sizeof err)) {
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    if (!upd_pick_stable_release((char *)list, list_n, &pick, err, sizeof err)) {
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONINFORMATION);
        goto done;
    }
    if (!upd_release_url(pick.tag, UPD_MANIFEST_NAME, url, sizeof url) ||
        !winhttp_get(url, &manifest_bytes, &man_n, 16384, err, sizeof err) ||
        !upd_manifest_parse((char *)manifest_bytes, man_n, &man, err, sizeof err) ||
        !upd_manifest_constants_ok(&man, "stable", err, sizeof err) ||
        !upd_image_name_ok(&man, err, sizeof err) ||
        strcmp(man.version, pick.tag) != 0) {
        if (!err[0]) snprintf(err, sizeof err, "the stable manifest does not match its release tag");
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    if (!upd_release_url(pick.tag, man.image_name, url, sizeof url) ||
        !winhttp_get(url, &image, &image_n, UPD_FACTORY_SIZE, err, sizeof err) ||
        !prepare_image(&man, image, image_n, err, sizeof err)) {
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    execute(owner, &man, image, image_n, 0);
done:
    free(list);
    free(manifest_bytes);
    free(image);
    InterlockedExchange(&g_busy, 0);
}

void link_updater_local_package(HWND owner)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH] = L"";
    wchar_t image_path[MAX_PATH];
    wchar_t *slash;
    uint8_t *manifest_bytes = NULL, *image = NULL;
    size_t man_n = 0, image_n = 0;
    char err[200];
    upd_manifest man;
    const wchar_t *base;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"CrowPanel test manifest\0crowpanel-update.json\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open crowpanel-update.json from the local test package";
    if (!GetOpenFileNameW(&ofn)) goto done;
    base = wcsrchr(file, L'\\');
    base = base ? base + 1 : file;
    if (_wcsicmp(base, L"crowpanel-update.json") != 0) {
        MessageBoxW(owner, L"The file has to be named crowpanel-update.json.", L"HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    if (!load_file(file, &manifest_bytes, &man_n, 16384, err, sizeof err) ||
        !upd_manifest_parse((char *)manifest_bytes, man_n, &man, err, sizeof err) ||
        !upd_manifest_constants_ok(&man, "test", err, sizeof err) ||
        !upd_image_name_ok(&man, err, sizeof err)) {
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    lstrcpynW(image_path, file, MAX_PATH);
    slash = wcsrchr(image_path, L'\\');
    if (!slash) goto done;
    slash[1] = 0;
    if (wcslen(image_path) + strlen(man.image_name) + 1 >= MAX_PATH) {
        MessageBoxW(owner, L"The image path is too long.", L"HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    {
        wchar_t name[80];
        widen(man.image_name, name, 80);
        wcscat(image_path, name);
    }
    if (!load_file(image_path, &image, &image_n, UPD_FACTORY_SIZE, err, sizeof err) ||
        !prepare_image(&man, image, image_n, err, sizeof err)) {
        MessageBoxA(owner, err, "HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
        goto done;
    }
    execute(owner, &man, image, image_n, 1);
done:
    free(manifest_bytes);
    free(image);
    InterlockedExchange(&g_busy, 0);
}


/* ---------------------------------------------------------------- startup update check */

static void cfg_log_startup(const char *msg)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    SYSTEMTIME st;
    if (!msg || !GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink\\logs", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink\\logs\\startup-check.log", dir);
    f = _wfopen(path, L"a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u  %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    fclose(f);
}

static int cfg_read_str(const wchar_t *name, char *out, size_t cap)
{
    HKEY k;
    wchar_t w[80];
    DWORD type = 0, sz = sizeof w;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)w, &sz) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(k);
        return 0;
    }
    RegCloseKey(k);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = 0;
    return out[0] != 0;
}

static void cfg_write_str(const wchar_t *name, const char *val)
{
    HKEY k;
    wchar_t w[80];
    if (!val) val = "";
    MultiByteToWideChar(CP_UTF8, 0, val, -1, w, 80);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)w, (DWORD)((wcslen(w) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

int link_updater_startup_enabled(void)
{
    HKEY k;
    DWORD on = 1, sz = sizeof on, type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 1;
    if (RegQueryValueExW(k, L"CheckUpdatesAtStartup", NULL, &type, (BYTE *)&on, &sz) != ERROR_SUCCESS || type != REG_DWORD)
        on = 1;
    RegCloseKey(k);
    return on ? 1 : 0;
}

void link_updater_startup_set(int on)
{
    HKEY k;
    DWORD v = on ? 1 : 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"CheckUpdatesAtStartup", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

static int show_offer_dialog(HWND owner, const char *version, const char *device_ver)
{
    WNDCLASSW wc;
    HWND w, text, ins, later, skip;
    HFONT font;
    MSG msg;
    wchar_t body[400], ver[40];
    int choice = 0;
    prompt_t box;
    ZeroMemory(&box, sizeof box);
    box.choice = 0;
    widen(version, ver, 40);
    if (device_ver && device_ver[0]) {
        wchar_t dev[40];
        widen(device_ver, dev, 40);
        _snwprintf(body, 400, L"Update available: %s.\r\nThe last known board version is %s.\r\n\r\nInstall now? Nothing is written until you confirm in the next window.", ver, dev);
    } else {
        _snwprintf(body, 400, L"Update available: %s.\r\nThe last known board version is not stored yet, so this may already be what is on the pad.\r\n\r\nInstall now? Nothing is written until you confirm in the next window.", ver);
    }
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HD2MacropadUpdateOffer";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"HD2 Macropad firmware update",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        100, 100, 480, 220, owner, NULL, wc.hInstance, NULL);
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    text = CreateWindowExW(0, L"STATIC", body, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           16, 16, 440, 90, w, NULL, wc.hInstance, NULL);
    ins = CreateWindowExW(0, L"BUTTON", L"Install now", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          16, 130, 130, 28, w, (HMENU)(INT_PTR)DLG_INSTALL, wc.hInstance, NULL);
    later = CreateWindowExW(0, L"BUTTON", L"Later", WS_CHILD | WS_VISIBLE,
                            160, 130, 130, 28, w, (HMENU)(INT_PTR)DLG_LATER, wc.hInstance, NULL);
    skip = CreateWindowExW(0, L"BUTTON", L"Skip this version", WS_CHILD | WS_VISIBLE,
                           304, 130, 150, 28, w, (HMENU)(INT_PTR)DLG_SKIP, wc.hInstance, NULL);
    SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ins, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(later, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(skip, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(w, SW_SHOWNORMAL);
    SetForegroundWindow(w);
    while (IsWindow(w) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_COMMAND) {
            WORD id = LOWORD(msg.wParam);
            if (id == DLG_INSTALL) { choice = 1; DestroyWindow(w); continue; }
            if (id == DLG_LATER) { choice = 0; DestroyWindow(w); continue; }
            if (id == DLG_SKIP) { choice = 2; DestroyWindow(w); continue; }
        }
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(w)) DestroyWindow(w);
    (void)owner;
    return choice;
}

static DWORD WINAPI startup_check_thread(LPVOID arg)
{
    HWND owner = (HWND)arg;
    uint8_t *list = NULL;
    size_t list_n = 0;
    char err[200], device[32], skipped[32];
    upd_release_pick pick;
    char *offer = NULL;
    if (!link_updater_startup_enabled()) {
        cfg_log_startup("startup check skipped: disabled in the tray");
        return 0;
    }
    cfg_log_startup("startup check: asking GitHub for stable releases");
    if (!winhttp_get("https://api.github.com/repos/" UPD_REPO "/releases?per_page=20",
                     &list, &list_n, 1u << 20, err, sizeof err)) {
        cfg_log_startup(err[0] ? err : "startup check: download failed");
        goto done;
    }
    if (!upd_pick_stable_release((char *)list, list_n, &pick, err, sizeof err)) {
        cfg_log_startup(err[0] ? err : "startup check: no stable package");
        goto done;
    }
    device[0] = 0;
    skipped[0] = 0;
    cfg_read_str(L"LastDeviceVersion", device, sizeof device);
    cfg_read_str(L"SkippedFirmwareVersion", skipped, sizeof skipped);
    if (!upd_should_offer(device, pick.tag, skipped)) {
        snprintf(err, sizeof err, "startup check: not offering %s (board=%s skipped=%s)",
                 pick.tag, device[0] ? device : "(unknown)", skipped[0] ? skipped : "(none)");
        cfg_log_startup(err);
        goto done;
    }
    offer = (char *)malloc(32);
    if (!offer) goto done;
    snprintf(offer, 32, "%s", pick.tag);
    snprintf(err, sizeof err, "startup check: offering %s", pick.tag);
    cfg_log_startup(err);
    if (!PostMessageW(owner, WM_LINK_UPDATE_AVAILABLE, 0, (LPARAM)offer)) {
        cfg_log_startup("startup check: could not post the offer to the UI thread");
        free(offer);
    }
    offer = NULL;
done:
    free(list);
    free(offer);
    return 0;
}

void link_updater_startup_check(HWND owner)
{
    HANDLE th;
    if (!owner) return;
    th = CreateThread(NULL, 0, startup_check_thread, owner, 0, NULL);
    if (th) CloseHandle(th);
}

void link_updater_handle_offer(HWND owner, char *version)
{
    char device[32], err[200], url[320];
    uint8_t *manifest_bytes = NULL, *image = NULL;
    size_t man_n = 0, image_n = 0;
    upd_manifest man;
    int choice;
    if (!version) return;
    device[0] = 0;
    cfg_read_str(L"LastDeviceVersion", device, sizeof device);
    choice = show_offer_dialog(owner, version, device);
    if (choice == 2) {
        cfg_write_str(L"SkippedFirmwareVersion", version);
        cfg_log_startup("startup offer: skipped this version");
        goto done;
    }
    if (choice != 1) {
        cfg_log_startup("startup offer: later");
        goto done;
    }
    cfg_log_startup("startup offer: install now");
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        cfg_log_startup("startup offer: updater already busy");
        goto done;
    }
    if (!upd_release_url(version, UPD_MANIFEST_NAME, url, sizeof url) ||
        !winhttp_get(url, &manifest_bytes, &man_n, 16384, err, sizeof err) ||
        !upd_manifest_parse((char *)manifest_bytes, man_n, &man, err, sizeof err) ||
        !upd_manifest_constants_ok(&man, "stable", err, sizeof err) ||
        !upd_image_name_ok(&man, err, sizeof err) ||
        strcmp(man.version, version) != 0) {
        if (!err[0]) snprintf(err, sizeof err, "the stable manifest does not match its release tag");
        cfg_log_startup(err);
        InterlockedExchange(&g_busy, 0);
        goto done;
    }
    if (!upd_release_url(version, man.image_name, url, sizeof url) ||
        !winhttp_get(url, &image, &image_n, UPD_FACTORY_SIZE, err, sizeof err) ||
        !prepare_image(&man, image, image_n, err, sizeof err)) {
        cfg_log_startup(err[0] ? err : "startup offer: image download failed");
        InterlockedExchange(&g_busy, 0);
        goto done;
    }
    execute(owner, &man, image, image_n, 0);
    InterlockedExchange(&g_busy, 0);
done:
    free(manifest_bytes);
    free(image);
    free(version);
}


/* ---------------------------------------------------------------- headless CLI (v1.2.3)
 *
 *   HD2MacropadLink-v1.2.3.exe --update <crowpanel-update.json> --port COM5 [--reinstall] --yes
 *                              [--log-stdout] [--full-backup] [--rom-only] [--dry-run] [--result-file <path>]
 *   HD2MacropadLink-v1.2.3.exe --probe --port COM5 [--log-stdout] [--rom-only] [--result-file <path>]
 *
 * No dialogs. Prints PROGRESS lines and one final RESULT/PROBE line. Exit codes:
 * 0 ok, 1 update or probe failed (see restored=), 2 usage, 3 port busy or Link running, 4 bad package.
 */

static FILE *g_result_file;

static void cli_result(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cli_out("%s", buf);
    if (g_result_file) {
        fprintf(g_result_file, "%s\n", buf);
        fflush(g_result_file);
    }
}

static void cli_open_stdout(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_out = h;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) g_out = h;
    }
}

static void q8(const char *in, char *out, size_t cap)
{
    /* Quote-safe copy for key="value" fields. */
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 2 < cap; i++) out[o++] = in[i] == '"' ? '\'' : (in[i] == '\n' || in[i] == '\r' ? ' ' : in[i]);
    out[o] = 0;
}

static void w8(const wchar_t *in, char *out, size_t cap)
{
    char tmp[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, in ? in : L"", -1, tmp, (int)sizeof tmp, NULL, NULL);
    q8(tmp, out, cap);
}

static int cli_port_ok(const wchar_t *port)
{
    if (!port || wcsncmp(port, L"COM", 3) != 0 || wcslen(port) < 4 || wcslen(port) > 6) return 0;
    for (const wchar_t *p = port + 3; *p; p++)
        if (*p < L'0' || *p > L'9') return 0;
    return 1;
}

static int cli_probe(const wchar_t *port, int log_stdout, int rom_only)
{
    static ser_t ser;
    rom_io io;
    rom_probe_result pr;
    volatile LONG cancel = 0;
    char logp[MAX_PATH * 3], errq[260];
    int rc;
    HANDLE h = open_flash_port(port);
    if (h == INVALID_HANDLE_VALUE) {
        cli_result("PROBE rc=3 error=\"port busy or missing; quit HD2 Macropad Link and anything else using it\"");
        return 3;
    }
    ser_setup(&ser, &io, h, port, &cancel, NULL);
    ser.cli = 1;
    ser.log_stdout = log_stdout;
    open_update_log(&ser);
    io.rom_only = rom_only || reg_dword(L"RomOnly") != 0;
    ser_log(&ser, "HD2 Macropad Link v1.2.7 probe log (read-only)");
    rc = rom_probe(&io, &pr);
    w8(ser.log_path, logp, sizeof logp);
    q8(pr.error, errq, sizeof errq);
    {
        char line[300];
        snprintf(line, sizeof line, "probe finished rc=%d stub=%d fallback=%d app_len=%u version=%s md5=%s comm_errors=0x%lX",
                 rc, pr.stub, pr.stub_fallback, (unsigned)pr.app_len, pr.version, pr.app_md5, (unsigned long)ser.comm_errors);
        ser_log(&ser, line);
    }
    ser_close(&ser);
    cli_result("PROBE rc=%d chip=%s stub=%d stub_fallback=%d baud=%u layout=%s app_len=%u version=%s app_md5=%s "
               "t_connect=%.2f t_md5=%.2f written=0 log=\"%s\" error=\"%s\"",
               rc == 0 ? 0 : 1, rc == 0 ? "ESP32-S3" : "?", pr.stub, pr.stub_fallback, pr.baud,
               pr.layout_cp7 ? "cp7" : "unknown", (unsigned)pr.app_len, pr.version[0] ? pr.version : "?",
               pr.app_md5[0] ? pr.app_md5 : "?", pr.ms_connect / 1000.0, pr.ms_md5 / 1000.0, logp,
               rc == 0 ? "" : errq);
    return rc == 0 ? 0 : 1;
}

static int cli_update(const wchar_t *manifest, const wchar_t *port, int allow_same, int do_write,
                      int log_stdout, int full_backup, int rom_only)
{
    static job_t job;
    uint8_t *manifest_bytes = NULL, *image = NULL;
    size_t man_n = 0, image_n = 0;
    char err[200], errq[460], logp[MAX_PATH * 3], bk[MAX_PATH * 3], srcq[80];
    static upd_manifest man;
    wchar_t image_path[MAX_PATH], full[MAX_PATH], *slash;
    const wchar_t *base;
    int code;
    /* Accept relative paths and forward slashes; work from the absolute path. */
    if (!GetFullPathNameW(manifest, MAX_PATH, full, NULL)) lstrcpynW(full, manifest, MAX_PATH);
    manifest = full;
    base = wcsrchr(manifest, L'\\');
    if (!base) base = wcsrchr(manifest, L'/');
    base = base ? base + 1 : manifest;
    err[0] = 0;
    if (_wcsicmp(base, L"crowpanel-update.json") != 0) {
        cli_result("RESULT rc=4 error=\"the manifest has to be named crowpanel-update.json\"");
        return 4;
    }
    if (!load_file(manifest, &manifest_bytes, &man_n, 16384, err, sizeof err) ||
        !upd_manifest_parse((char *)manifest_bytes, man_n, &man, err, sizeof err) ||
        !upd_manifest_constants_ok(&man, "test", err, sizeof err) ||
        !upd_image_name_ok(&man, err, sizeof err)) {
        q8(err, errq, sizeof errq);
        cli_result("RESULT rc=4 error=\"%s\"", errq);
        free(manifest_bytes);
        return 4;
    }
    lstrcpynW(image_path, manifest, MAX_PATH);
    slash = wcsrchr(image_path, L'\\');
    if (!slash) slash = wcsrchr(image_path, L'/');
    if (slash) slash[1] = 0;
    else image_path[0] = 0;
    {
        wchar_t name[80];
        widen(man.image_name, name, 80);
        if (wcslen(image_path) + wcslen(name) + 1 >= MAX_PATH) {
            cli_result("RESULT rc=4 error=\"image path too long\"");
            free(manifest_bytes);
            return 4;
        }
        wcscat(image_path, name);
    }
    if (!load_file(image_path, &image, &image_n, UPD_FACTORY_SIZE, err, sizeof err) ||
        !prepare_image(&man, image, image_n, err, sizeof err)) {
        q8(err, errq, sizeof errq);
        cli_result("RESULT rc=4 error=\"%s\"", errq);
        free(manifest_bytes);
        free(image);
        return 4;
    }
    cli_out("PACKAGE version=%s size=%u sha256=%s (verified)", man.version, (unsigned)image_n, man.sha256_hex);
    ZeroMemory(&job, sizeof job);
    job.image = image;
    job.image_len = (uint32_t)image_n;
    job.version = man.version;
    job.allow_same = allow_same;
    job.do_write = do_write;
    job.cli = 1;
    job.log_stdout = log_stdout;
    job.full_backup = full_backup;
    job.rom_only = rom_only;
    lstrcpynW(job.port, port, 16);
    {
        wchar_t dir[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            cli_result("RESULT rc=4 error=\"LOCALAPPDATA is not set; no backup folder\"");
            free(manifest_bytes);
            free(image);
            return 4;
        }
        _snwprintf(job.backup_dir, MAX_PATH, L"%s\\HD2MacropadLink", dir);
        CreateDirectoryW(job.backup_dir, NULL);
        _snwprintf(job.backup_dir, MAX_PATH, L"%s\\HD2MacropadLink\\backups", dir);
        CreateDirectoryW(job.backup_dir, NULL);
    }
    job_thread(&job);
    if (job.rr.device_version[0] || job.rr.confirmed_version[0]) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\HD2MacropadLink", 0, NULL, 0,
                            KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
            wchar_t ver[40];
            widen(job.rr.confirmed_version[0] ? job.rr.confirmed_version : job.rr.device_version, ver, 40);
            RegSetValueExW(k, L"LastDeviceVersion", 0, REG_SZ, (const BYTE *)ver,
                           (DWORD)((wcslen(ver) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }
    q8(job.rr.error, errq, sizeof errq);
    w8(job.log_path, logp, sizeof logp);
    w8(job.backup_path, bk, sizeof bk);
    q8(job.rr.backup_source, srcq, sizeof srcq);
    code = job.rc == 0 ? 0 : 1;
    if (job.rc != 0 && strstr(job.rr.error, "busy or missing")) code = 3;
    cli_result("RESULT rc=%d wrote=%d verified=%d verify_ran=%d restored=%d confirmed=%d board_version_before=%s "
               "board_version_after=%s used_stub=%d stub_fallback=%d backup_source=\"%s\" t_connect=%.2f t_backup=%.2f "
               "t_write=%.2f t_verify=%.2f t_confirm=%.2f t_total=%.2f backup=\"%s\" log=\"%s\" message=\"%s\"",
               code, job.rr.wrote, job.rr.verified, job.rr.verify_ran, job.rr.restored, job.rr.confirmed,
               job.rr.device_version[0] ? job.rr.device_version : "?",
               job.rr.confirmed_version[0] ? job.rr.confirmed_version : "?", job.rr.used_stub, job.rr.stub_fallback,
               srcq, job.rr.ms_connect / 1000.0, job.rr.ms_backup / 1000.0, job.rr.ms_write / 1000.0,
               job.rr.ms_verify / 1000.0, job.rr.ms_confirm / 1000.0, job.ms_total / 1000.0, bk, logp, errq);
    free(manifest_bytes);
    free(image);
    return code;
}

int link_updater_cli(const wchar_t *cmdline)
{
    int argc = 0, code;
    wchar_t **argv = CommandLineToArgvW(cmdline, &argc);
    const wchar_t *manifest = NULL, *port = NULL, *result_file = NULL;
    int probe = 0, update = 0, reinstall = 0, yes = 0, log_stdout = 0, full_backup = 0, rom_only = 0, dry = 0;
    int bad = 0;
    HANDLE mtx;
    if (!argv) return -1;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--update") && i + 1 < argc) { update = 1; manifest = argv[++i]; }
        else if (!wcscmp(a, L"--probe")) probe = 1;
        else if (!wcscmp(a, L"--port") && i + 1 < argc) port = argv[++i];
        else if (!wcscmp(a, L"--reinstall")) reinstall = 1;
        else if (!wcscmp(a, L"--yes")) yes = 1;
        else if (!wcscmp(a, L"--log-stdout")) log_stdout = 1;
        else if (!wcscmp(a, L"--full-backup")) full_backup = 1;
        else if (!wcscmp(a, L"--rom-only")) rom_only = 1;
        else if (!wcscmp(a, L"--dry-run")) dry = 1;
        else if (!wcscmp(a, L"--result-file") && i + 1 < argc) result_file = argv[++i];
        else bad = 1;
    }
    if (!probe && !update) {
        LocalFree(argv);
        return -1; /* normal tray start */
    }
    cli_open_stdout();
    if (result_file) g_result_file = _wfopen(result_file, L"w");
    cli_out("HD2 Macropad Link v1.2.7 headless %s", probe ? "probe (read-only)" : "update");
    if (bad || (probe && update) || !cli_port_ok(port) || (update && !yes && !dry)) {
        cli_result("%s rc=2 error=\"usage: --update <crowpanel-update.json> --port COMn [--reinstall] --yes "
                   "[--log-stdout] [--full-backup] [--rom-only] [--dry-run] [--result-file f] | --probe --port COMn "
                   "[--log-stdout] [--rom-only] [--result-file f]%s\"",
                   probe ? "PROBE" : "RESULT", (update && !yes && !dry) ? ". Writing needs --yes" : "");
        code = 2;
        goto out;
    }
    /* The tray instance holds the COM port. Refuse rather than fight it. */
    mtx = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\HD2MacropadLink");
    if (mtx) {
        CloseHandle(mtx);
        cli_result("%s rc=3 error=\"HD2 Macropad Link is running in the tray. Quit it first (right-click, Exit).\"",
                   probe ? "PROBE" : "RESULT");
        code = 3;
        goto out;
    }
    if (probe) code = cli_probe(port, log_stdout, rom_only);
    else code = cli_update(manifest, port, reinstall, dry ? 0 : 1, log_stdout, full_backup, rom_only);
out:
    if (g_result_file) {
        fclose(g_result_file);
        g_result_file = NULL;
    }
    LocalFree(argv);
    return code;
}

void link_cli_prepare(void)
{
    cli_open_stdout();
}

void link_cli_line(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cli_out("%s", buf);
}

void link_cli_result(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cli_result("%s", buf);
}

int link_cli_result_open(const wchar_t *path)
{
    if (g_result_file) {
        fclose(g_result_file);
        g_result_file = NULL;
    }
    if (!path || !path[0]) return 0;
    g_result_file = _wfopen(path, L"w");
    return g_result_file != NULL;
}

void link_cli_result_close(void)
{
    if (g_result_file) {
        fclose(g_result_file);
        g_result_file = NULL;
    }
}

int link_run_rom_body(const wchar_t *port, HWND status, volatile LONG *cancel,
                      int cli, int log_stdout, int rom_only, int full_chip,
                      const wchar_t *log_kind, int (*body)(void *io, void *ctx), void *ctx,
                      wchar_t *log_path, size_t log_cap, unsigned *ms_total)
{
    static ser_t ser;
    rom_io io;
    unsigned t0;
    int rc;
    HANDLE h;
    if (!port || !body) return -3;
    h = open_flash_port(port);
    if (h == INVALID_HANDLE_VALUE) return -3;
    t0 = (unsigned)GetTickCount64();
    ser_setup(&ser, &io, h, port, cancel, status);
    ser.cli = cli;
    ser.log_stdout = log_stdout;
    ser.full_chip = full_chip;
    if (log_kind && log_kind[0]) lstrcpynW(ser.log_kind, log_kind, 16);
    io.rom_only = rom_only || reg_dword(L"RomOnly") != 0;
    open_update_log(&ser);
    if (log_path && log_cap) lstrcpynW(log_path, ser.log_path, (int)log_cap);
    rc = body(&io, ctx);
    if (ms_total) *ms_total = (unsigned)GetTickCount64() - t0;
    ser_close(&ser);
    return rc;
}
