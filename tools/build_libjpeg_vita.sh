#!/bin/bash
# 1.2.4i: builds libjpeg-turbo for the PS Vita WITH its ARM NEON code into third_party/libjpeg-vita.
# VitaSDK's own libjpeg-turbo package is built with -DWITH_SIMD=FALSE (no NEON): a 600x900 poster took 196 ms to
# decode on the Vita. CMakeLists.txt uses this one when it is there (the posters and vita2d's JPEGs), and the
# package's one otherwise (the app works either way, only slower).
#
#   export VITASDK=/home/acer/vitasdk          (as for building the app)
#   ./tools/build_libjpeg_vita.sh              (downloads libjpeg-turbo 3.2.1, about a minute)
#
# libjpeg-turbo: IJG licence + BSD-3-Clause (LICENSE.md, copied into the VPK).
# For a test without VitaSDK: CC_PREFIX=arm-none-eabi- TOOLCHAIN=none ./tools/build_libjpeg_vita.sh (compiles only)
set -e
CC_PREFIX="${CC_PREFIX:-arm-vita-eabi-}"
if [ "${TOOLCHAIN:-vita}" = "vita" ]; then
    : "${VITASDK:?VITASDK is not set (export VITASDK=/path/to/vitasdk)}"
    export PATH="$VITASDK/bin:$PATH"
fi
command -v "${CC_PREFIX}gcc" >/dev/null || { echo "${CC_PREFIX}gcc not found"; exit 1; }

VER=3.2.1
SHA=c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f   # (the same as VitaSDK's package)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/third_party/libjpeg-vita}"
WORK="$ROOT/build-libjpeg"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "libjpeg-turbo-$VER" ]; then
    [ -f "libjpeg-turbo-$VER.tar.gz" ] || \
        curl -fL -o "libjpeg-turbo-$VER.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$VER/libjpeg-turbo-$VER.tar.gz"
    echo "$SHA  libjpeg-turbo-$VER.tar.gz" | sha256sum -c -
    tar xf "libjpeg-turbo-$VER.tar.gz"
fi
rm -rf build && mkdir build && cd build

if [ "${TOOLCHAIN:-vita}" = "vita" ]; then
    TC="-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake"
    FABI=""                         # (VitaSDK's compiler is hard-float already)
else   # (a plain ARM compiler: the same CPU, a static library only)
    TC="-DCMAKE_SYSTEM_NAME=Generic -DCMAKE_SYSTEM_PROCESSOR=armv7-a -DCMAKE_C_COMPILER=${CC_PREFIX}gcc -DCMAKE_ASM_COMPILER=${CC_PREFIX}gcc -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    FABI="-mfloat-abi=hard"         # (as the Vita)
fi
# (the same options as VitaSDK's package, with the NEON code required: no silent build without it)
cmake "../libjpeg-turbo-$VER" $TC \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_C_FLAGS="-mcpu=cortex-a9 -mfpu=neon $FABI -ffunction-sections -fdata-sections" \
    -DCMAKE_ASM_FLAGS="-mcpu=cortex-a9 -mfpu=neon $FABI" \
    -DENABLE_SHARED=FALSE -DENABLE_STATIC=TRUE -DWITH_TOOLS=FALSE -DWITH_TESTS=FALSE \
    -DWITH_SIMD=TRUE -DREQUIRE_SIMD=TRUE | tee cmake.log
grep -qi "SIMD extensions: arm (WITH_SIMD = 1)\|SIMD extensions: arm" cmake.log || { echo "the NEON code was not enabled"; exit 1; }
make -j"$(nproc)" jpeg-static
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include"
cp libjpeg.a "$OUT/lib/"
cp jconfig.h "../libjpeg-turbo-$VER/src/jpeglib.h" "../libjpeg-turbo-$VER/src/jmorecfg.h" "../libjpeg-turbo-$VER/src/jerror.h" "$OUT/include/"
cp "../libjpeg-turbo-$VER/LICENSE.md" "$OUT/LICENSE.md"
echo "libjpeg-turbo $VER NEON" > "$OUT/build_flags.txt"
echo
echo "Done: $OUT"
ls -l "$OUT/lib"
echo "Now build the app again (rm -rf build; mkdir build; cd build; cmake ..; make): CMake says"
echo "\"Posters: libjpeg-turbo with NEON from third_party/libjpeg-vita\"."
