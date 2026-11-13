/*
 * HD2 Macropad Link (Windows)
 *
 * Finds the CrowPanel macropad on its USB-C serial port (CH340), answers its keyboard
 * reports with real key presses (SendInput, scancodes, same path HD2 accepts from AHK),
 * and sits in the tray. Starts with Windows (toggle in the tray menu).
 *
 * CrowPanel serial companion for Erik Lembke's HD2 Macropad. The firmware port
 * is Evan's. v1.1 holds each key at least 30 ms and leaves 8 ms between events.
 * The protocol rate is 115200 baud. That is not the esptool flash baud.
 *
 * Build: see BUILD.txt (MinGW-w64, -municode, -lsetupapi -lwinmm).
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <stdio.h>
#include <stdarg.h>
#include <mmsystem.h>
#include <string.h>
#include "link_core.h"
#include "updater_ui.h"

DEFINE_GUID(GUID_DEVINTERFACE_COMPORT_LOCAL, 0x86E0D1E0, 0x8089, 0x11D0, 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73);

#define APP_NAME      L"HD2 Macropad Link"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE     L"HD2MacropadLink"
#define CFG_KEY       L"Software\\HD2MacropadLink"
#define WM_TRAY       (WM_APP + 1)
#define WM_STATUS     (WM_APP + 2)
#define ID_AUTOSTART  1001
#define ID_EXIT       1002
#define ID_UPDATE     1003
#define ID_TESTPKG    1004
#define ID_STARTUP_UPD 1005
#define ID_INSTALL    1006

static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static volatile LONG g_quit = 0;
static volatile LONG g_hold = 0;
static volatile LONG g_in_port = 0;
static wchar_t g_last_port[16];
static wchar_t g_status[128] = L"Looking for the macropad...";
static CRITICAL_SECTION g_cs;

/* ---------------------------------------------------------------- key injection */

/* v1.1: pacing. If the reports reach us in a burst (USB hiccup, thread descheduled, PC
 * waking from an idle state), a key's down and up would be injected in the same instant and
 * the game, which samples input once per frame, never sees the press. Keep every key held at
 * least MIN_HOLD_MS and keep MIN_GAP_MS between events. Normal 40 ms reports are not delayed. */
#define MIN_HOLD_MS 30
#define MIN_GAP_MS  8
static LARGE_INTEGER g_qpf, g_last_ev;
static LARGE_INTEGER g_down_at[0x200];
static FILE *g_log;

static double ms_since(LARGE_INTEGER t)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void wait_until(LARGE_INTEGER t, double after_ms)
{
    double left;
    while (t.QuadPart && (left = after_ms - ms_since(t)) > 0) {
        if (left > 2.0) Sleep((DWORD)(left - 1.0)); else SwitchToThread();
    }
}

static void log_line(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void send_scancode(uint16_t sc, int down, void *ctx)
{
    (void)ctx;
    sc &= 0x1FF;
    wait_until(g_last_ev, MIN_GAP_MS);
    if (!down) wait_until(g_down_at[sc], MIN_HOLD_MS);
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)(sc & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) | ((sc & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0);
    UINT ok = SendInput(1, &in, sizeof(INPUT));
    QueryPerformanceCounter(&g_last_ev);
    if (down) g_down_at[sc] = g_last_ev; else g_down_at[sc].QuadPart = 0;
    log_line("inject sc=%03X %s%s", sc, down ? "down" : "up", ok ? "" : " (SendInput FAILED)");
}

/* ---------------------------------------------------------------- status / tray */

static void set_status(const wchar_t *s)
{
    EnterCriticalSection(&g_cs);
    if (wcscmp(g_status, s) != 0) {
        lstrcpynW(g_status, s, 128);
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hwnd, WM_STATUS, 0, 0);
        return;
    }
    LeaveCriticalSection(&g_cs);
}

static void tray_update(void)
{
    EnterCriticalSection(&g_cs);
    _snwprintf(g_nid.szTip, 127, L"HD2 Macropad Link: %s", g_status);
    g_nid.szTip[127] = 0;
    LeaveCriticalSection(&g_cs);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ---------------------------------------------------------------- autostart */

static int autostart_enabled(void)
{
    HKEY k;
    int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void autostart_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH + 2], quoted[MAX_PATH + 8];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        _snwprintf(quoted, MAX_PATH + 8, L"\"%s\"", path);
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

/* First run: turn autostart on once, then respect whatever the user picks */
static void autostart_first_run(void)
{
    HKEY k;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CFG_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS) return;
    DWORD done = 0, sz = sizeof(done);
    if (RegQueryValueExW(k, L"Configured", NULL, NULL, (BYTE *)&done, &sz) != ERROR_SUCCESS || !done) {
        autostart_set(1);
        done = 1;
        RegSetValueExW(k, L"Configured", 0, REG_DWORD, (const BYTE *)&done, sizeof(done));
    } else if (autostart_enabled()) {
        autostart_set(1);   /* refresh path in case the exe was moved */
    }
    RegCloseKey(k);
}

/* ---------------------------------------------------------------- port discovery */

/* Collect COM port names whose hardware ID is a WCH CH34x (VID 1A86). Falls back to all ports. */
static int find_ports(wchar_t ports[][16], int max, int ch34x_only)
{
    int n = 0;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT_LOCAL, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd) && n < max; i++) {
        wchar_t hwid[512] = {0};
        SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_HARDWAREID, NULL, (PBYTE)hwid, sizeof(hwid) - 2, NULL);
        _wcsupr(hwid);
        if (ch34x_only && !wcsstr(hwid, L"VID_1A86")) continue;
        HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        wchar_t name[16] = {0};
        DWORD sz = sizeof(name) - 2, type = 0;
        if (RegQueryValueExW(k, L"PortName", NULL, &type, (BYTE *)name, &sz) == ERROR_SUCCESS && !wcsncmp(name, L"COM", 3)) {
            lstrcpynW(ports[n++], name, 16);
        }
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return n;
}

static HANDLE open_port(const wchar_t *name)
{
    wchar_t path[32];
    _snwprintf(path, 32, L"\\\\.\\%s", name);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    /* Release RTS before DTR: RTS asserted with DTR released is the auto-reset combination */
    EscapeCommFunction(h, CLRRTS);
    EscapeCommFunction(h, CLRDTR);

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    /* DTR/RTS drive the board's auto-reset circuit: keep both released so it doesn't reboot */
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    SetCommState(h, &dcb);

    COMMTIMEOUTS to;
    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 20;        /* read returns after <=20 ms, or as soon as a byte arrives */
    to.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

static int send_ping(HANDLE h)
{
    uint8_t f[8];
    int n = link_build_frame(f, LINK_T_PING, NULL, 0);
    DWORD w = 0;
    return WriteFile(h, f, n, &w, NULL) && (int)w == n;
}

/* ---------------------------------------------------------------- worker */

/* Talk to one open port until it disappears or the board stops answering for good.
 * Returns 1 if the macropad answered at least once. */
static int run_port(HANDLE h, const wchar_t *name, DWORD probe_ms)
{
    link_parser_t p;
    link_state_t st;
    ZeroMemory(&p, sizeof(p));
    ZeroMemory(&st, sizeof(st));
    DWORD start = GetTickCount(), last_ping = 0, last_pong = 0;
    int seen = 0;
    wchar_t msg[128];

    while (!g_quit && !g_hold) {
        DWORD now = GetTickCount();
        if (now - last_ping >= 500 || last_ping == 0) {   /* v1.1: was 1000 */
            if (!send_ping(h)) break;                 /* unplugged */
            last_ping = now;
        }
        uint8_t buf[256];
        DWORD r = 0;
        if (!ReadFile(h, buf, sizeof(buf), &r, NULL)) break;
        for (DWORD i = 0; i < r; i++) {
            if (!link_parse_byte(&p, buf[i])) continue;
            if (p.type == LINK_T_PONG && p.len >= 3 && p.payload[0] == 'H' && p.payload[1] == 'D' && p.payload[2] == '2') {
                if (seen && GetTickCount() - last_pong > 1500) log_line("PONG gap %lu ms", (unsigned long)(GetTickCount() - last_pong));
                last_pong = GetTickCount();
                if (!seen) {
                    seen = 1;
                    lstrcpynW(g_last_port, name, 16);
                    _snwprintf(msg, 128, L"connected on %s", name);
                    set_status(msg);
                }
            } else if (p.type == LINK_T_KEY && p.len == 7) {
                log_line("KEY frame mod=%02X k0=%02X (foreground=%p)", p.payload[0], p.payload[1], (void *)GetForegroundWindow());
                link_apply_report(&st, p.payload, send_scancode, NULL);
            }
        }
        now = GetTickCount();
        if (!seen && now - start > probe_ms) break;  /* not the macropad, or not in USB mode */
        if (seen && now - last_pong > 3000) {
            link_release_all(&st, send_scancode, NULL);
            _snwprintf(msg, 128, L"found on %s, set Connectivity to USB on the macropad", name);
            set_status(msg);
            seen = 0;
            start = now;
            probe_ms = 0x7FFFFFFF;                    /* stay on this port, keep pinging */
        }
    }
    link_release_all(&st, send_scancode, NULL);
    return seen;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    /* v1.1: tray apps get Windows power throttling (EcoQoS) and coarse timers; opt out */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    timeBeginPeriod(1);
    wchar_t ports[16][16];
    while (!g_quit) {
        if (g_hold) {
            set_status(L"paused for a firmware update");
            while (g_hold && !g_quit) Sleep(50);
            continue;
        }
        int n = find_ports(ports, 16, 1);
        if (n == 0) {
            set_status(L"plug the macropad's USB-C into this PC");
            Sleep(2000);
            continue;
        }
        int any = 0;
        for (int i = 0; i < n && !g_quit; i++) {
            HANDLE h = open_port(ports[i]);
            if (h == INVALID_HANDLE_VALUE) {
                wchar_t msg[128];
                _snwprintf(msg, 128, L"%s is in use by another program (close the flash tool / serial monitor)", ports[i]);
                set_status(msg);
                continue;
            }
            any = 1;
            wchar_t msg[128];
            _snwprintf(msg, 128, L"checking %s...", ports[i]);
            set_status(msg);
            InterlockedExchange(&g_in_port, 1);
            /* Only one CH34x port: it's almost certainly the macropad, so stay on it */
            run_port(h, ports[i], n == 1 ? 0x7FFFFFFF : 3000);
            CloseHandle(h);
            InterlockedExchange(&g_in_port, 0);
            if (g_hold) {
                set_status(L"paused for a firmware update");
                while (g_hold && !g_quit) Sleep(50);
            }
        }
        if (!any) Sleep(2000);
        else {
            set_status(L"looking for the macropad...");
            Sleep(1000);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- window */

int link_pause_for_update(wchar_t *port, size_t port_chars, wchar_t *err, size_t err_chars)
{
    wchar_t ports[8][16];
    int n, i;
    const wchar_t *chosen = NULL;
    InterlockedExchange(&g_hold, 1);
    Sleep(150);
    for (i = 0; i < 100 && g_in_port; i++) Sleep(50);
    if (g_in_port) {
        InterlockedExchange(&g_hold, 0);
        _snwprintf(err, err_chars, L"The serial connection did not pause. Nothing was written.");
        return 0;
    }
    n = find_ports(ports, 8, 1);
    if (n <= 0) {
        InterlockedExchange(&g_hold, 0);
        _snwprintf(err, err_chars, L"No CH340 serial device is connected. Nothing was written.");
        return 0;
    }
    if (n == 1) {
        chosen = ports[0];
    } else {
        for (i = 0; i < n; i++) {
            if (g_last_port[0] && wcscmp(ports[i], g_last_port) == 0) chosen = ports[i];
        }
        if (!chosen) {
            InterlockedExchange(&g_hold, 0);
            _snwprintf(err, err_chars, L"More than one CH340 is connected, and none is the macropad that was answering. Unplug the others. Nothing was written.");
            return 0;
        }
    }
    if (port_chars < 2) {
        InterlockedExchange(&g_hold, 0);
        return 0;
    }
    lstrcpynW(port, chosen, (int)port_chars);
    return 1;
}

void link_resume_after_update(void)
{
    InterlockedExchange(&g_hold, 0);
}

static void show_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    wchar_t st[160];
    EnterCriticalSection(&g_cs);
    _snwprintf(st, 160, L"Status: %s", g_status);
    LeaveCriticalSection(&g_cs);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, st);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (link_updater_startup_enabled() ? MF_CHECKED : 0), ID_STARTUP_UPD, L"Check for updates at startup");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_INSTALL, L"Install firmware on a new board...");
    AppendMenuW(m, MF_STRING, ID_UPDATE, L"Check for firmware update...");
    AppendMenuW(m, MF_STRING, ID_TESTPKG, L"Install local test package...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu(hwnd);
        return 0;
    case WM_STATUS:
        tray_update();
        return 0;
    case WM_LINK_UPDATE_AVAILABLE:
        link_updater_handle_offer(hwnd, (char *)lp);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_AUTOSTART) autostart_set(!autostart_enabled());
        if (LOWORD(wp) == ID_STARTUP_UPD) link_updater_startup_set(!link_updater_startup_enabled());
        if (LOWORD(wp) == ID_UPDATE) link_updater_check_stable(hwnd);
        if (LOWORD(wp) == ID_INSTALL) link_install_show(hwnd, 0);
        if (LOWORD(wp) == ID_TESTPKG) link_updater_local_package(hwnd);
        if (LOWORD(wp) == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_quit, 1);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
    (void)hp; (void)cmd; (void)show;
    int cli;
    cli = link_install_cli(GetCommandLineW());
    if (cli >= 0) return cli; /* headless --install: no tray */
    cli = link_updater_cli(GetCommandLineW());
    if (cli >= 0) return cli; /* headless --update / --probe: no tray, no dialogs */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\HD2MacropadLink");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   /* already running */

    InitializeCriticalSection(&g_cs);
    autostart_first_run();
    QueryPerformanceFrequency(&g_qpf);

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define ProcessPowerThrottling 4
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
    {
        typedef BOOL (WINAPI *SPI_t)(HANDLE, int, LPVOID, DWORD);
        SPI_t spi = (SPI_t)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
        PROCESS_POWER_THROTTLING_STATE ps = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
        if (spi) spi(GetCurrentProcess(), ProcessPowerThrottling, &ps, sizeof(ps));
    }

    /* v1.1: optional diagnostic log. Enable with
     *   reg add HKCU\Software\HD2MacropadLink /v Log /t REG_DWORD /d 1
     * -> %LOCALAPPDATA%\HD2MacropadLink\link.log */
    {
        HKEY k;
        DWORD on = 0, sz = sizeof(on);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, CFG_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryValueExW(k, L"Log", NULL, NULL, (BYTE *)&on, &sz);
            RegCloseKey(k);
        }
        if (on) {
            wchar_t dir[MAX_PATH], path[MAX_PATH];
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
                _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink", dir);
                CreateDirectoryW(path, NULL);
                _snwprintf(path, MAX_PATH, L"%s\\HD2MacropadLink\\link.log", dir);
                g_log = _wfopen(path, L"a");
                log_line("HD2 Macropad Link v1.2.7 started");
            }
        }
    }

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"HD2MacropadLinkWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hi, NULL);

    if (link_install_should_prompt())
        link_install_show(g_hwnd, 1);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"HD2 Macropad Link: starting", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    /* Once per launch, and only if the tray toggle is on (default on). */
    link_updater_startup_check(g_hwnd);

    HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    InterlockedExchange(&g_quit, 1);
    WaitForSingleObject(th, 2000);
    ReleaseMutex(mtx);
    return 0;
}
