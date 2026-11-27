#!/usr/bin/env python3
"""1.2.4s: copies a Matroska file and adds index points of the sound (track 2) at every cluster that has none,
as some files have: such a point may be a cluster that does not start with a keyframe.
Usage: mkv_audio_cues.py in.mkv out.mkv"""
import sys

def vint(b, p):
    """EBML variable-size number at p: (value, length, all-ones)"""
    x = b[p]; n = 1; m = 0x80
    while n <= 8 and not (x & m): n += 1; m >>= 1
    v = x & (m - 1); ones = v == m - 1
    for i in range(1, n):
        v = (v << 8) | b[p + i]; ones = ones and b[p + i] == 0xFF
    return v, n, ones

def eid(b, p):
    x = b[p]; n = 1; m = 0x80
    while not (x & m): n += 1; m >>= 1
    return int.from_bytes(b[p:p + n], "big"), n

def head(b, p):
    i, il = eid(b, p)
    s, sl, unk = vint(b, p + il)
    return i, il + sl, s, unk

def enc_size(n):
    for L in range(1, 9):
        if n < (1 << (7 * L)) - 1:
            return ((1 << (7 * L)) | n).to_bytes(L, "big") if L > 1 else bytes([0x80 | n])
    raise ValueError

def el(i, data):
    ib = i.to_bytes((i.bit_length() + 7) // 8, "big")
    return ib + enc_size(len(data)) + data

def uint(i, v):
    return el(i, v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big"))

src = open(sys.argv[1], "rb").read()
b = bytearray(src)
p = 0
i, h, s, _ = head(b, p); p += h + s                      # EBML header
seg_at = p
i, h, seg_size, unk = head(b, p); assert i == 0x18538067
seg_data = p + h
p = seg_data
clusters, cues_at, cues_len = [], None, 0
end = seg_data + seg_size if not unk else len(b)
while p < end:
    i, h, s, u = head(b, p)
    if i == 0x1F43B675:                                   # Cluster: its timecode
        q = p + h
        while True:
            ci, ch, cs, _ = head(b, q)
            if ci == 0xE7: clusters.append((int.from_bytes(b[q + ch:q + ch + cs], "big"), p - seg_data)); break
            q += ch + cs
    elif i == 0x1C53BB6B:
        cues_at, cues_len = p, h + s
    p += h + s
assert cues_at and cues_at + cues_len == end, "the index must be the last element"
# the points there are
i, h, s, _ = head(b, cues_at)
q, qe, have, points = cues_at + h, cues_at + h + s, set(), []
while q < qe:
    pi, ph, ps, _ = head(b, q)
    if pi == 0xBB:
        r, t = q + ph, None
        while r < q + ph + ps:
            ci, ch, cs, _ = head(b, r)
            if ci == 0xB3: t = int.from_bytes(b[r + ch:r + ch + cs], "big")
            r += ch + cs
        have.add(t)
        points.append((t, bytes(b[q:q + ph + ps])))
    q += ph + ps
added = 0
for tc, rel in clusters:
    if tc in have: continue
    points.append((tc, el(0xBB, uint(0xB3, tc) + el(0xB7, uint(0xF7, 2) + uint(0xF1, rel)))))
    added += 1
points.sort(key=lambda x: x[0])
cues = el(0x1C53BB6B, b"".join(x[1] for x in points))
out = b[:cues_at] + cues
if not unk:                                               # the Segment's size, in its own width
    i, il = eid(b, seg_at)
    _, sl, _ = vint(b, seg_at + il)
    ns = len(out) - seg_data
    out[seg_at + il:seg_at + il + sl] = ((1 << (7 * sl)) | ns).to_bytes(sl, "big")
open(sys.argv[2], "wb").write(out)
print(f"{len(clusters)} clusters, {len(have)} video points, {added} sound points added")
