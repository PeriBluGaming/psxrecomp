/*
 * psx_fiber.c — Win32-Fiber / POSIX-ucontext backends for psx_fiber.h.
 */
#include "psx_fiber.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

psx_fiber_t psx_fiber_convert_thread(void)
{
    /* Attempt the conversion unconditionally and check the error code on
     * failure. The 0x1E00000000000000 TEB sentinel for "not yet a fiber"
     * is MSVC-internal and not guaranteed by MinGW's GetCurrentFiber()
     * implementation — reading it before ConvertThreadToFiber is called
     * can return an unspecified TEB value that isn't the sentinel, causing
     * the sentinel check to silently skip ConvertThreadToFiber, leaving
     * the thread as a non-fiber, and crashing the first SwitchToFiber. */
    void* fib = ConvertThreadToFiber(NULL);
    if (!fib && GetLastError() == ERROR_ALREADY_FIBER)
        fib = GetCurrentFiber();
    return (psx_fiber_t)fib;
}

psx_fiber_t psx_fiber_current(void)      { return (psx_fiber_t)GetCurrentFiber(); }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    return (psx_fiber_t)CreateFiber((SIZE_T)stack_size,
                                    (LPFIBER_START_ROUTINE)entry, arg);
}

void psx_fiber_switch(psx_fiber_t target) { SwitchToFiber((LPVOID)target); }
void psx_fiber_destroy(psx_fiber_t fiber) { if (fiber) DeleteFiber((LPVOID)fiber); }

#elif defined(__SWITCH__) || defined(PSX_FIBER_AARCH64_ASM)
/* Nintendo Switch (devkitA64 / libnx): newlib has no ucontext, so switch
 * stacks with a tiny AArch64 routine. Only AAPCS64 callee-saved state needs
 * preserving across a cooperative switch: x19-x28, fp (x29), lr (x30), sp
 * and the low 64 bits of v8-v15 (d8-d15). */

#if !defined(__aarch64__)
#  error "psx_fiber: the AArch64 asm backend requires an AArch64 target"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define PSX_FIBER_FRAME     176u      /* 22 x 8 bytes saved, rounded to 16 */
#define PSX_FIBER_MIN_STACK (64u * 1024u)

typedef struct psx_fiber_impl {
    void*           sp;       /* saved stack pointer while suspended */
    void*           stack;    /* NULL for the thread-fiber */
    psx_fiber_entry entry;
    void*           arg;
} psx_fiber_impl;

static psx_fiber_impl* s_current = NULL;

/* void psx_fiber_aarch64_swap(void** save_sp, void* load_sp) */
void psx_fiber_aarch64_swap(void** save_sp, void* load_sp);
/* First-run stub: x19 holds the fiber pointer (see psx_fiber_create). */
void psx_fiber_aarch64_boot(void);

__asm__(
    ".text\n"
    ".balign 16\n"
    ".global psx_fiber_aarch64_swap\n"
    ".type psx_fiber_aarch64_swap, %function\n"
    "psx_fiber_aarch64_swap:\n"
    "    sub  sp, sp, #176\n"
    "    stp  x19, x20, [sp, #0]\n"
    "    stp  x21, x22, [sp, #16]\n"
    "    stp  x23, x24, [sp, #32]\n"
    "    stp  x25, x26, [sp, #48]\n"
    "    stp  x27, x28, [sp, #64]\n"
    "    stp  x29, x30, [sp, #80]\n"
    "    stp  d8,  d9,  [sp, #96]\n"
    "    stp  d10, d11, [sp, #112]\n"
    "    stp  d12, d13, [sp, #128]\n"
    "    stp  d14, d15, [sp, #144]\n"
    "    mov  x2, sp\n"
    "    str  x2, [x0]\n"
    "    mov  sp, x1\n"
    "    ldp  x19, x20, [sp, #0]\n"
    "    ldp  x21, x22, [sp, #16]\n"
    "    ldp  x23, x24, [sp, #32]\n"
    "    ldp  x25, x26, [sp, #48]\n"
    "    ldp  x27, x28, [sp, #64]\n"
    "    ldp  x29, x30, [sp, #80]\n"
    "    ldp  d8,  d9,  [sp, #96]\n"
    "    ldp  d10, d11, [sp, #112]\n"
    "    ldp  d12, d13, [sp, #128]\n"
    "    ldp  d14, d15, [sp, #144]\n"
    "    add  sp, sp, #176\n"
    "    ret\n"
    ".size psx_fiber_aarch64_swap, .-psx_fiber_aarch64_swap\n"
    "\n"
    ".balign 16\n"
    ".global psx_fiber_aarch64_boot\n"
    ".type psx_fiber_aarch64_boot, %function\n"
    "psx_fiber_aarch64_boot:\n"
    "    mov  x0, x19\n"
    "    bl   psx_fiber_aarch64_entry\n"
    "    brk  #0\n"
    ".size psx_fiber_aarch64_boot, .-psx_fiber_aarch64_boot\n"
);

/* Called from the boot stub on a fresh fiber stack. Not static: the asm
 * above references it by symbol name. */
void psx_fiber_aarch64_entry(psx_fiber_impl* f);
void psx_fiber_aarch64_entry(psx_fiber_impl* f)
{
    f->entry(f->arg);
    /* Same contract as the ucontext backend: entries never return. */
    abort();
}

psx_fiber_t psx_fiber_convert_thread(void)
{
    if (!s_current) {
        psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
        s_current = f;            /* runs on the real thread stack */
    }
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_current(void) { return (psx_fiber_t)s_current; }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    if (stack_size < PSX_FIBER_MIN_STACK) stack_size = PSX_FIBER_MIN_STACK;
    stack_size = (stack_size + 15u) & ~(size_t)15u;

    psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->stack = aligned_alloc(16, stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->entry = entry;
    f->arg   = arg;

    /* Build an initial frame that psx_fiber_aarch64_swap will "restore":
     * x19 = fiber, x29 = 0 (terminates frame-pointer unwinding),
     * x30 = boot stub (the swap's `ret` jumps there). */
    uintptr_t top = ((uintptr_t)f->stack + stack_size) & ~(uintptr_t)15u;
    uint64_t* frame = (uint64_t*)(top - PSX_FIBER_FRAME);
    memset(frame, 0, PSX_FIBER_FRAME);
    frame[0]  = (uint64_t)(uintptr_t)f;                         /* x19 */
    frame[10] = 0;                                              /* x29 */
    frame[11] = (uint64_t)(uintptr_t)&psx_fiber_aarch64_boot;   /* x30 */
    f->sp = frame;
    return (psx_fiber_t)f;
}

void psx_fiber_switch(psx_fiber_t target)
{
    psx_fiber_impl* to   = (psx_fiber_impl*)target;
    psx_fiber_impl* from = s_current;
    if (!to || to == from) return;
    s_current = to;
    psx_fiber_aarch64_swap(&from->sp, to->sp);
}

void psx_fiber_destroy(psx_fiber_t fiber)
{
    psx_fiber_impl* f = (psx_fiber_impl*)fiber;
    if (!f) return;
    free(f->stack);
    free(f);
}

#else /* POSIX: ucontext */

#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
#endif
#ifdef __APPLE__
#  define _DARWIN_C_SOURCE 1
#endif

#include <ucontext.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>    /* SIGSTKSZ — minimum fiber stack floor */

/* glibc >= 2.34 no longer exposes SIGSTKSZ as a compile-time constant under
 * _XOPEN_SOURCE; provide a portable fallback (used only as a stack floor). */
#ifndef SIGSTKSZ
#  define SIGSTKSZ 16384
#endif

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

typedef struct psx_fiber_impl {
    ucontext_t      ctx;
    void*           stack;    /* NULL for the thread-fiber */
    psx_fiber_entry entry;
    void*           arg;
} psx_fiber_impl;

/* Cooperative + single-threaded, so a plain static tracks who's running. */
static psx_fiber_impl* s_current = NULL;

/* makecontext only passes ints; split the fiber pointer across two. */
static void psx_fiber_trampoline(unsigned int hi, unsigned int lo)
{
    uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    psx_fiber_impl* f = (psx_fiber_impl*)p;
    f->entry(f->arg);
    /* The BIOS thread entry never returns normally (it switches back to its
     * scheduler target, or trap_crashes). Reaching here is a bug. */
    abort();
}

psx_fiber_t psx_fiber_convert_thread(void)
{
    if (!s_current) {
        psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
        f->stack = NULL;       /* runs on the real thread stack */
        s_current = f;
    }
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_current(void) { return (psx_fiber_t)s_current; }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    if (stack_size < SIGSTKSZ) stack_size = SIGSTKSZ;
    psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->stack = malloc(stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->entry = entry;
    f->arg   = arg;
    if (getcontext(&f->ctx) != 0) { free(f->stack); free(f); return NULL; }
    f->ctx.uc_stack.ss_sp   = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link          = NULL;   /* entry never returns; see trampoline */
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->ctx, (void (*)(void))psx_fiber_trampoline, 2,
                (unsigned int)(p >> 32), (unsigned int)(p & 0xFFFFFFFFu));
    return (psx_fiber_t)f;
}

void psx_fiber_switch(psx_fiber_t target)
{
    psx_fiber_impl* to   = (psx_fiber_impl*)target;
    psx_fiber_impl* from = s_current;
    if (!to || to == from) return;
    s_current = to;
    swapcontext(&from->ctx, &to->ctx);
    /* Resumed: s_current was set back to `from` by whoever switched here. */
}

void psx_fiber_destroy(psx_fiber_t fiber)
{
    psx_fiber_impl* f = (psx_fiber_impl*)fiber;
    if (!f) return;
    free(f->stack);
    free(f);
}

#endif /* _WIN32 */
