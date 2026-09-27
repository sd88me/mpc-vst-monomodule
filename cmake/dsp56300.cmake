# DSP56300 emulator (GPLv3, https://github.com/dsp56300/dsp56300), forked at sd88me/dsp56300 branch
# `arm32` (this repo's `libs/dsp56300` submodule) for the 32-bit ARM static recompiler this port needs
# — see libs/dsp56300/docs/ARM32_JIT.md. Only the emulator's static libraries are built: its own
# top-level CMake project adds tools and global optimisation flags this project does not want.
set(MNM_DSP56300_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/dsp56300" CACHE PATH
    "dsp56300 checkout (the arm32 fork); defaults to this repo's own submodule")
if(NOT EXISTS "${MNM_DSP56300_DIR}/source/dsp56kEmu/dsp.h")
  message(FATAL_ERROR "dsp56300 not found at ${MNM_DSP56300_DIR} -- run 'git submodule update --init' first, "
                       "or pass -DMNM_DSP56300_DIR=<an existing checkout>")
endif()

set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
add_subdirectory("${MNM_DSP56300_DIR}/source/asmjit"     "${CMAKE_BINARY_DIR}/dsp56300/asmjit"     EXCLUDE_FROM_ALL)
add_subdirectory("${MNM_DSP56300_DIR}/source/dsp56kBase" "${CMAKE_BINARY_DIR}/dsp56300/dsp56kBase" EXCLUDE_FROM_ALL)
add_subdirectory("${MNM_DSP56300_DIR}/source/dsp56kEmu"  "${CMAKE_BINARY_DIR}/dsp56300/dsp56kEmu"  EXCLUDE_FROM_ALL)
if(UNIX AND NOT APPLE)   # dsp56kEmu links vtuneSdk unconditionally on Linux (its own CMakeLists.txt)
  add_subdirectory("${MNM_DSP56300_DIR}/source/vtuneSdk" "${CMAKE_BINARY_DIR}/dsp56300/vtuneSdk" EXCLUDE_FROM_ALL)
endif()
