#!/usr/bin/env python3
"""Removes the VUI from every SPS of an Annex B H.264 file (main profile), like the encoders of the 720p
channels the Vita showed no picture for. Usage: strip_vui.py in.h264 out.h264"""
import sys
def unesc(b):
    o = bytearray(); z = 0
    for x in b:
        if z >= 2 and x == 3: z = 0; continue
        o.append(x); z = z + 1 if x == 0 else 0
    return bytes(o)
def esc(b):
    o = bytearray(); z = 0
    for x in b:
        if z >= 2 and x <= 3: o.append(3); z = 0
        o.append(x); z = z + 1 if x == 0 else 0
    return bytes(o)
class R:
    def __init__(s, b): s.b = b; s.p = 0
    def u(s, n):
        v = 0
        for _ in range(n): v = (v << 1) | ((s.b[s.p >> 3] >> (7 - (s.p & 7))) & 1); s.p += 1
        return v
    def ue(s):
        z = 0
        while s.u(1) == 0: z += 1
        return (1 << z) - 1 + s.u(z) if z else 0
def vui_bit(rbsp):
    r = R(rbsp); r.u(8); prof = r.u(8); r.u(16); r.ue()
    assert prof in (66, 77), "main / baseline profile only"
    r.ue(); poc = r.ue()
    if poc == 0: r.ue()
    r.ue(); r.u(1); r.ue(); r.ue()
    if not r.u(1): r.u(1)
    r.u(1)
    if r.u(1): r.ue(); r.ue(); r.ue(); r.ue()
    return r.p
def strip(d):
    st = [k for k in range(len(d) - 3) if d[k] == 0 and d[k + 1] == 0 and d[k + 2] == 1]
    o = bytearray(); i = 0
    for j, s in enumerate(st):
        e = st[j + 1] if j + 1 < len(st) else len(d)
        if j + 1 < len(st) and d[e - 1] == 0: e -= 1
        nal = d[s + 3:e]; o += d[i:s + 3]
        if nal[0] & 0x1f == 7:
            r = unesc(nal); vb = vui_bit(r)
            bits = ''.join(f'{x:08b}' for x in r)[:vb] + '01'
            while len(bits) % 8: bits += '0'
            nb = bytes(int(bits[k:k + 8], 2) for k in range(0, len(bits), 8))
            o += nb[:1] + esc(nb[1:])
        else: o += nal
        i = e
    return bytes(o + d[i:])
if __name__ == "__main__":
    open(sys.argv[2], "wb").write(strip(open(sys.argv[1], "rb").read()))
