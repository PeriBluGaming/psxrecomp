#pragma once
/* Nintendo Switch process setup (runtime/src/switch_platform.c). */
#if defined(__SWITCH__)
#ifdef __cplusplus
extern "C" {
#endif
/* Call first in main(): routes stdout/stderr to nxlink when launched by it. */
void psx_switch_platform_init(void);
#ifdef __cplusplus
}
#endif
#endif
