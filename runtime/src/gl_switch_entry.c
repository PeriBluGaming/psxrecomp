/* gl_switch_entry.c — GL entry points for Nintendo Switch (switch-mesa).
 *
 * The renderers call a set of core GL functions directly (the ones desktop
 * platforms export from libGL/opengl32). switch-mesa links statically through
 * EGL + glapi and exports no gl* symbols, so each of them is defined here as
 * a register-preserving AArch64 trampoline: the first call resolves the real
 * function with eglGetProcAddress (Mesa returns core entry points too) and
 * caches it; every call then tail-branches with the caller's arguments
 * untouched, so no per-function signature is needed.
 *
 * The list is the set of gl* symbols the runtime objects leave undefined
 * (a superset is harmless: unused trampolines are never resolved). A name eglGetProcAddress cannot resolve aborts: calling on with
 * a null function would be undefined behaviour, never a usable fallback.
 *
 * Built only on Switch (see cmake/psx_platform_switch.cmake). */
#if defined(__SWITCH__) && defined(__aarch64__)

#include <stdint.h>
#include <stdlib.h>

typedef void (*psx_gl_proc)(void);
/* EGL 1.5: __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*) */
extern psx_gl_proc eglGetProcAddress(const char* procname);

static const char* const k_psx_gl_names[] = {
    "glBindTexture",
    "glClear",
    "glClearColor",
    "glClearDepth",
    "glClearStencil",
    "glColorMask",
    "glCopyTexImage2D",
    "glCopyTexSubImage2D",
    "glDeleteTextures",
    "glDepthFunc",
    "glDepthMask",
    "glDisable",
    "glDrawArrays",
    "glEnable",
    "glFinish",
    "glFlush",
    "glGenTextures",
    "glGetBooleanv",
    "glGetError",
    "glGetFloatv",
    "glGetIntegerv",
    "glGetString",
    "glGetTexImage",
    "glGetTexLevelParameteriv",
    "glGetTexParameteriv",
    "glIsEnabled",
    "glLineWidth",
    "glPixelStorei",
    "glReadBuffer",
    "glReadPixels",
    "glScissor",
    "glStencilFunc",
    "glStencilMask",
    "glStencilOp",
    "glTexImage2D",
    "glTexParameterf",
    "glTexParameteri",
    "glTexSubImage2D",
    "glViewport",
};
#define PSX_GL_ENTRY_COUNT 39

_Static_assert(sizeof(k_psx_gl_names) / sizeof(k_psx_gl_names[0]) == PSX_GL_ENTRY_COUNT,
               "trampoline table and name list out of sync");

/* Read by the trampolines below (symbol referenced from asm). */
psx_gl_proc psx_gl_switch_table[PSX_GL_ENTRY_COUNT];

psx_gl_proc psx_gl_switch_resolve(uint32_t index);
psx_gl_proc psx_gl_switch_resolve(uint32_t index)
{
    if (index >= PSX_GL_ENTRY_COUNT) abort();
    psx_gl_proc proc = eglGetProcAddress(k_psx_gl_names[index]);
    if (!proc) abort();   /* the driver lacks a core entry point the renderer needs */
    __atomic_store_n(&psx_gl_switch_table[index], proc, __ATOMIC_RELEASE);
    return proc;
}

/* Slow path, entered from a trampoline with x17 = table index and the
 * caller's arguments still in x0-x8 / q0-q7. */
__asm__(
    ".text\n"
    ".balign 16\n"
    ".type psx_gl_switch_lazy, %function\n"
    "psx_gl_switch_lazy:\n"
    "    stp  x29, x30, [sp, #-224]!\n"
    "    mov  x29, sp\n"
    "    stp  x0, x1, [sp, #16]\n"
    "    stp  x2, x3, [sp, #32]\n"
    "    stp  x4, x5, [sp, #48]\n"
    "    stp  x6, x7, [sp, #64]\n"
    "    str  x8,     [sp, #80]\n"
    "    stp  q0, q1, [sp, #96]\n"
    "    stp  q2, q3, [sp, #128]\n"
    "    stp  q4, q5, [sp, #160]\n"
    "    stp  q6, q7, [sp, #192]\n"
    "    mov  w0, w17\n"
    "    bl   psx_gl_switch_resolve\n"
    "    mov  x17, x0\n"
    "    ldp  x0, x1, [sp, #16]\n"
    "    ldp  x2, x3, [sp, #32]\n"
    "    ldp  x4, x5, [sp, #48]\n"
    "    ldp  x6, x7, [sp, #64]\n"
    "    ldr  x8,     [sp, #80]\n"
    "    ldp  q0, q1, [sp, #96]\n"
    "    ldp  q2, q3, [sp, #128]\n"
    "    ldp  q4, q5, [sp, #160]\n"
    "    ldp  q6, q7, [sp, #192]\n"
    "    ldp  x29, x30, [sp], #224\n"
    "    br   x17\n"
    ".size psx_gl_switch_lazy, .-psx_gl_switch_lazy\n"
);

#define PSX_GL_TRAMPOLINE(name, index)                                   \
    __asm__(                                                             \
        ".text\n"                                                        \
        ".balign 16\n"                                                   \
        ".global " #name "\n"                                            \
        ".type " #name ", %function\n"                                   \
        #name ":\n"                                                      \
        "    adrp x16, psx_gl_switch_table\n"                            \
        "    add  x16, x16, :lo12:psx_gl_switch_table\n"                 \
        "    ldr  x17, [x16, #(" #index " * 8)]\n"                       \
        "    cbz  x17, 1f\n"                                             \
        "    br   x17\n"                                                 \
        "1:  mov  x17, #" #index "\n"                                    \
        "    b    psx_gl_switch_lazy\n"                                  \
        ".size " #name ", .-" #name "\n")

PSX_GL_TRAMPOLINE(glBindTexture, 0);
PSX_GL_TRAMPOLINE(glClear, 1);
PSX_GL_TRAMPOLINE(glClearColor, 2);
PSX_GL_TRAMPOLINE(glClearDepth, 3);
PSX_GL_TRAMPOLINE(glClearStencil, 4);
PSX_GL_TRAMPOLINE(glColorMask, 5);
PSX_GL_TRAMPOLINE(glCopyTexImage2D, 6);
PSX_GL_TRAMPOLINE(glCopyTexSubImage2D, 7);
PSX_GL_TRAMPOLINE(glDeleteTextures, 8);
PSX_GL_TRAMPOLINE(glDepthFunc, 9);
PSX_GL_TRAMPOLINE(glDepthMask, 10);
PSX_GL_TRAMPOLINE(glDisable, 11);
PSX_GL_TRAMPOLINE(glDrawArrays, 12);
PSX_GL_TRAMPOLINE(glEnable, 13);
PSX_GL_TRAMPOLINE(glFinish, 14);
PSX_GL_TRAMPOLINE(glFlush, 15);
PSX_GL_TRAMPOLINE(glGenTextures, 16);
PSX_GL_TRAMPOLINE(glGetBooleanv, 17);
PSX_GL_TRAMPOLINE(glGetError, 18);
PSX_GL_TRAMPOLINE(glGetFloatv, 19);
PSX_GL_TRAMPOLINE(glGetIntegerv, 20);
PSX_GL_TRAMPOLINE(glGetString, 21);
PSX_GL_TRAMPOLINE(glGetTexImage, 22);
PSX_GL_TRAMPOLINE(glGetTexLevelParameteriv, 23);
PSX_GL_TRAMPOLINE(glGetTexParameteriv, 24);
PSX_GL_TRAMPOLINE(glIsEnabled, 25);
PSX_GL_TRAMPOLINE(glLineWidth, 26);
PSX_GL_TRAMPOLINE(glPixelStorei, 27);
PSX_GL_TRAMPOLINE(glReadBuffer, 28);
PSX_GL_TRAMPOLINE(glReadPixels, 29);
PSX_GL_TRAMPOLINE(glScissor, 30);
PSX_GL_TRAMPOLINE(glStencilFunc, 31);
PSX_GL_TRAMPOLINE(glStencilMask, 32);
PSX_GL_TRAMPOLINE(glStencilOp, 33);
PSX_GL_TRAMPOLINE(glTexImage2D, 34);
PSX_GL_TRAMPOLINE(glTexParameterf, 35);
PSX_GL_TRAMPOLINE(glTexParameteri, 36);
PSX_GL_TRAMPOLINE(glTexSubImage2D, 37);
PSX_GL_TRAMPOLINE(glViewport, 38);

#endif /* __SWITCH__ && __aarch64__ */
