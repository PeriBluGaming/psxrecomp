/* switch_platform.c — process-level setup for Nintendo Switch (libnx).
 *
 * Diagnostics: stdout/stderr have no destination on the console. When the
 * NRO was started with `nxlink -s <file>.nro` from a PC, hbloader records the
 * PC's address in __nxlink_host; the BSD socket service is brought up and both
 * streams are redirected to that host, where nxlink prints them live. Started
 * from hbmenu (no nxlink host), nothing is opened and output stays discarded,
 * An opted-in psxrecomp_stdio.txt next to the NRO takes precedence (below).
 * The image base is printed at start, and a fault prints PC/LR and the frame
 * chain as image offsets for addr2line.
 *
 * Built only on Switch (see cmake/psx_platform_switch.cmake). */
#if defined(__SWITCH__)

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>   /* struct in_addr for __nxlink_host */
#include <switch.h>

#include "switch_platform.h"

static int s_nxlink_fd = -1;

/* Load address of this NRO. The image is position independent and the
 * linker script's __start__ is an absolute (unrelocated) 0, so ask the
 * kernel for the start of the code mapping that contains this file's code;
 * the NRO's text segment is mapped first, at the image base. The crash
 * screen and the exception handler report absolute addresses; subtracting
 * this gives offsets that addr2line resolves against the .elf. */
static uint64_t image_base(void)
{
    static uint64_t s_base;
    if (!s_base) {
        MemoryInfo mi;
        u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&image_base)))
            s_base = mi.addr;
    }
    return s_base;
}

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
    return (uintptr_t)(addr - image_base());
}

void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char line[160];
    snprintf(line, sizeof(line),
             "\npsxrecomp: CRASH desc=0x%x base=%p\n"
             "  pc  +0x%lx\n  lr  +0x%lx\n  far 0x%016lx  esr 0x%08x\n",
             ctx->error_desc, (void*)(uintptr_t)image_base(),
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

/* ---- CPU clock (opt-in) ---------------------------------------------------
 * Homebrew inherits the 1020 MHz CPU clock of normal applications; games use
 * up to 1785 MHz (boost mode). PSX_SWITCH_CPU_MHZ=<MHz> (e.g. via
 * psxrecomp_env.txt) sets the CPU bus clock through clkrst (HOS 8.0+) or pcv
 * and restores the previous rate at exit. The system may reset it on
 * performance-mode changes (docking). */
static u32 s_cpu_prev_hz;

static Result cpu_clock_get_set(u32 set_hz, u32* out_prev_hz)
{
    Result rc;
    if (hosversionAtLeast(8, 0, 0)) {
        if (R_FAILED(rc = clkrstInitialize())) return rc;
        ClkrstSession session;
        if (R_SUCCEEDED(rc = clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) {
            if (out_prev_hz) clkrstGetClockRate(&session, out_prev_hz);
            rc = clkrstSetClockRate(&session, set_hz);
            clkrstCloseSession(&session);
        }
        clkrstExit();
    } else {
        if (R_FAILED(rc = pcvInitialize())) return rc;
        if (out_prev_hz) pcvGetClockRate(PcvModule_CpuBus, out_prev_hz);
        rc = pcvSetClockRate(PcvModule_CpuBus, set_hz);
        pcvExit();
    }
    return rc;
}

static void cpu_clock_restore(void)
{
    if (s_cpu_prev_hz) {
        cpu_clock_get_set(s_cpu_prev_hz, NULL);
        s_cpu_prev_hz = 0;
    }
}

static void cpu_clock_apply(void)
{
    const char* e = getenv("PSX_SWITCH_CPU_MHZ");
    if (!e || !e[0]) return;
    const long mhz = strtol(e, NULL, 10);
    if (mhz < 612 || mhz > 1785) {
        fprintf(stderr, "psxrecomp: PSX_SWITCH_CPU_MHZ=%s ignored (612..1785)\n", e);
        return;
    }
    u32 prev = 0;
    const Result rc = cpu_clock_get_set((u32)mhz * 1000000u, &prev);
    if (R_FAILED(rc)) {
        fprintf(stderr, "psxrecomp: setting CPU clock to %ld MHz failed (rc=0x%x)\n",
                mhz, (unsigned)rc);
        return;
    }
    s_cpu_prev_hz = prev;
    atexit(cpu_clock_restore);
    fprintf(stderr, "psxrecomp: CPU clock %u -> %ld MHz\n", (unsigned)(prev / 1000000u), mhz);
}

/* Builds "<dir of argv0>/<name>" into out; 0 if it does not fit. */
static int sibling_path(char* out, size_t cap, const char* argv0, const char* name)
{
    const char* slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (!slash) return 0;
    size_t dir_len = (size_t)(slash - argv0), name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > cap) return 0;
    memcpy(out, argv0, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len + 1);
    return 1;
}

/* Opt-in capture: if the user placed a file named psxrecomp_stdio.txt next
 * to the NRO, append stdout/stderr to it (takes precedence over nxlink).
 * Absent (the default), nothing is written anywhere. */
static int route_to_capture_file(const char* argv0)
{
    char path[512];
    if (!sibling_path(path, sizeof(path), argv0, "psxrecomp_stdio.txt")) return 0;
    if (access(path, F_OK) != 0) return 0;
    if (!freopen(path, "a", stdout)) return 0;
    if (!freopen(path, "a", stderr)) return 0;
    return 1;
}

/* Opt-in environment: Horizon has no shell, so the runtime's diagnostic
 * switches (PSX_RUNTIME_PERF_DIAG, PSX_FPS_TELEMETRY, ...) are read from
 * psxrecomp_env.txt next to the NRO when the user creates it: one NAME=VALUE
 * per line, '#' starts a comment line. Absent, nothing changes. Returns the
 * number of variables set (reported once output routing is up). */
static int load_env_file(const char* argv0)
{
    char path[512], line[512];
    if (!sibling_path(path, sizeof(path), argv0, "psxrecomp_env.txt")) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = '\0';
        char* key = line;
        while (*key == ' ' || *key == '\t') key++;
        if (!*key || *key == '#') continue;
        char* eq = strchr(key, '=');
        if (!eq || eq == key) continue;
        *eq = '\0';
        if (setenv(key, eq + 1, 1) == 0) count++;
    }
    fclose(f);
    return count;
}

void psx_switch_platform_init(const char* argv0)
{
    static int s_initialized = 0;
    if (s_initialized) return;           /* userAppInit already ran it */
    s_initialized = 1;
    int routed = 0;
    const int env_count = load_env_file(argv0);
    /* An opted-in capture file wins: it needs no network, so it also
     * reports problems in bringing up sockets/nxlink themselves. */
    if (route_to_capture_file(argv0)) {
        fprintf(stderr, "\n===== psxrecomp (switch) =====\n");
        routed = 1;
    } else if (__nxlink_host.s_addr != 0) {    /* launched through nxlink */
        if (R_SUCCEEDED(socketInitializeDefault())) {
            s_sockets_up = 1;
            s_nxlink_fd = nxlinkStdio();
            routed = s_nxlink_fd >= 0;
        }
        atexit(psx_switch_platform_exit);
    }
    cpu_clock_apply();     /* reports to stderr when routed */
    if (!routed) return;   /* nxlink absent or its connection failed */
    if (env_count)
        fprintf(stderr, "psxrecomp: %d variable(s) set from psxrecomp_env.txt\n", env_count);
    fprintf(stderr, "psxrecomp: switch image base %p (crash offset = address - base)\n",
            (void*)(uintptr_t)image_base());
    fflush(stderr);
}

/* libnx calls userAppInit after argv, services and sdmc are up but BEFORE
 * C++ static constructors, so output routing (and the crash report) covers
 * a fault during static initialization too. main() calls the init again;
 * the second call is a no-op. */
/* ---- default thread stack ------------------------------------------------
 * libnx gives a pthread/std::thread created without an explicit stack size
 * 128 KiB (newlib.c: __syscall_thread_create). The runtime's worker threads
 * (deferred overlay init, render, present, audio, ...) are sized for desktop
 * defaults (512 KiB macOS, 8 MiB Linux) and overflow that. The link wraps
 * __syscall_thread_create (-Wl,--wrap, cmake/psx_platform_switch.cmake) so a
 * request without a size gets PSX_SWITCH_THREAD_STACK; explicit sizes and
 * caller-provided stacks pass through unchanged. */
#define PSX_SWITCH_THREAD_STACK (4u * 1024u * 1024u)

struct __pthread_t;
int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*),
                                   void* arg, void* stack_addr, size_t stack_size);
int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*),
                                   void* arg, void* stack_addr, size_t stack_size);
int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*),
                                   void* arg, void* stack_addr, size_t stack_size)
{
    if (!stack_addr && stack_size == 0)
        stack_size = PSX_SWITCH_THREAD_STACK;
    return __real___syscall_thread_create(thread, func, arg, stack_addr, stack_size);
}

/* ---- rename() over an existing file ---------------------------------------
 * POSIX rename() replaces an existing target; Horizon's file system refuses
 * it. The runtime's atomic-replace pattern (write tmp, rename over the real
 * file: overlay captures, settings, savestates, mod receipts, and
 * std::filesystem::rename in libstdc++) therefore failed from the second
 * write on. The link wraps rename (-Wl,--wrap=rename) so that when the plain
 * rename fails and both paths are regular files, the old target is removed
 * and the rename retried. Horizon offers no atomic replace, so a crash
 * between the two steps leaves only the complete tmp file behind. */
int __real_rename(const char* from, const char* to);
int __wrap_rename(const char* from, const char* to);
int __wrap_rename(const char* from, const char* to)
{
    if (__real_rename(from, to) == 0) return 0;
    const int saved = errno;
    struct stat st_from, st_to;
    if (stat(from, &st_from) == 0 && S_ISREG(st_from.st_mode) &&
        stat(to, &st_to) == 0 && S_ISREG(st_to.st_mode) &&
        remove(to) == 0)
        return __real_rename(from, to);
    errno = saved;
    return -1;
}

typedef struct {
    int (*fn)(int, char**);
    int argc;
    char** argv;
    int result;
} main_thread_args;

static void main_thread_entry(void* arg)
{
    main_thread_args* a = (main_thread_args*)arg;
    a->result = a->fn(a->argc, a->argv);
}

/* ---- sampling profiler (opt-in) ------------------------------------------
 * PSX_SWITCH_PROFILE=1 (e.g. via psxrecomp_env.txt) starts a thread that
 * pauses the runtime main thread about every millisecond, reads its PC with
 * svcGetThreadContext3 and resumes it. Every PSX_SWITCH_PROFILE_MS (default
 * 10000) it prints the hottest PCs as image offsets with their share of
 * samples, for addr2line against the .elf. The main thread is never paused
 * while the report is printed, so stdio locks it may hold cannot deadlock. */
#define PROF_SLOTS 16384u      /* power of two */
#define PROF_TOP   30

static uint64_t s_prof_off[PROF_SLOTS];
/* (pc, lr) pairs for PSX_SWITCH_PROFILE=2: the caller of a leaf function
 * (memcmp, svc wrappers, ...) is in lr when the sample lands in it. */
#define PROF_PAIR_SLOTS 32768u   /* power of two */
static uint64_t s_pair_pc[PROF_PAIR_SLOTS], s_pair_lr[PROF_PAIR_SLOTS];
static uint32_t s_pair_cnt[PROF_PAIR_SLOTS];
static uint32_t s_prof_cnt[PROF_SLOTS];
static uint32_t s_prof_total, s_prof_lost;
static Handle   s_prof_target;
static volatile int s_prof_stop;
static Thread   s_prof_thread;
static int      s_prof_full;

static void prof_add(uint64_t off)
{
    uint32_t h = (uint32_t)((off >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (uint32_t probe = 0; probe < 64; probe++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_prof_cnt[h] && s_prof_off[h] == off) { s_prof_cnt[h]++; s_prof_total++; return; }
        if (!s_prof_cnt[h]) { s_prof_off[h] = off; s_prof_cnt[h] = 1; s_prof_total++; return; }
    }
    s_prof_lost++;
}

static void prof_add_pair(uint64_t pc, uint64_t lr)
{
    uint32_t h = (uint32_t)(((pc >> 2) * 2654435761u) ^ ((lr >> 2) * 40503u)) & (PROF_PAIR_SLOTS - 1);
    for (uint32_t probe = 0; probe < 64; probe++, h = (h + 1) & (PROF_PAIR_SLOTS - 1)) {
        if (s_pair_cnt[h] && s_pair_pc[h] == pc && s_pair_lr[h] == lr) { s_pair_cnt[h]++; return; }
        if (!s_pair_cnt[h]) { s_pair_pc[h] = pc; s_pair_lr[h] = lr; s_pair_cnt[h] = 1; return; }
    }
}

static void prof_report(void)
{
    uint32_t top_idx[PROF_TOP];
    int n = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++) {
        const uint32_t c = s_prof_cnt[i];
        if (!c) continue;
        int pos;
        if (n < PROF_TOP) pos = n++;
        else if (c <= s_prof_cnt[top_idx[PROF_TOP - 1]]) continue;
        else pos = PROF_TOP - 1;
        top_idx[pos] = i;
        while (pos > 0 && s_prof_cnt[top_idx[pos - 1]] < c) {
            const uint32_t t = top_idx[pos - 1];
            top_idx[pos - 1] = top_idx[pos];
            top_idx[pos] = t;
            pos--;
        }
    }
    fprintf(stderr, "psxrecomp: [profile] %u samples (%u unbinned); hottest PCs (image offsets):\n",
            (unsigned)s_prof_total, (unsigned)s_prof_lost);
    for (int i = 0; i < n; i++) {
        uint32_t k = top_idx[i];
        fprintf(stderr, "  %5.1f%%  +0x%lx\n",
                s_prof_total ? 100.0 * s_prof_cnt[k] / s_prof_total : 0.0,
                (unsigned long)s_prof_off[k]);
    }
    fprintf(stderr, "psxrecomp: [profile] addr2line -f -C -e <exe>.elf <offsets>\n");
    if (s_prof_full) {
        /* PSX_SWITCH_PROFILE=2: every bin of this interval, for per-function
         * aggregation with tools/switch_profile_report.py. */
        fprintf(stderr, "psxrecomp: [profile-dump-begin] %u\n", (unsigned)s_prof_total);
        for (uint32_t i = 0; i < PROF_SLOTS; i++)
            if (s_prof_cnt[i])
                fprintf(stderr, "P %lx %u\n", (unsigned long)s_prof_off[i], (unsigned)s_prof_cnt[i]);
        for (uint32_t i = 0; i < PROF_PAIR_SLOTS; i++)
            if (s_pair_cnt[i])
                fprintf(stderr, "C %lx %lx %u\n", (unsigned long)s_pair_pc[i],
                        (unsigned long)s_pair_lr[i], (unsigned)s_pair_cnt[i]);
        memset(s_pair_cnt, 0, sizeof(s_pair_cnt));
        fprintf(stderr, "psxrecomp: [profile-dump-end]\n");
    }
    fflush(stderr);
    memset(s_prof_cnt, 0, sizeof(s_prof_cnt));
    s_prof_total = s_prof_lost = 0;
}

static void prof_thread_entry(void* arg)
{
    (void)arg;
    const uint64_t base = image_base();
    uint64_t interval_ms = 10000;
    const char* e = getenv("PSX_SWITCH_PROFILE_MS");
    if (e && atoi(e) >= 1000) interval_ms = (uint64_t)atoi(e);
    uint64_t next = armGetSystemTick() + armNsToTicks(interval_ms * 1000000ull);
    while (!s_prof_stop) {
        svcSleepThread(1000000);   /* ~1 kHz */
        if (R_SUCCEEDED(svcSetThreadActivity(s_prof_target, ThreadActivity_Paused))) {
            ThreadContext ctx;
            const Result rc = svcGetThreadContext3(&ctx, s_prof_target);
            svcSetThreadActivity(s_prof_target, ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) {
                prof_add(ctx.pc.x - base);
                if (s_prof_full) prof_add_pair(ctx.pc.x - base, ctx.lr - base);
            }
        }
        if (armGetSystemTick() >= next) {
            prof_report();
            next = armGetSystemTick() + armNsToTicks(interval_ms * 1000000ull);
        }
    }
}

static void prof_start(Handle target)
{
    const char* e = getenv("PSX_SWITCH_PROFILE");
    if (!e || !e[0] || e[0] == '0') return;
    s_prof_full = e[0] == '2';
    s_prof_target = target;
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* Higher priority than the sampled thread so samples are taken on time. */
    if (R_FAILED(threadCreate(&s_prof_thread, prof_thread_entry, NULL, NULL,
                              64 * 1024, prio > 0x18 ? prio - 1 : prio, -2)) ||
        R_FAILED(threadStart(&s_prof_thread))) {
        fprintf(stderr, "psxrecomp: [profile] could not start the sampler thread\n");
        return;
    }
    fprintf(stderr, "psxrecomp: [profile] sampling the runtime main thread at ~1 kHz\n");
}

static void prof_stop(void)
{
    if (!s_prof_target) return;
    s_prof_stop = 1;
    threadWaitForExit(&s_prof_thread);
    threadClose(&s_prof_thread);
    s_prof_target = 0;
}

int psx_switch_run_main(int (*fn)(int, char**), int argc, char** argv)
{
    main_thread_args args = { fn, argc, argv, 0 };
    s32 prio = 0x2C;
    Thread thread;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Result rc = threadCreate(&thread, main_thread_entry, &args, NULL,
                             PSX_SWITCH_MAIN_STACK, prio, -2);
    if (R_FAILED(rc)) {
        fprintf(stderr, "psxrecomp: cannot create the %u-byte main stack thread "
                        "(rc=0x%x); running on the loader's stack\n",
                PSX_SWITCH_MAIN_STACK, (unsigned)rc);
        return fn(argc, argv);
    }
    fprintf(stderr, "psxrecomp: runtime main on a %u MB stack thread\n",
            PSX_SWITCH_MAIN_STACK >> 20);
    rc = threadStart(&thread);
    if (R_FAILED(rc)) {
        threadClose(&thread);
        fprintf(stderr, "psxrecomp: cannot start the main stack thread (rc=0x%x); "
                        "running on the loader's stack\n", (unsigned)rc);
        return fn(argc, argv);
    }
    prof_start(thread.handle);
    threadWaitForExit(&thread);
    prof_stop();
    threadClose(&thread);
    return args.result;
}

extern int __system_argc;
extern char** __system_argv;

void userAppInit(void)
{
    psx_switch_platform_init(__system_argc > 0 ? __system_argv[0] : NULL);
    fprintf(stderr, "psxrecomp: userAppInit done; running static constructors\n");
    fflush(stderr);
}

#endif /* __SWITCH__ */
