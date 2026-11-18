/* switch_platform.c — process-level setup for Nintendo Switch (libnx).
 *
 * Diagnostics: stdout/stderr have no destination on the console. When the
 * NRO was started with `nxlink -s <file>.nro` from a PC, hbloader records the
 * PC's address in __nxlink_host; the BSD socket service is brought up and both
 * streams are redirected to that host, where nxlink prints them live. Started
 * from hbmenu (no nxlink host), nothing is opened and output stays discarded,
 * unless the user opted into the psxrecomp_stdio.txt capture file (below).
 * The image base is printed at start, and a fault prints PC/LR and the frame
 * chain as image offsets for addr2line.
 *
 * Built only on Switch (see cmake/psx_platform_switch.cmake). */
#if defined(__SWITCH__)

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>   /* struct in_addr for __nxlink_host */
#include <switch.h>

#include "switch_platform.h"

static int s_nxlink_fd = -1;

/* Image base of this NRO (libnx linker script). The crash screen and the
 * exception handler report absolute addresses; subtracting this gives
 * offsets that addr2line resolves against the .elf. */
extern char __start__[];

/* ---- crash report -------------------------------------------------------
 * libnx calls __libnx_exception_handler on its own stack when a thread of
 * this process faults (when the exception reaches this image; under hbloader
 * that depends on the loader forwarding it). Only async-signal-safe work:
 * snprintf into a stack buffer and write(2) to stderr, which is the nxlink
 * socket when launched through nxlink. Returning lets the system fatal. */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static void crash_line(const char* text)
{
    size_t n = 0;
    while (text[n]) n++;
    (void)write(STDERR_FILENO, text, n);
}

static uintptr_t image_off(uint64_t addr)
{
    return (uintptr_t)(addr - (uint64_t)(uintptr_t)__start__);
}

void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char line[160];
    snprintf(line, sizeof(line),
             "\npsxrecomp: CRASH desc=0x%x base=%p\n"
             "  pc  +0x%lx\n  lr  +0x%lx\n  far 0x%016lx  esr 0x%08x\n",
             ctx->error_desc, (void*)__start__,
             (unsigned long)image_off(ctx->pc.x), (unsigned long)image_off(ctx->lr.x),
             (unsigned long)ctx->far.x, ctx->esr);
    crash_line(line);
    /* Frame-pointer chain: [fp] = caller fp, [fp+8] = return address. */
    uint64_t fp = ctx->fp.x;
    for (int i = 0; i < 32 && fp && (fp & 15) == 0; i++) {
        const uint64_t* frame = (const uint64_t*)(uintptr_t)fp;
        uint64_t ret = frame[1];
        if (!ret) break;
        snprintf(line, sizeof(line), "  bt%-2d +0x%lx\n", i, (unsigned long)image_off(ret));
        crash_line(line);
        if (frame[0] <= fp) break;   /* the stack grows down; callers sit above */
        fp = frame[0];
    }
    crash_line("psxrecomp: resolve with aarch64-none-elf-addr2line -f -C -i -e <exe>.elf <offsets>\n");
}

static int s_sockets_up = 0;

static void psx_switch_platform_exit(void)
{
    if (s_nxlink_fd >= 0) {
        close(s_nxlink_fd);
        s_nxlink_fd = -1;
    }
    if (s_sockets_up) {
        socketExit();
        s_sockets_up = 0;
    }
}

/* Opt-in fallback when nxlink is unavailable: if the user placed a file
 * named psxrecomp_stdio.txt next to the NRO, append stdout/stderr to it.
 * Absent (the default), nothing is written anywhere. */
static int route_to_capture_file(const char* argv0)
{
    char path[512];
    const char* slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (!slash) return 0;
    size_t dir_len = (size_t)(slash - argv0);
    if (dir_len + sizeof("/psxrecomp_stdio.txt") > sizeof(path)) return 0;
    memcpy(path, argv0, dir_len);
    memcpy(path + dir_len, "/psxrecomp_stdio.txt", sizeof("/psxrecomp_stdio.txt"));
    if (access(path, F_OK) != 0) return 0;
    if (!freopen(path, "a", stdout)) return 0;
    if (!freopen(path, "a", stderr)) return 0;
    return 1;
}

void psx_switch_platform_init(const char* argv0)
{
    int nxlink_tried = 0, routed = 0;
    if (__nxlink_host.s_addr != 0) {           /* launched through nxlink */
        nxlink_tried = 1;
        if (R_SUCCEEDED(socketInitializeDefault())) {
            s_sockets_up = 1;
            s_nxlink_fd = nxlinkStdio();
            routed = s_nxlink_fd >= 0;
        }
        atexit(psx_switch_platform_exit);
    }
    if (!routed && !route_to_capture_file(argv0)) return;
    if (nxlink_tried && !routed)
        fprintf(stderr, "psxrecomp: nxlink host %08x set but connecting failed; "
                        "writing to psxrecomp_stdio.txt instead\n",
                (unsigned)__nxlink_host.s_addr);
    fprintf(stderr, "psxrecomp: switch image base %p (crash offset = address - base)\n",
            (void*)__start__);
    fflush(stderr);
}

#endif /* __SWITCH__ */
