#!/usr/bin/env python3
"""Stream analysis (src/probe.c) over a fake HTTP server: TS, HLS, MP4, MKV, FLV. Needs gcc, ffmpeg."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="probe_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
import aac_main                                            # 1.2.4l: FFmpeg 8 has no AAC Main encoder
def sh_main(c):
    if os.environ.get("TSP_DIR") and os.path.exists(c.split()[-1]): return None
    r = aac_main.make(c)
    if r: print(r.stdout, r.stderr[-3000:]); sys.exit(1)

SRC = "-f lavfi -i testsrc2=size={s}:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t {t} -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 2"
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', t=4)} -f mpegts {T}/s.ts")
sh(f"ffmpeg -loglevel error -y -i {T}/s.ts -c copy -f hls -hls_time 1 -hls_list_size 0 -hls_segment_filename {T}/seg%03d.ts {T}/media.m3u8")
open(f"{T}/master.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080\nbig.m3u8\n"
                                    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nmedia.m3u8\n")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', t=6)} {T}/film.mp4")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', t=6)} {T}/film.mkv")
sh(f"ffmpeg -loglevel error -y -i {T}/s.ts -c copy -f flv {T}/live.flv")
sh_main(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=50 -f lavfi -i sine=frequency=440:sample_rate=24000 -t 3 "
   f"-c:v libx264 -preset ultrafast -bf 2 -x264-params b-pyramid=0 -g 50 -pix_fmt yuv420p -c:a aac -profile:a aac_main -ac 2 -f mpegts {T}/fast1080.ts")
# 1.2.4k: MPEG-4 Part 2 + MP3 in MKV (the device's film): decoded in software, not "not supported yet"
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=528x400:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 "
   f"-c:v mpeg4 -q:v 4 -c:a libmp3lame -b:a 128k {T}/xvid.mkv")
# 1.2.4m: the same in AVI, and a Video CD file (MPEG-1 + MP2 in raw CD sectors)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x352:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 "
   f"-c:v mpeg4 -vtag XVID -q:v 4 -c:a libmp3lame -b:a 128k {T}/xvid.avi")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=352x288:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4 "
   f"-target pal-vcd {T}/vcd.mpg")
import cdxa
open(f"{T}/vcd.dat", "wb").write(cdxa.wrap(open(f"{T}/vcd.mpg", "rb").read()))
open(f"{T}/page.ts", "w").write("<html><body>blocked</body></html>")
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-missing-field-initializers -pthread -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_probe_net.c {ROOT}/src/probe.c {ROOT}/src/spsfix.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c {ROOT}/src/avidemux.c {ROOT}/src/psdemux.c {ROOT}/src/hls.c "
   f"{ROOT}/src/vod.c {ROOT}/src/curlio.c {M}/mock_rt.c -lm -o {T}/test_probe_net")
r = subprocess.run(f"{T}/test_probe_net {T}", shell=True, capture_output=True, text=True, timeout=300)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
