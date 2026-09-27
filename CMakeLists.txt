cmake_minimum_required(VERSION 3.22)
project(MpcVstMonomodule LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Link-time optimisation: bit-exact, and measurably faster (see legsmechanical/schwung-monomodule docs/PERF.md,
# the project this repo's vst/engine glue is vendored from).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(MPC_VST_DIR "" CACHE PATH "mpc-vst-plugins checkout (the shared VST2 wrapper this plugin links)")
if(NOT MPC_VST_DIR)
  message(FATAL_ERROR "set -DMPC_VST_DIR=<mpc-vst-plugins checkout>")
endif()

# Upstream Monomodule (GPLv3, libs/monomodule submodule): the emulator core only. Its JUCE plugins,
# Library app and tests are not built. vst/engine/{MonoVoice,dsp/DspEngine} shadow upstream's own copies
# of those two files (vendored from legsmechanical/schwung-monomodule; see vst/engine/VENDORED.md for why).
set(MNM_CORE "${CMAKE_CURRENT_SOURCE_DIR}/libs/monomodule/src/core")
include(cmake/dsp56300.cmake)

add_library(mnmcore STATIC
  ${MNM_CORE}/firmware/Firmware.cpp
  ${MNM_CORE}/dsp/Digibank.cpp
  ${MNM_CORE}/host/HostModel.cpp
  ${MNM_CORE}/host/Lfo.cpp
  vst/engine/dsp/DspEngine.cpp
  vst/engine/MonoVoice.cpp
)
target_include_directories(mnmcore PUBLIC vst/engine ${MNM_CORE})
target_link_libraries(mnmcore PUBLIC dsp56kEmu)
target_compile_definitions(mnmcore PUBLIC MNM_OS_SYX="")

# monomodule_one (MNM_FX=0, an instrument) and monomodule_fx (MNM_FX=1, an audio effect): the same engine
# source (vst/mnm_engine.cpp), built twice against the shared wrapper (mpc-vst-plugins' vst2_wrap.c).
# Each needs its own generated params.h (mpc-vst's gen_vst.py --params-h, run by vst/build_so.sh) on its
# include path first: vst/build/ for One, vst/fx/build/ for FX.
foreach(variant one fx)
  if(variant STREQUAL "one")
    set(_target monomodule_one)
    set(_fx 0)
    set(_paramsdir "${CMAKE_CURRENT_SOURCE_DIR}/vst/build")
    set(_smoke mnm-vst-smoke)
  else()
    set(_target monomodule_fx)
    set(_fx 1)
    set(_paramsdir "${CMAKE_CURRENT_SOURCE_DIR}/vst/fx/build")
    set(_smoke mnm-vst-smoke-fx)
  endif()

  add_library(${_target} SHARED vst/mnm_engine.cpp ${MNM_CORE}/library/MnmDump.cpp ${MPC_VST_DIR}/wrapper/vst2_wrap.c)
  target_compile_definitions(${_target} PRIVATE MNM_FX=${_fx})
  target_include_directories(${_target} PRIVATE ${MPC_VST_DIR}/wrapper ${_paramsdir})
  target_link_libraries(${_target} PRIVATE mnmcore pthread)
  set_target_properties(${_target} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  # --exclude-libs,ALL,-Bsymbolic: two instances of this plugin (One and FX) link the same static mnmcore/
  # libstdc++, and without this their exported C++ runtime symbols collide when both are loaded into MPC's
  # single process (each .so must keep its own copy private, not share the first one loaded).
  target_link_options(${_target} PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)

  add_executable(${_smoke} vst/smoke.cpp vst/mnm_engine.cpp ${MNM_CORE}/library/MnmDump.cpp)
  target_compile_definitions(${_smoke} PRIVATE MNM_FX=${_fx})
  target_include_directories(${_smoke} PRIVATE ${MPC_VST_DIR}/wrapper)
  target_link_libraries(${_smoke} PRIVATE mnmcore pthread)
endforeach()
