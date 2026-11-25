"""1.2.4l: a Video CD .dat file from an MPEG program stream whose packs are 2324 bytes (ffmpeg -f vcd): RIFF CDXA,
raw Mode 2 Form 2 sectors (sync, header, subheader, 2324 bytes, EDC). Usage: cdxa.py in.mpg out.dat"""
import struct, sys

def bcd(v): return ((v // 10) << 4) | (v % 10)

def wrap(ps, extra_zero_sectors=2):
    out = bytearray()
    lba = 150 + 225                                      # (a track starting a few seconds in)
    packs = [ps[i:i + 2324] for i in range(0, len(ps), 2324)]
    sectors = [b"\0" * 2324] * extra_zero_sectors + packs   # (VCD tracks start with empty sectors)
    for k, d in enumerate(sectors):
        d = d.ljust(2324, b"\0")
        a = lba + k
        hdr = bytes([0] + [0xFF] * 10 + [0, bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75), 2])
        sub = bytes([1, 1, 0x62 if k >= extra_zero_sectors else 0x20, 0x0F]) * 2   # form 2 (+ video / real-time)
        out += hdr + sub + d + b"\0\0\0\0"
    fmt = struct.pack("<IHH4sB7s", 0, 0, 0x3111, b"XA\x01\x00", 1, b"\0" * 7)   # (the XA fmt chunk: 16 bytes)
    body = b"CDXA" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(out)) + bytes(out)
    return b"RIFF" + struct.pack("<I", len(body)) + body

if __name__ == "__main__":
    open(sys.argv[2], "wb").write(wrap(open(sys.argv[1], "rb").read()))
