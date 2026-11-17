# psx_platform_switch.cmake — Nintendo Switch homebrew (devkitA64 + libnx).
#
# Active only when configured with devkitPro's toolchain file, which sets
# NINTENDO_SWITCH:
#
#   cmake -B build-switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Generated game/BIOS C is produced on the host first (the recompiler is a
# host tool and is never cross-compiled); this file only shapes the runtime.
#
# Platform facts this file encodes:
#   * SDL: devkitPro ships switch-sdl2 (static). SDL3 is not packaged for
#     Switch homebrew, so the SDL2 backend is required.
#   * GL: switch-mesa provides desktop GL through EGL + glapi + drm_nouveau,
#     exported as the OpenGL::GL config package. It exports no gl* symbols,
#     so the core functions the renderers call directly are trampolines in
#     runtime/src/gl_switch_entry.c.
#   * No fork/exec and no dynamic loader: in-session overlay compilation is
#     replaced by autocompile_unavailable.c; overlays run from the build-time
#     static shard or the dirty-RAM interpreter.
#   * No Vulkan, no OpenXR. Netplay stays off (recomp-net is not ported).
#   * Output is an .nro for hbmenu, built next to the .elf.

include_guard(GLOBAL)

if(NOT NINTENDO_SWITCH)
    return()
endif()

message(STATUS "psxrecomp: target platform = Nintendo Switch (libnx)")

# ---- language level ------------------------------------------------------
# devkitA64's GCC 16 defaults to gnu++20, where path::u8string() returns
# std::u8string; the runtime is written against C++17 (what AppleClang, MSVC
# and the desktop GCC/Clang builds use by default). Pin it unless the project
# chose a standard itself.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)
endif()

# ---- option defaults (set before runtime.cmake declares the options, so
# ---- option()/set(CACHE) keep these; an explicit -D still wins) ----------
set(PSX_SDL_BACKEND "SDL2" CACHE STRING "SDL backend (SDL3 or SDL2)")
set(PSX_SDL3_FETCH OFF CACHE BOOL
    "Fetch the pinned SDL3 release when no system SDL3 package is found")
set(PSX_STATIC_RUNTIME ON CACHE BOOL
    "Statically link SDL + libgcc/libstdc++ for a self-contained exe")
set(PSX_ENABLE_VULKAN OFF CACHE BOOL
    "Build the Vulkan renderer backend when SDK tools are available")
set(PSX_OPENXR OFF CACHE BOOL "Build the OpenXR backend")
set(PSX_NETPLAY OFF CACHE BOOL
    "Link recomp-net delay-sync (opt-in; needs recomp-net)")
set(PSX_ZLIB_FETCH OFF CACHE BOOL
    "Fetch zlib when no system ZLIB package is found")

# NRO metadata (hbmenu title/author/icon). The icon must be a 256x256 JPEG;
# empty uses libnx's default icon.
set(PSX_SWITCH_APP_TITLE "" CACHE STRING
    "hbmenu title of the .nro (empty = the runtime target's WINDOW_TITLE or name)")
set(PSX_SWITCH_APP_AUTHOR "PSXRecomp" CACHE STRING "hbmenu author of the .nro")
set(PSX_SWITCH_APP_ICON "" CACHE FILEPATH "256x256 JPEG icon for the .nro")

# ---- hard requirements ---------------------------------------------------
function(psx_switch_validate_options)
    if(NOT PSX_SDL_BACKEND STREQUAL "SDL2")
        message(FATAL_ERROR
            "psxrecomp: Nintendo Switch requires PSX_SDL_BACKEND=SDL2 "
            "(devkitPro packages switch-sdl2; there is no SDL3 portlib). "
            "Got '${PSX_SDL_BACKEND}'.")
    endif()
    foreach(_opt PSX_ENABLE_VULKAN PSX_OPENXR PSX_NETPLAY)
        if(${_opt})
            message(FATAL_ERROR
                "psxrecomp: ${_opt}=ON is not supported on Nintendo Switch.")
        endif()
    endforeach()
endfunction()

# Swap host-only sources for their Switch backends. Called on the runtime
# source list after runtime.cmake builds it.
function(psx_switch_adjust_sources list_var)
    set(_srcs ${${list_var}})
    list(REMOVE_ITEM _srcs "${PSXRECOMP_ROOT}/runtime/src/autocompile.c")
    list(APPEND _srcs "${PSXRECOMP_ROOT}/runtime/src/autocompile_unavailable.c")
    # Core GL entry points resolved through eglGetProcAddress (switch-mesa
    # exports no gl* symbols).
    list(APPEND _srcs "${PSXRECOMP_ROOT}/runtime/src/gl_switch_entry.c")
    # nxlink stdout/stderr redirection for on-console diagnostics.
    list(APPEND _srcs "${PSXRECOMP_ROOT}/runtime/src/switch_platform.c")
    set(${list_var} ${_srcs} PARENT_SCOPE)
endfunction()

# Platform link line + .nro packaging for one runtime executable.
function(psx_switch_finalize_target target title)
    # Desktop GL via switch-mesa (OpenGLConfig.cmake from the portlib).
    find_package(OpenGL CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PRIVATE OpenGL::GL Threads::Threads)

    if(PSX_SWITCH_APP_TITLE)
        set(_title "${PSX_SWITCH_APP_TITLE}")
    elseif(title)
        set(_title "${title}")
    else()
        set(_title "${target}")
    endif()

    set(_version "${PROJECT_VERSION}")
    if(NOT _version)
        set(_version "1.0.0")
    endif()

    set(_nacp "${CMAKE_CURRENT_BINARY_DIR}/${target}.nacp")
    nx_generate_nacp(OUTPUT "${_nacp}"
        NAME "${_title}"
        AUTHOR "${PSX_SWITCH_APP_AUTHOR}"
        VERSION "${_version}")

    set(_nro_args NACP "${_nacp}")
    if(PSX_SWITCH_APP_ICON)
        list(APPEND _nro_args ICON "${PSX_SWITCH_APP_ICON}")
    endif()
    nx_create_nro(${target} ${_nro_args})
    message(STATUS "psxrecomp: ${target} -> ${target}.nro (title '${_title}')")
endfunction()
