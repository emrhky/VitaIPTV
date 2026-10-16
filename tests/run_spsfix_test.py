#!/usr/bin/env python3
"""SPS fix test (PC): an x264 stream gets its VUI removed (like the 720p channels whose SPS has no
bitstream_restriction); tests/test_spsfix adds the restriction back. ffmpeg must decode the same pictures
and see the restriction; a stream that has it stays byte for byte the same. Needs gcc and ffmpeg."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="spsfix_")
def sh(c): subprocess.run(c, shell=True, check=True)
def out(c): return subprocess.run(c, shell=True, check=True, capture_output=True, text=True).stdout + \
                   subprocess.run(c, shell=True, capture_output=True, text=True).stderr

sh(f"gcc -O1 -Wall -Wextra -fsanitize=address,undefined -I{ROOT}/src {ROOT}/tests/test_spsfix.c {ROOT}/src/spsfix.c -o {T}/tsf")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 -t 4 -c:v libx264 -profile:v main "
   f"-bf 2 -refs 2 -g 50 -pix_fmt yuv420p {T}/x.h264")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from strip_vui import strip
open(f"{T}/novui.h264", "wb").write(strip(open(f"{T}/x.h264", "rb").read()))

print(out(f"{T}/tsf {T}/x.h264 {T}/x_fix.h264").strip())
assert open(f"{T}/x.h264", "rb").read() == open(f"{T}/x_fix.h264", "rb").read(), "a stream with the restriction changed"
res = out(f"{T}/tsf {T}/novui.h264 {T}/fix.h264").strip(); print(res)
import re
m = re.match(r"sps (\d+), changed (\d+), reorder (\d+)", res)
assert m and int(m.group(1)) > 0 and m.group(1) == m.group(2), res
tr = out(f"ffmpeg -i {T}/fix.h264 -c:v copy -bsf:v trace_headers -frames:v 1 -f null -")
assert "bitstream_restriction_flag                                  1 = 1" in tr and "max_dec_frame_buffering" in tr
warn = out(f"ffmpeg -v warning -i {T}/fix.h264 -f null -").strip()
assert not warn, warn
a = [l for l in out(f"ffmpeg -loglevel error -i {T}/x.h264 -f framemd5 -").splitlines() if l and l[0] != '#']
b = [l for l in out(f"ffmpeg -loglevel error -i {T}/fix.h264 -f framemd5 -").splitlines() if l and l[0] != '#']
assert len(a) == 100 and a == b, (len(a), len(b))
print(f"same {len(b)} pictures, restriction added")
print("ALL SPSFIX TESTS PASSED")
