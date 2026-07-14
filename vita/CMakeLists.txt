#---------------------------------------------------------------------------------
# OpenTyrian 2000 (widescreen fork) -- Sony PlayStation Vita build
#
# Produces eboot.bin (a Sony SELF); vita/build.sh wraps this and assembles the final
# OpenTyrian2000.vpk (param.sfo + LiveArea assets + the freeware Tyrian data). Build with:
#   ./vita/build.sh          (from Git Bash / MSYS with VITASDK set)
#
# Requires VitaSDK with the sdl2 package installed (vdpm sdl2).
#---------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)

## VitaSDK toolchain -----------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set in the environment. export VITASDK=<path to>vitasdk")
  endif()
endif()

project(OpenTyrian2000 C)

# vita.toolchain.cmake (above) sets up the arm-vita-eabi compiler/sysroot; vita.cmake adds the
# vita_create_self / vita_create_vpk helper macros.
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

## VPK / LiveArea metadata (also used by build.sh) -----------------------------
set(VITA_APP_NAME "OpenTyrian 2000")
set(VITA_TITLEID  "OTYR20000")
set(VITA_VERSION  "01.00")

## Compiler flags --------------------------------------------------------------
# -fsigned-char        : ARM chars are unsigned by default; the DOS-era engine assumes signed x86.
# -fno-strict-aliasing : the binary data-file loaders type-pun freely.
add_compile_options(-Wall -O2 -fsigned-char -fno-strict-aliasing)

# Compile the whole engine. Every src/*.c is portable or self-guards on its platform macro
# (win_native_midi.c/crashlog.c -> _WIN32, network.c -> WITH_NETWORK, switch_platform.c ->
# __SWITCH__, vita_platform.c -> __vita__), so the flat glob is safe. midiproc/ is NOT globbed
# (MIDI is a Windows-only extra, off here, exactly like the Switch build).
file(GLOB OT_SOURCES "${CMAKE_SOURCE_DIR}/../src/*.c")

add_executable(${PROJECT_NAME} ${OT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
  "${CMAKE_SOURCE_DIR}/../src"
  "$ENV{VITASDK}/arm-vita-eabi/include/SDL2"   # the engine includes "SDL.h" unqualified
)

# -DNDEBUG: this is a RELEASE build (what players run) -- compile out assert() exactly like
# the desktop MSVC Release config and the Switch build. Several asserts are latent tripwires
# the engine relies on NDEBUG to elide (blit_sprite bounds, JE_loadCompShapesB, ...); with
# them live, a Vita build would abort where desktop Release silently returns. __vita__ is
# predefined by the VitaSDK gcc, so it needs no -D. TYRIAN_DIR is a compile-time fallback
# data path; file.c prepends ux0:/app0: at runtime and takes priority.
target_compile_definitions(${PROJECT_NAME} PRIVATE
  NDEBUG
  TARGET_UNIX
  TYRIAN_DIR="app0:data"
)

# SDL2 + its Vita stub dependency chain (mirrors sdl2.pc's Libs), plus the threading stub SDL
# uses and libm. Order matters for the static GNU linker: SDL2 before its Sce* stubs.
target_link_libraries(${PROJECT_NAME}
  SDL2
  SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub SceHid_stub
  SceAudio_stub SceAudioIn_stub SceCommonDialog_stub SceSysmodule_stub
  SceAppMgr_stub SceAppUtil_stub ScePower_stub SceProcessmgr_stub SceIofilemgr_stub
  SceKernelThreadMgr_stub
  m
)

## eboot.bin (SELF). UNSAFE: the game reads/writes ux0: outside its own app0: sandbox.
## The VPK itself is assembled by build.sh (see the header comment).
vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
