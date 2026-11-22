#!/bin/bash
# 1.2.2: the same FFmpeg decoders as tools/build_ffmpeg_vita.sh, built for the PC tests (tests/run_all.py with
# FFMPEG_PREFIX=<prefix>). Usage: tools/build_ffmpeg_pc.sh [prefix]   (default /tmp/ffpc)
set -e
OUT="${1:-/tmp/ffpc}"
VER=6.1.1
WORK="$(mktemp -d)"
cd "$WORK"
curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz" -o ff.tar.xz
tar xf ff.tar.xz
cd "ffmpeg-$VER"
./configure --prefix="$OUT" --disable-everything --enable-decoder=ac3,eac3,aac,opus,vorbis,dca,mpeg1video,mpeg2video,mpeg4,msmpeg4v3 --enable-parser=ac3,dca,mpegvideo,mpeg4video \
    --disable-programs --disable-doc --disable-network --disable-autodetect --disable-avdevice --disable-avformat \
    --disable-swscale --disable-postproc --disable-avfilter --enable-swresample --disable-pthreads --disable-debug \
    --enable-small --enable-static --disable-shared --disable-asm --extra-cflags=-O1
make -j"$(nproc)"
make install
rm -rf "$WORK"
