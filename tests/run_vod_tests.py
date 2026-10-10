#!/usr/bin/env python3
"""Tests src/vod.c (films and episodes: MP4 / MKV / TS files, length and seeking). Needs gcc, ffmpeg."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="vod_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r
LEN = 60
SRC = (f"-f lavfi -i testsrc2=size=320x240:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t {LEN} "
       "-c:v libx264 -profile:v high -bf 2 -g 50 -keyint_min 50 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -ac 2")
sh(f"ffmpeg -loglevel error -y {SRC} {T}/end.mp4")
sh(f"ffmpeg -loglevel error -y {SRC} -movflags +faststart {T}/start.mp4")
sh(f"ffmpeg -loglevel error -y {SRC} -cluster_time_limit 1000 {T}/film.mkv")
sh(f"ffmpeg -loglevel error -y {SRC} -f mpegts {T}/film.ts")
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -I{ROOT}/src {ROOT}/tests/test_vod.c {ROOT}/src/vod.c "
   f"{ROOT}/src/mkvdemux.c {ROOT}/src/tsdemux.c -o {T}/test_vod")
r = subprocess.run(f"{T}/test_vod {LEN * 1000} {T}/end.mp4 {T}/start.mp4 {T}/film.mkv {T}/film.ts", shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
