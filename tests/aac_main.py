"""1.2.4l: "AAC Main" test sound on any FFmpeg. FFmpeg 8 (Ubuntu 26.04, the GitHub runners since November 2026)
no longer encodes the AAC Main profile ("Profile not supported"). Then the file is made with AAC LC and every ADTS
header is marked Main (object type 1): an LC frame is a valid Main frame (no prediction data: the same syntax)."""
import os, subprocess

def _run(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)

def make(cmd):
    """cmd: an ffmpeg command line with "-profile:a aac_main", writing MPEG-TS to its last word. None = ok, else
    the CompletedProcess of the failure."""
    r = _run(cmd)
    if r.returncode == 0: return None
    if "aac_main" not in cmd: return r
    out = cmd.split()[-1]
    r = _run(cmd.replace("-profile:a aac_main", ""))
    if r.returncode: return r
    a, v = out + ".main.aac", out + ".lc.ts"
    os.replace(out, v)
    r = _run(f"ffmpeg -loglevel error -y -i {v} -map 0:a -c copy -f adts {a}")
    if r.returncode: return r
    b = bytearray(open(a, "rb").read())
    i = n = 0
    while i + 7 <= len(b) and b[i] == 0xFF and (b[i + 1] & 0xF6) == 0xF0:
        b[i + 2] &= 0x3F                                   # profile 0 = Main
        i += ((b[i + 3] & 3) << 11) | (b[i + 4] << 3) | (b[i + 5] >> 5)
        n += 1
    assert n > 0 and i == len(b), (n, i, len(b))
    open(a, "wb").write(b)
    r = _run(f"ffmpeg -loglevel error -y -i {v} -i {a} -map 0:v -map 1:a -c copy -f mpegts {out}")
    os.remove(a); os.remove(v)
    return r if r.returncode else None
