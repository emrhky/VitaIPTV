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
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -f mpegts {T}/s720.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1920x1080', r=25)} -c:v libx264 -profile:v main -g 50 -pix_fmt yuv420p -c:a aac -f mpegts {T}/s1080.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', r=25)} -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac -f mpegts {T}/hevc.ts")
raw = open(f"{T}/s720.ts", "rb").read()
pk = [raw[o:o + 188] for o in range(0, len(raw) - 187, 188)]
open(f"{T}/midgop.ts", "wb").write(b"".join(pk[len(pk) // 3:]))        # starts in the middle of a GOP
vid = [k for k, x in enumerate(pk) if ((x[1] & 0x1F) << 8 | x[2]) == 256 and not (x[1] & 0x40)]
drop = set(random.Random(5).sample(vid, 8))
open(f"{T}/dropped.ts", "wb").write(b"".join(x for k, x in enumerate(pk) if k not in drop))
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_tsplayer.c {ROOT}/src/tsplayer.c {ROOT}/src/tsdemux.c {M}/mock_rt.c -o {T}/test_tsplayer")
r = subprocess.run(f"{T}/test_tsplayer {T}/s720.ts {T}/s1080.ts {T}/hevc.ts {T}/midgop.ts {T}/dropped.ts",
                   shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
