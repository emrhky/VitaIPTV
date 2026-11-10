cmake_minimum_required(VERSION 3.5)

# Vita IPTV hardware test: a small separate test app (see main.c), not part of Vita IPTV.
# Build: cmake -B build && cmake --build build   (in this folder, with VITASDK set) -> build/HwBench.vpk
# Test files: ./make_files.sh (needs ffmpeg with libx264) -> copy hwbench_files/*.h264 to ux0:data/hwbench/

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(HwBench C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
add_executable(hwbench.elf main.c dec.c gpu.c h264parse.c)
target_link_libraries(hwbench.elf
  SceVideodec_stub SceCodecEngine_stub SceSysmodule_stub
  SceGxm_stub SceShaccCg_stub SceDisplay_stub SceCtrl_stub ScePower_stub
  m
)
vita_create_self(hwbench.self hwbench.elf)
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_vpk(HwBench.vpk VIPTVHWB1 hwbench.self
  VERSION 01.00
  NAME "HW Test"
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../sce_sys/icon0.png sce_sys/icon0.png
)
