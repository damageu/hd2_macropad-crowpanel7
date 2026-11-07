#ifndef HD2_LINK_SESSION_H
#define HD2_LINK_SESSION_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
/* Pause the tray worker and choose the CH340 port that answered, or the only one.
 * Does not assume a COM number. On failure the worker is already resumed. */
int link_pause_for_update(wchar_t *port, size_t port_chars, wchar_t *err, size_t err_chars);
void link_resume_after_update(void);
#endif
