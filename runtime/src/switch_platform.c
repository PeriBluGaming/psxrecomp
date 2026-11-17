/* switch_platform.c — process-level setup for Nintendo Switch (libnx).
 *
 * Diagnostics: stdout/stderr have no destination on the console. When the
 * NRO was started with `nxlink -s <file>.nro` from a PC, hbloader records the
 * PC's address in __nxlink_host; the BSD socket service is brought up and both
 * streams are redirected to that host, where nxlink prints them live. Started
 * from hbmenu (no nxlink host), nothing is opened and output stays discarded.
 *
 * Built only on Switch (see cmake/psx_platform_switch.cmake). */
#if defined(__SWITCH__)

#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>   /* struct in_addr for __nxlink_host */
#include <switch.h>

#include "switch_platform.h"

static int s_nxlink_fd = -1;

static void psx_switch_platform_exit(void)
{
    if (s_nxlink_fd >= 0) {
        close(s_nxlink_fd);
        s_nxlink_fd = -1;
    }
    socketExit();
}

void psx_switch_platform_init(void)
{
    if (__nxlink_host.s_addr == 0) return;   /* not launched through nxlink */
    if (R_FAILED(socketInitializeDefault())) return;
    s_nxlink_fd = nxlinkStdio();
    atexit(psx_switch_platform_exit);
}

#endif /* __SWITCH__ */
