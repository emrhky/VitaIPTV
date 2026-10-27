cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(VitaIPTV C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vita IPTV")
set(VITA_TITLEID  "VIPTV0001")
# the version, in one place: the VPK's "00.70" and the app's "v0.7.0" (shown in About and in the log) both come from here
set(APP_VER_MAJOR 1)
set(APP_VER_MINOR 0)
set(APP_VER_PATCH 0)
# The VPK takes "XX.YY" (two digits each side): minor and patch share the two digits, so a patch above 9
# is written as 9 (0.2.10 -> "00.29"; 0.3.0 -> "00.30" is still higher).
if(APP_VER_PATCH GREATER 9)
  set(VITA_VER_PATCH 9)
else()
  set(VITA_VER_PATCH ${APP_VER_PATCH})
endif()
set(VITA_VERSION  "0${APP_VER_MAJOR}.${APP_VER_MINOR}${VITA_VER_PATCH}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
# VitaSDK since 08/2026 starts the data segment at the next 64 KiB boundary; when the code ends just below one,
# vita-elf-create has no room for its SCE data ("Cannot allocate ... segment 1 overlaps", 0.4.0). Leave room
# for it (older linkers ignore the symbol).
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=__sce_headroom=0x4000")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/iptv.c
  src/player.c
  src/httpio.c
  src/tsdemux.c
  src/mkvdemux.c
  src/flvdemux.c
  src/probe.c
  src/tsplayer.c
  src/ui.c
  src/screens.c
  src/lang.c
  src/mpadec.c
  src/hls.c
  src/spsfix.c
  src/curlio.c
  src/vod.c
  src/fav.c
  src/epg.c
  src/a52dec.c
  src/guide.c
  src/catchup.c
  src/poster.c
)
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE "APP_VERSION=\"v${APP_VER_MAJOR}.${APP_VER_MINOR}.${APP_VER_PATCH}\"")
# 0.4.1: when this build was made (the log's second line says it, to tell builds apart while testing). Set when
# CMake runs: build in a fresh folder (rm -rf build) for a new time.
string(TIMESTAMP APP_BUILD_TIME "%Y-%m-%d %H:%M")
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE "APP_BUILD=\"${APP_BUILD_TIME}\"")

# https:// with the app's own TLS: mbedTLS 3.6 (third_party/mbedtls, Apache-2.0) and a small HTTP client
# (src/curlio.c). The Vita's own TLS fails on most sites today; the SDK's libcurl does not link with its
# OpenSSL. -DVITAIPTV_HTTPS=OFF builds without it (https:// then uses the Vita's own TLS only).
option(VITAIPTV_HTTPS "https:// with the built-in mbedTLS" ON)
if(VITAIPTV_HTTPS)
  file(GLOB MBEDTLS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/library/*.c)
  target_sources(${PROJECT_NAME}.elf PRIVATE ${MBEDTLS_SRC})
  target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE HAVE_TLS "MBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"")
  message(STATUS "HTTPS: built-in mbedTLS")
endif()

# 0.5.0: AC-3 / E-AC-3 (Dolby Digital / Dolby Digital Plus) with FFmpeg's decoders, LGPL 2.1+. Built once with
# tools/build_ffmpeg_vita.sh into third_party/ffmpeg-vita; without it the app builds as before (no AC-3 sound).
set(FFMPEG_VITA ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-vita)
set(A52_LIBS "")
set(A52_VPK_FILES "")
if(EXISTS ${FFMPEG_VITA}/lib/libavcodec.a AND EXISTS ${FFMPEG_VITA}/lib/libavutil.a)
  target_include_directories(${PROJECT_NAME}.elf BEFORE PRIVATE ${FFMPEG_VITA}/include)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE HAVE_A52)
  set(A52_LIBS ${FFMPEG_VITA}/lib/libavcodec.a ${FFMPEG_VITA}/lib/libavutil.a)
  if(EXISTS ${FFMPEG_VITA}/COPYING.LGPLv2.1)
    set(A52_VPK_FILES FILE third_party/ffmpeg-vita/COPYING.LGPLv2.1 licenses/ffmpeg-COPYING.LGPLv2.1.txt)
  endif()
  message(STATUS "AC-3 / E-AC-3: FFmpeg from third_party/ffmpeg-vita")
  # 0.6.0: 5.1 / 7.1 AAC needs FFmpeg's aac decoder too (built by tools/build_ffmpeg_vita.sh since 0.6.0)
  file(STRINGS ${FFMPEG_VITA}/lib/libavcodec.a FF_AAC LIMIT_COUNT 1 REGEX "ff_aac_decoder")
  if(FF_AAC)
    message(STATUS "AAC 5.1 / 7.1: FFmpeg's AAC decoder is built in")
  else()
    message(WARNING "AAC 5.1 / 7.1: this FFmpeg has no AAC decoder (built before 0.6.0): run tools/build_ffmpeg_vita.sh again "
                    "(rm -rf third_party/ffmpeg-vita build-ffmpeg first). Without it such films play without sound.")
  endif()
else()
  message(STATUS "AC-3 / E-AC-3: not built in (run tools/build_ffmpeg_vita.sh)")
endif()

# Extra system stubs that vita2d needs (found automatically if present in the SDK)
file(GLOB EXTRA_STUBS
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceSharedFb*_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceAppMgr_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceIme_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceKernelModulemgr_stub.a")
message(STATUS "Extra stubs: ${EXTRA_STUBS}")

target_link_libraries(${PROJECT_NAME}.elf
  ${A52_LIBS}
  vita2d
  freetype png jpeg z m pthread
  SceRtc_stub
  SceAvPlayer_stub SceAudio_stub SceVideodec_stub SceCodecEngine_stub SceAudiodec_stub
  SceNet_stub SceNetCtl_stub SceHttp_stub SceSsl_stub
  SceCtrl_stub SceSysmodule_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub
  ScePgf_stub ScePvf_stub SceAppUtil_stub ScePower_stub SceAVConfig_stub SceRegistryMgr_stub
  ${EXTRA_STUBS}
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE resources/splash.png resources/splash.png
  FILE resources/waiting.png resources/waiting.png
  FILE LICENSE LICENSE.txt
  FILE THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.txt
  FILE third_party/mbedtls/LICENSE licenses/mbedtls-LICENSE.txt
  ${A52_VPK_FILES}
)
