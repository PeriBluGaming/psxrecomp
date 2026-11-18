#pragma once
/* Nintendo Switch process setup (runtime/src/switch_platform.c). */
#if defined(__SWITCH__)
#ifdef __cplusplus
extern "C" {
#endif
/* Call first in main() with argv[0]: routes stdout/stderr to nxlink when
 * launched by it, else to an existing psxrecomp_stdio.txt next to the NRO. */
void psx_switch_platform_init(const char* argv0);
/* Runs fn(argc, argv) on a thread with PSX_SWITCH_MAIN_STACK bytes of stack
 * at the calling thread's priority, waits for it and returns its result. */
#define PSX_SWITCH_MAIN_STACK (16u * 1024u * 1024u)
int psx_switch_run_main(int (*fn)(int, char**), int argc, char** argv);
#ifdef __cplusplus
}
#endif
#endif
