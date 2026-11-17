# Nintendo Switch (homebrew, libnx)

Status: **experimental bring-up.** The runtime configures and compiles for
`aarch64-none-elf`; nothing has been verified on hardware yet.

## Requirements

- devkitPro with `switch-dev`, `switch-cmake`, `switch-sdl2`, `switch-mesa`,
  `switch-libdrm_nouveau`, `switch-zlib` (`sudo dkp-pacman -S ...`)
- Host CMake ≥ 3.20 and Ninja (macOS: `brew install cmake ninja`)
- Generated C is produced on the host as usual (the recompiler never runs on
  the Switch). Regenerate it with the same psxrecomp revision you build.

## First milestone: BIOS-only runtime

```bash
git submodule update --init --recursive
cmake -S runtime -B build-switch -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release -DPSX_RECOMP_UI=OFF
cmake --build build-switch --target psx-runtime
```

Copy `build-switch/psx-runtime.nro` to `sdmc:/switch/psxrecomp/` together
with `bios/openbios.bin` (as `sdmc:/switch/psxrecomp/bios/openbios.bin`).
Launch from hbmenu via **title override** (hold R while starting a game), not
from the album applet, which only has a few hundred MB of memory.

## What differs from desktop

| Area | Switch behaviour |
|---|---|
| SDL | SDL2 only (`switch-sdl2`); SDL3 is not packaged for homebrew |
| GL | desktop GL through switch-mesa (EGL + glapi + drm_nouveau) |
| BIOS fibers | AArch64 stack switch in `psx_fiber.c` (newlib has no ucontext) |
| Overlays | build-time static shard or dirty-RAM interpreter; no in-session compile (`autocompile_unavailable.c`), no `dlopen` |
| Relaunch | `envSetNextLoad` (hbloader) instead of `exec` |
| Paths | `sdmc:/…` device paths count as absolute (`host_path.h`) |
| Vulkan / OpenXR / netplay | not available |

Platform wiring lives in `cmake/psx_platform_switch.cmake`.

## Known open items

- recomp-ui (launcher) needs two Switch fixes in its own repository:
  the ImGui GL3 loader uses `dlopen`, and `tinyfiledialogs.c` needs
  `termios.h`. Until then build with `-DPSX_RECOMP_UI=OFF`.
- GL 1.x entry points are called directly by `gpu_gl_renderer.c`; whether
  switch-mesa's static `glapi` exports them is confirmed only at link time.
