#!/usr/bin/env python3
"""Integration test for src/tsplayer.c with a fake decoder (needs gcc, ffmpeg)."""
import os, random, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="tsp_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r
SRC = "-f lavfi -i testsrc2=size={s}:rate={r} -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5"
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/s720.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1920x1080', r=25)} -c:v libx264 -profile:v main -g 50 -pix_fmt yuv420p -c:a aac -f mpegts {T}/s1080.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', r=25)} -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac -f mpegts {T}/hevc.ts")
raw = open(f"{T}/s720.ts", "rb").read()
pk = [raw[o:o + 188] for o in range(0, len(raw) - 187, 188)]
open(f"{T}/midgop.ts", "wb").write(b"".join(pk[len(pk) // 3:]))        # starts in the middle of a GOP
vid = [k for k, x in enumerate(pk) if ((x[1] & 0x1F) << 8 | x[2]) == 256 and not (x[1] & 0x40)]
drop = set(random.Random(5).sample(vid, 8))
open(f"{T}/dropped.ts", "wb").write(b"".join(x for k, x in enumerate(pk) if k not in drop))
# I-pictures that are not IDR and carry no recovery SEI: every IDR after the first is
# relabelled as a plain slice (NAL type 5 -> 1), SEI removed, then cut mid-GOP.
# (The relabelled stream is only for the demuxer/player logic; a real decoder would object.)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -t 8 -c:v libx264 -bf 0 "
   f"-x264-params keyint=50:min-keyint=50:scenecut=0 -pix_fmt yuv420p -f h264 {T}/og.h264")
es = open(f"{T}/og.h264", "rb").read()
starts = [i for i in range(len(es) - 3) if es[i:i + 3] == b"\0\0\1"]
bounds = starts + [len(es)]
keep, seen_idr = bytearray(), 0
for a, b in zip(bounds, bounds[1:]):
    t = es[a + 3] & 0x1F if a + 3 < len(es) else 0
    if t == 6: continue
    nal = bytearray(es[a:b])
    if t == 5:
        seen_idr += 1
        if seen_idr > 1: nal[3] = (nal[3] & 0xE0) | 1
    keep += nal
open(f"{T}/og_nosei.h264", "wb").write(bytes(keep))
sh(f"ffmpeg -loglevel error -y -fflags +genpts -r 25 -f h264 -i {T}/og_nosei.h264 -c copy -f mpegts {T}/og.ts")
info = sh(f"ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 {T}/og.ts").stdout.split()
print("open-GOP test stream:", len(info), "pictures")
ograw = open(f"{T}/og.ts", "rb").read()
ogpk = [ograw[o:o + 188] for o in range(0, len(ograw) - 187, 188)]
open(f"{T}/opengop_mid.ts", "wb").write(b"".join(ogpk[len(ogpk) // 3:]))
sh(f"ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 -c:a aac -f mpegts {T}/audio.ts")
sc = bytearray(raw)
for o in range(0, len(sc) - 187, 188):
    if sc[o] == 0x47 and ((sc[o + 1] & 0x1F) << 8 | sc[o + 2]) == 256: sc[o + 3] |= 0xC0
open(f"{T}/scrambled.ts", "wb").write(bytes(sc))
# audio packets moved later / earlier in the file than their timestamps
def shift_audio(src, dst, seconds):
    d = open(src, "rb").read()
    p = [d[o:o + 188] for o in range(0, len(d) - 187, 188)]
    per_s = len(p) / 5.0
    keyed = []
    for k, x in enumerate(p):
        pid = (x[1] & 0x1F) << 8 | x[2]
        # never in front of the PAT/PMT at the start, or the demuxer cannot know the stream yet
        keyed.append((max(3, k + seconds * per_s) if pid == 257 else k, k, x))
    keyed.sort(key=lambda e: (e[0], e[1]))
    open(dst, "wb").write(b"".join(x for _, _, x in keyed))
shift_audio(f"{T}/s720.ts", f"{T}/alate.ts", 1.5)
shift_audio(f"{T}/s720.ts", f"{T}/aearly.ts", -1.5)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 5 "
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a aac -ac 1 -f mpegts {T}/mono441.ts")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5 "
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a ac3 -f mpegts {T}/ac3.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2 -f matroska {T}/bframes.mkv")
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -DSTALL_US=1500000ULL -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_tsplayer.c {ROOT}/src/tsplayer.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {M}/mock_rt.c -lm -o {T}/test_tsplayer")
r = subprocess.run(f"{T}/test_tsplayer {T}/s720.ts {T}/s1080.ts {T}/hevc.ts {T}/midgop.ts {T}/dropped.ts "
                   f"{T}/opengop_mid.ts {T}/audio.ts {T}/scrambled.ts {T}/alate.ts {T}/aearly.ts {T}/mono441.ts {T}/ac3.ts {T}/bframes.mkv {T}/s1080.ts",
                   shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
