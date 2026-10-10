#!/usr/bin/env python3
"""Stream analysis (src/probe.c) over a fake HTTP server: TS, HLS, MP4, MKV. Needs gcc, ffmpeg."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="probe_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
SRC = "-f lavfi -i testsrc2=size={s}:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t {t} -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 2"
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', t=4)} -f mpegts {T}/s.ts")
sh(f"ffmpeg -loglevel error -y -i {T}/s.ts -c copy -f hls -hls_time 1 -hls_list_size 0 -hls_segment_filename {T}/seg%03d.ts {T}/media.m3u8")
open(f"{T}/master.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080\nbig.m3u8\n"
                                    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nmedia.m3u8\n")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', t=6)} {T}/film.mp4")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', t=6)} {T}/film.mkv")
open(f"{T}/page.ts", "w").write("<html><body>blocked</body></html>")
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-missing-field-initializers -pthread -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_probe_net.c {ROOT}/src/probe.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/hls.c "
   f"{ROOT}/src/vod.c {ROOT}/src/curlio.c {M}/mock_rt.c -lm -o {T}/test_probe_net")
r = subprocess.run(f"{T}/test_probe_net {T}", shell=True, capture_output=True, text=True, timeout=300)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
