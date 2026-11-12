#ifndef HD2_UPDATER_UI_H
#define HD2_UPDATER_UI_H
#include <windows.h>

#define WM_LINK_UPDATE_AVAILABLE (WM_APP + 20)

void link_updater_check_stable(HWND owner);
void link_updater_local_package(HWND owner);
void link_updater_startup_check(HWND owner);
void link_updater_handle_offer(HWND owner, char *version);
int link_updater_startup_enabled(void);
/* v1.2.3 headless mode. Returns the process exit code, or -1 if the command
 * line has neither --update nor --probe (normal tray start). */
int link_updater_cli(const wchar_t *cmdline);
void link_updater_startup_set(int on);

/* v1.2.5 first-install. link_install_cli returns -1 when --install is absent. */
int link_install_cli(const wchar_t *cmdline);
int link_install_should_prompt(void);
void link_install_show(HWND owner, int first_run);

/* Shared serial session used by the first-install UI. body() receives a rom_io*.
 * Returns -3 if the port cannot be opened, otherwise body's return code. */
int link_run_rom_body(const wchar_t *port, HWND status, volatile LONG *cancel,
                      int cli, int log_stdout, int rom_only, int full_chip,
                      const wchar_t *log_kind, int (*body)(void *io, void *ctx), void *ctx,
                      wchar_t *log_path, size_t log_cap, unsigned *ms_total);
void link_cli_prepare(void);
void link_cli_line(const char *fmt, ...);
void link_cli_result(const char *fmt, ...);
int link_cli_result_open(const wchar_t *path);
void link_cli_result_close(void);
#endif
