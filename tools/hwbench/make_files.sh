#!/bin/sh
# Test files for the hardware test (H.264 like IPTV channels send it: High profile, B-pictures, AUD).
# Needs ffmpeg with libx264. Output: ./hwbench_files/*.h264 -> copy to ux0:data/hwbench/ on the Vita.
set -e
OUT=${1:-hwbench_files}
mkdir -p "$OUT"
mk() { # name w h fps bitrate x264-extra
  ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=$2x$3:rate=$4,noise=alls=10:allf=t" -t 8 \
    -c:v libx264 -preset medium -profile:v high -pix_fmt yuv420p -b:v $5 -maxrate $5 -bufsize $5 \
    -x264-params "aud=1:keyint=$(( $4 * 2 )):min-keyint=$4:scenecut=0$6" -f h264 "$OUT/$1.h264"
}
mk 720p50       1280 720  50 5M   ":ref=4:bframes=3"
mk 720p60       1280 720  60 5M   ":ref=4:bframes=3"
mk 1080p25      1920 1080 25 6M   ":ref=4:bframes=3"
mk 1080p50      1920 1080 50 8M   ":ref=4:bframes=3"
mk 1080p50_b2   1920 1080 50 8M   ":ref=2:bframes=2:b-pyramid=none:b-adapt=0"
ls -l "$OUT"
