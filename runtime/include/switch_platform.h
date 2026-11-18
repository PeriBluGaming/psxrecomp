#pragma once
/* Nintendo Switch process setup (runtime/src/switch_platform.c). */
#if defined(__SWITCH__)
#ifdef __cplusplus
extern "C" {
#endif
/* Call first in main() with argv[0]: routes stdout/stderr to nxlink when
 * launched by it, else to an existing psxrecomp_stdio.txt next to the NRO. */
void psx_switch_platform_init(const char* argv0);
#ifdef __cplusplus
}
#endif
#endif
