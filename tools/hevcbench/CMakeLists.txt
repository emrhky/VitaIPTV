cmake_minimum_required(VERSION 3.5)

# H.265 software decoding speed test (see main.c): a separate small app, title ID VIPTVHEV1.
# First: ./build_ffmpeg_hevc.sh (FFmpeg with the H.265 / H.264 decoders into ../../third_party/ffmpeg-hevc).
# Then in this folder: cmake -B build && cmake --build build   (VITASDK set) -> build/HevcBench.vpk

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(HevcBench C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(FF "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg-hevc")
if(NOT EXISTS "${FF}/lib/libavcodec.a")
  message(FATAL_ERROR "Run ./build_ffmpeg_hevc.sh first (${FF}/lib/libavcodec.a is missing)")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
add_executable(hevcbench.elf main.c bench.c)
target_include_directories(hevcbench.elf PRIVATE "${FF}/include")
target_link_options(hevcbench.elf PRIVATE "-Wl,--wrap=pthread_create")
target_link_libraries(hevcbench.elf
  "${FF}/lib/libavcodec.a" "${FF}/lib/libavutil.a"
  m pthread
  SceCtrl_stub SceDisplay_stub ScePower_stub
)
vita_create_self(hevcbench.self hevcbench.elf)
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")              # more memory (1080p with frame threads)
vita_create_vpk(HevcBench.vpk VIPTVHEV1 hevcbench.self
  VERSION 01.00
  NAME "H.265 Test"
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../sce_sys/icon0.png sce_sys/icon0.png
)
