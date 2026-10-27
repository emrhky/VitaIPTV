#!/bin/bash
# Builds FFmpeg's AC-3, E-AC-3 and AAC decoders (nothing else) for the PS Vita into third_party/ffmpeg-vita.
# CMakeLists.txt finds them there and the app plays Dolby Digital / Dolby Digital Plus audio (0.5.0) and
# 5.1 / 7.1 AAC, which the Vita's own AAC decoder refuses (0.6.0; the stereo AAC stays on the hardware).
# Without this step the app builds as before, without them. An older build (0.5.0, no AAC): run it again.
#
#   export VITASDK=/home/acer/vitasdk          (as for building the app)
#   ./tools/build_ffmpeg_vita.sh               (downloads FFmpeg 7.1, about 5 minutes)
#   ASM=1 ./tools/build_ffmpeg_vita.sh         (with FFmpeg's ARM assembler code; the default leaves it out)
#
# FFmpeg is LGPL 2.1 or later (this configuration uses no GPL parts): see THIRD_PARTY_NOTICES.md.
set -e
: "${VITASDK:?VITASDK is not set (export VITASDK=/path/to/vitasdk)}"
export PATH="$VITASDK/bin:$PATH"
command -v arm-vita-eabi-gcc >/dev/null || { echo "arm-vita-eabi-gcc not found in $VITASDK/bin"; exit 1; }

VER=7.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/ffmpeg-vita"
WORK="$ROOT/build-ffmpeg"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "ffmpeg-$VER" ]; then
    [ -f "ffmpeg-$VER.tar.xz" ] || curl -fL -o "ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
    tar xf "ffmpeg-$VER.tar.xz"
fi
cd "ffmpeg-$VER"

# GCC 14 and later (VitaSDK since 2025) turn some old warnings into errors; FFmpeg 7.1 has a few of them
# (libavutil/opt.c: enum pointers passed as int pointers, in functions this app does not call)
LENIENT="-Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration"

EXTRA=""
[ "${ASM:-0}" = "1" ] || EXTRA="--disable-asm"

make distclean >/dev/null 2>&1 || true
./configure \
    --prefix="$OUT" \
    --enable-cross-compile --cross-prefix=arm-vita-eabi- --arch=arm --cpu=cortex-a9 --target-os=none \
    --disable-everything --enable-decoder=ac3,eac3,aac --enable-parser=ac3 \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-swscale --disable-postproc --disable-avfilter --disable-swresample \
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-runtime-cpudetect \
    --disable-debug --enable-small --enable-static --disable-shared \
    --extra-cflags="-O2 -ffunction-sections -fdata-sections $LENIENT" \
    $EXTRA

make -j"$(nproc)"
rm -rf "$OUT"
make install
cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
echo
echo "Done: $OUT"
ls -l "$OUT/lib"
echo "Now build the app again (rm -rf build; mkdir build; cd build; cmake ..; make): CMake says"
echo "\"AC-3 / E-AC-3: FFmpeg from third_party/ffmpeg-vita\"."
