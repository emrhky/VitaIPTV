#!/usr/bin/env python3
"""Removes the VUI from the SPSs of the H.264 stream in an MPEG-TS file, keeping every byte position: the
shorter SPS is followed by a filler NAL (type 12) of the length saved. ffmpeg cannot mux such a stream
itself (no timestamps without VUI timing). Usage: ts_strip_vui.py in.ts out.ts"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strip_vui import strip
d = bytearray(open(sys.argv[1], "rb").read())
pos = []                                  # file offsets of the video payload bytes, in order
vpid = None
for p in range(0, len(d) - 187, 188):
    if d[p] != 0x47: continue
    pid = ((d[p + 1] & 0x1f) << 8) | d[p + 2]
    afc = (d[p + 3] >> 4) & 3
    o = p + 4
    if afc & 2: o += 1 + d[o]
    if not afc & 1: continue
    pusi = d[p + 1] & 0x40
    if vpid is None and pusi and d[o:o + 4] == b"\x00\x00\x01\xe0": vpid = pid
    if pid != vpid: continue
    if pusi: o += 9 + d[o + 8]            # PES header
    pos.extend(range(o, p + 188))
es = bytes(d[k] for k in pos)
n = 0; i = 0; out = bytearray(es)
while True:
    s = es.find(b"\x00\x00\x01", i)
    if s < 0: break
    e = es.find(b"\x00\x00\x01", s + 3); e = len(es) if e < 0 else e
    while e > s + 3 and es[e - 1] == 0: e -= 1
    if es[s + 3] & 0x1f == 7:
        new = strip(es[s:e])
        k = (e - s) - len(new)
        assert k >= 5, "VUI too short to replace"
        out[s:e] = new + b"\x00\x00\x01\x0c" + b"\xff" * (k - 5) + b"\x80"
        n += 1
    i = e
for k, b in zip(pos, out): d[k] = b
open(sys.argv[2], "wb").write(d)
print("sps stripped", n)
