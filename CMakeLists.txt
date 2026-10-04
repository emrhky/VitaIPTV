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
)

target_link_libraries(${PROJECT_NAME}.elf
  vita2d
  curl mbedtls mbedx509 mbedcrypto z
  freetype png jpeg m
  SceAvPlayer_stub SceAudio_stub
  SceNet_stub SceNetCtl_stub SceHttp_stub SceSsl_stub
  SceCtrl_stub SceSysmodule_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
