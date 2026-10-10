cmake_minimum_required(VERSION 3.5)

# Vita IPTV decoder probe: a small separate test app (see main.c). Builds two VPKs:
#   VitaIPTVProbe.vpk   - normal build              (title ID VIPTVPRB1)
#   VitaIPTVProbeU.vpk  - "unsafe" build, no ASLR, memory expansion (ATTRIBUTE2=12) (title ID VIPTVPRB2)
# Build: cmake -B build && cmake --build build   (in this folder, with VITASDK set)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(VitaIPTVProbe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
set(ICON ${CMAKE_CURRENT_SOURCE_DIR}/../../sce_sys/icon0.png)
set(PROBE_LIBS
  vita2d
  png jpeg z m
  SceVideodec_stub SceCodecEngine_stub SceAppMgr_stub
  SceCtrl_stub SceSysmodule_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub
  ScePgf_stub SceAppUtil_stub
)
file(GLOB EXTRA_STUBS "$ENV{VITASDK}/arm-vita-eabi/lib/libSceSharedFb*_stub.a")

# vita_create_self / vita_create_vpk are macros that append to flag variables: each build goes through
# a function so the flags of one VPK (unsafe, ATTRIBUTE2) cannot leak into the other.
function(probe_build elf self vpk titleid name unsafe)
  add_executable(${elf} main.c)
  target_link_libraries(${elf} ${PROBE_LIBS} ${EXTRA_STUBS})
  unset(VITA_MKSFOEX_FLAGS CACHE)
  if(unsafe)
    target_compile_definitions(${elf} PRIVATE PROBE_UNSAFE)
    vita_create_self(${self} ${elf} UNSAFE NOASLR)
    set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")          # memory expansion, as ViTube / wiliwili use
  else()
    vita_create_self(${self} ${elf})
    set(VITA_MKSFOEX_FLAGS "")
  endif()
  vita_create_vpk(${vpk} ${titleid} ${self}
    VERSION 01.00
    NAME "${name}"
    FILE ${ICON} sce_sys/icon0.png
  )
endfunction()

probe_build(probe_safe.elf probe_safe.self VitaIPTVProbe.vpk VIPTVPRB1 "IPTV Probe" OFF)
probe_build(probe_unsafe.elf probe_unsafe.self VitaIPTVProbeU.vpk VIPTVPRB2 "IPTV Probe U" ON)
