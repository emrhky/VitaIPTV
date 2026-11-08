#!/bin/bash
# FFmpeg 7.1 with only the H.265 and H.264 software decoders (and their parsers), with threads and the ARM NEON
# code, for the speed test in this folder: into ../../third_party/ffmpeg-hevc. Vita IPTV itself does not use it.
#
#   export VITASDK=/usr/local/vitasdk
#   ./build_ffmpeg_hevc.sh            (with NEON, about 5 minutes; downloads FFmpeg 7.1 if needed)
#   ASM=0 ./build_ffmpeg_hevc.sh      (without the assembler code, only if the first one does not build)
set -e
: "${VITASDK:?VITASDK is not set (export VITASDK=/path/to/vitasdk)}"
export PATH="$VITASDK/bin:$PATH"
command -v arm-vita-eabi-gcc >/dev/null || { echo "arm-vita-eabi-gcc not found in $VITASDK/bin"; exit 1; }

VER=7.1
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/third_party/ffmpeg-hevc"
WORK="$ROOT/build-ffmpeg-hevc"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "ffmpeg-$VER" ]; then
    if [ -f "$ROOT/build-ffmpeg/ffmpeg-$VER.tar.xz" ]; then cp "$ROOT/build-ffmpeg/ffmpeg-$VER.tar.xz" .; fi
    [ -f "ffmpeg-$VER.tar.xz" ] || curl -fL -o "ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
    tar xf "ffmpeg-$VER.tar.xz"
fi
cd "ffmpeg-$VER"

LENIENT="-Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration"
if [ "${ASM:-1}" = "1" ]; then ASMFLAGS="--enable-neon --enable-vfp"; else ASMFLAGS="--disable-asm"; fi

make distclean >/dev/null 2>&1 || true
./configure \
    --prefix="$OUT" \
    --enable-cross-compile --cross-prefix=arm-vita-eabi- --arch=arm --cpu=cortex-a9 --target-os=none \
    --disable-everything --enable-decoder=hevc,h264 --enable-parser=hevc,h264 \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-swscale --disable-postproc --disable-avfilter --disable-swresample \
    --enable-pthreads --extra-libs=-lpthread --disable-runtime-cpudetect \
    --disable-debug --enable-static --disable-shared \
    --extra-cflags="-O3 -ffunction-sections -fdata-sections $LENIENT" \
    $ASMFLAGS | tee "$WORK/configure.log" | grep -E "^(threading|ARMv|NEON|VFP|debug|optimize)|^install prefix" || true

grep -q "^threading support *pthreads" "$WORK/configure.log" || { echo "FFmpeg found no pthreads: see $WORK/configure.log"; exit 1; }
make -j"$(nproc)"
rm -rf "$OUT"
make install
cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
echo
echo "Done: $OUT"
ls -l "$OUT/lib"
