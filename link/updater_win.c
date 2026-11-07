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

#define DLG_UPDATE 2001
#define DLG_DRY    2002
#define DLG_CANCEL 2003
#define DLG_SAME   2004
#define DLG_TEXT   2005
#define DLG_STATUS 2006

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
    volatile LONG *cancel;
    HWND status;
} ser_t;

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

static int ser_read(void *ctx, uint8_t *data, size_t n, int timeout_ms)
{
    ser_t *s = ctx;
    COMMTIMEOUTS to;
    DWORD got = 0;
    ZeroMemory(&to, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = timeout_ms < 0 ? 0 : (DWORD)timeout_ms;
    to.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(s->port, &to);
    if (!ReadFile(s->port, data, (DWORD)n, &got, NULL)) return -1;
    return (int)got;
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
}

static void ser_sleep(void *ctx, unsigned ms)
{
    (void)ctx;
    Sleep(ms);
}

static void ser_progress(void *ctx, const char *phase, unsigned done, unsigned total)
{
    ser_t *s = ctx;
    wchar_t msg[160];
    const wchar_t *label = L"Working";
    if (strcmp(phase, "backup") == 0) label = L"Backing up the current app";
    else if (strcmp(phase, "write") == 0) label = L"Writing the new app";
    else if (strcmp(phase, "readback") == 0) label = L"Reading the new app back";
    else if (strcmp(phase, "reset") == 0) label = L"Restarting into the bootloader";
    if (total) {
        _snwprintf(msg, 160, L"%s  %u / %u bytes. Do not unplug the macropad.", label, done, total);
    } else {
        _snwprintf(msg, 160, L"%s. Do not unplug the macropad.", label);
    }
    if (s->status) SetWindowTextW(s->status, msg);
}

static int ser_cancelled(void *ctx)
{
    ser_t *s = ctx;
    return s->cancel && *s->cancel;
}

static HANDLE open_flash_port(const wchar_t *name)
{
    wchar_t path[32];
    _snwprintf(path, 32, L"\\\\.\\%s", name);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DCB dcb;
    COMMTIMEOUTS to;
    if (h == INVALID_HANDLE_VALUE) return h;
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
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 50;
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
    ses = WinHttpOpen(L"HD2MacropadLink/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
    wchar_t backup_path[MAX_PATH];
    rom_result rr;
    int rc;
} job_t;

static int backup_cb(void *ctx, const uint8_t *data, size_t n)
{
    job_t *job = ctx;
    FILE *f;
    uint8_t dig[32];
    char hex[80];
    wchar_t side[MAX_PATH];
    FILE *sf;
    if (n == 0 || n > UPD_FACTORY_SIZE) return -1;
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

static DWORD WINAPI job_thread(LPVOID arg)
{
    job_t *job = arg;
    ser_t ser;
    rom_io io;
    HANDLE h = open_flash_port(job->port);
    if (h == INVALID_HANDLE_VALUE) {
        char port8[16];
        WideCharToMultiByte(CP_UTF8, 0, job->port, -1, port8, (int)sizeof port8, NULL, NULL);
        snprintf(job->rr.error, sizeof job->rr.error,
                 "%s is busy or missing. Close anything else using it. Nothing was written.", port8);
        job->rc = -1;
        return 0;
    }
    ZeroMemory(&ser, sizeof ser);
    ser.port = h;
    ser.cancel = &job->cancel;
    ser.status = job->status;
    ZeroMemory(&io, sizeof io);
    io.ctx = &ser;
    io.write = ser_write;
    io.read = ser_read;
    io.set_baud = ser_baud;
    io.set_dtr_rts = ser_lines;
    io.flush_rx = ser_flush;
    io.sleep_ms = ser_sleep;
    io.progress = ser_progress;
    io.cancelled = ser_cancelled;
    job->rc = rom_update_app(&io, job->image, job->image_len, job->version, job->allow_same, job->do_write,
                             job->do_write ? backup_cb : NULL, job, &job->rr);
    EscapeCommFunction(h, CLRRTS);
    EscapeCommFunction(h, CLRDTR);
    CloseHandle(h);
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

static void finish_dialog(const wchar_t *backup, const rom_result *rr, int rc)
{
    wchar_t msg[1200];
    wchar_t errw[600];
    widen(rr->error, errw, 600);
    if (rc == 0) {
        _snwprintf(msg, 1200,
                   L"%s\n\nBackup:\n%s\n\nThe link will look for the macropad again. Set Connectivity to USB if the icon does not return. Presets were not erased.",
                   errw, backup[0] ? backup : L"(no backup, board was not written)");
    } else {
        _snwprintf(msg, 1200,
                   L"%s\n\nBackup, if one was saved:\n%s\n\nRecovery, if the screen stays blank: hold BOOT, tap RESET, release BOOT, then write that backup at 0x10000 with esptool at 115200. Do not erase the chip. A power loss during the write is not automatically recoverable.",
                   errw, backup[0] ? backup : L"(none)");
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
    SYSTEMTIME st;
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
        GetLocalTime(&st);
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            link_resume_after_update();
            MessageBoxW(owner, L"LOCALAPPDATA is not set, so a backup path could not be chosen. Nothing was written.",
                        L"HD2 Macropad firmware update", MB_OK | MB_ICONWARNING);
            return;
        }
        _snwprintf(job.backup_path, MAX_PATH, L"%s\\HD2MacropadLink", dir);
        CreateDirectoryW(job.backup_path, NULL);
        _snwprintf(job.backup_path, MAX_PATH, L"%s\\HD2MacropadLink\\backups", dir);
        CreateDirectoryW(job.backup_path, NULL);
        _snwprintf(job.backup_path, MAX_PATH, L"%s\\HD2MacropadLink\\backups\\app-%hs-%04u%02u%02u-%02u%02u%02u.bin",
                   dir, m->version, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    run_job(owner, &job);
    link_resume_after_update();
    finish_dialog(job.backup_path, &job.rr, job.rc);
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
