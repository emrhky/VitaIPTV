#!/bin/bash
# Builds FFmpeg's AC-3, E-AC-3 and AAC decoders (nothing else) for the PS Vita into third_party/ffmpeg-vita.
# CMakeLists.txt finds them there and the app plays Dolby Digital / Dolby Digital Plus audio (0.5.0) and
# 5.1 / 7.1 AAC, which the Vita's own AAC decoder refuses (0.6.0; the stereo AAC stays on the hardware).
# Without this step the app builds as before, without them. An older build (0.5.0, no AAC): run it again.
#
#   export VITASDK=/home/acer/vitasdk          (as for building the app)
#   ./tools/build_ffmpeg_vita.sh               (downloads FFmpeg 7.1, about 5 minutes)
#   ASM=0 THREADS=0 ./tools/build_ffmpeg_vita.sh   (1.2.3: without NEON code / threads; both are on by default)
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

# 1.2.3: ARM NEON code (IDCT, motion compensation: the MPEG-2 / MPEG-4 video decoded on the ARM) and threads
# (MPEG-2 slices / MPEG-4 pictures on more cores) are on; if this FFmpeg does not build so with this VitaSDK, it
# is built again without them, one at a time (the app works either way, only slower). ASM=0 / THREADS=0 leave
# them out. What was used goes to $OUT/build_flags.txt (CMake shows it; the app writes it in its log).
try_build() {   # $1 asm (1/0), $2 threads (1/0)
    local extra="" cflags="-O2 -ffunction-sections -fdata-sections $LENIENT"
    if [ "$1" = "1" ]; then extra="--enable-neon"; cflags="$cflags -mfpu=neon"; else extra="--disable-asm"; fi
    if [ "$2" = "1" ]; then extra="$extra --enable-pthreads"; else extra="$extra --disable-pthreads"; fi
    make distclean >/dev/null 2>&1 || true
    ./configure \
        --prefix="$OUT" \
        --enable-cross-compile --cross-prefix=arm-vita-eabi- --arch=arm --cpu=cortex-a9 --target-os=none \
        --disable-everything --enable-decoder=ac3,eac3,aac,opus,vorbis,dca,mpeg1video,mpeg2video,mpeg4,msmpeg4v3 --enable-parser=ac3,dca,mpegvideo,mpeg4video \
        --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-avdevice --disable-avformat --disable-swscale --disable-postproc --disable-avfilter --enable-swresample \
        --disable-w32threads --disable-os2threads --disable-runtime-cpudetect \
        --disable-debug --enable-small --enable-static --disable-shared \
        --extra-cflags="$cflags" --extra-libs="-lpthread" \
        $extra || return 1
    if [ "$1" = "1" ] && ! grep -q "^#define HAVE_NEON 1" config.h; then echo "NEON was not enabled by configure"; return 1; fi
    if [ "$2" = "1" ] && ! grep -q "^#define HAVE_PTHREADS 1" config.h; then echo "pthreads were not enabled by configure"; return 1; fi
    make -j"$(nproc)" || return 1
    USED="neon=$1 threads=$2"
}

USED=""
A="${ASM:-1}"; TH="${THREADS:-1}"
for combo in "$A $TH" "0 $TH" "$A 0" "0 0"; do
    set -- $combo
    echo "=== FFmpeg: NEON $1, threads $2 ==="
    if try_build "$1" "$2"; then break; fi
    echo "=== that did not build: trying the next way ==="
done
[ -n "$USED" ] || { echo "FFmpeg did not build"; exit 1; }
rm -rf "$OUT"
make install
cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
echo "$USED" > "$OUT/build_flags.txt"
echo
echo "Done: $OUT"
ls -l "$OUT/lib"
echo "Now build the app again (rm -rf build; mkdir build; cd build; cmake ..; make): CMake says"
echo "\"AC-3 / E-AC-3: FFmpeg from third_party/ffmpeg-vita\"."
