#!/bin/bash
# The test files (10 s each, 25 fps, with grain so they are not too easy): run on a PC with ffmpeg (libx265, libx264).
# Copy the results to ux0:data/hevcbench/ on the Vita.
set -e
OUT="${1:-hevcbench_files}"
mkdir -p "$OUT"
SRC="-f lavfi -i testsrc2=size=1920x1080:rate=25,noise=alls=12:allf=t+u -t 10"
enc() { ffmpeg -loglevel error -y $SRC -vf scale=$1 -c:v libx265 -preset medium -b:v $2 -x265-params log-level=error:keyint=50 $3 -an -f hevc "$OUT/$4"; }
enc 640:360  600k  "-pix_fmt yuv420p"     hevc_360p.hevc
enc 854:480  1000k "-pix_fmt yuv420p"     hevc_480p.hevc
enc 1280:720 2000k "-pix_fmt yuv420p"     hevc_720p.hevc
enc 1920:1080 4000k "-pix_fmt yuv420p"    hevc_1080p.hevc
enc 1920:1080 4000k "-pix_fmt yuv420p10le" hevc_1080p_10bit.hevc
ffmpeg -loglevel error -y $SRC -vf scale=854:480 -c:v libx264 -preset medium -b:v 1200k -g 50 -pix_fmt yuv420p -an -f h264 "$OUT/h264_480p.h264"
ffmpeg -loglevel error -y $SRC -vf scale=1280:720 -c:v libx264 -preset medium -b:v 2500k -g 50 -pix_fmt yuv420p -an -f h264 "$OUT/h264_720p.h264"
ls -l "$OUT"
