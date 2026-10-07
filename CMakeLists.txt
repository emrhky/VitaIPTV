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
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/iptv.c
  src/player.c
  src/httpio.c
  src/tsdemux.c
  src/mkvdemux.c
  src/probe.c
  src/tsplayer.c
  src/ui.c
  src/screens.c
  src/lang.c
  src/mpadec.c
  src/hls.c
  src/curlio.c
)

# https:// through libcurl + OpenSSL (vdpm packages curl, openssl, zlib, zstd; "vdpm install-all" has them).
# The Vita's own TLS fails on most sites today. Turn off with -DVITAIPTV_HTTPS=OFF if the link fails.
option(VITAIPTV_HTTPS "https:// through libcurl + OpenSSL" ON)
set(HTTPS_LIBS "")
if(VITAIPTV_HTTPS)
  find_library(CURL_LIB curl)
  find_library(SSL_LIB ssl)
  find_library(CRYPTO_LIB crypto)
  find_library(ZSTD_LIB zstd)
  if(CURL_LIB AND SSL_LIB AND CRYPTO_LIB)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE HAVE_CURL)
    set(HTTPS_LIBS ${CURL_LIB} ${SSL_LIB} ${CRYPTO_LIB})
    if(ZSTD_LIB)
      list(APPEND HTTPS_LIBS ${ZSTD_LIB})
    endif()
    message(STATUS "HTTPS: libcurl + OpenSSL (${CURL_LIB})")
  else()
    message(WARNING "curl / OpenSSL not found in the SDK (vdpm install curl openssl): https:// uses the Vita's own TLS only")
  endif()
endif()

# Extra system stubs that vita2d needs (found automatically if present in the SDK)
file(GLOB EXTRA_STUBS
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceSharedFb*_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceAppMgr_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceIme_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceKernelModulemgr_stub.a")
message(STATUS "Extra stubs: ${EXTRA_STUBS}")

target_link_libraries(${PROJECT_NAME}.elf
  vita2d
  ${HTTPS_LIBS}
  freetype png jpeg z m pthread
  SceAvPlayer_stub SceAudio_stub SceVideodec_stub SceAudiodec_stub
  SceNet_stub SceNetCtl_stub SceHttp_stub SceSsl_stub
  SceCtrl_stub SceSysmodule_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub
  ScePgf_stub ScePvf_stub SceAppUtil_stub ScePower_stub
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
)
