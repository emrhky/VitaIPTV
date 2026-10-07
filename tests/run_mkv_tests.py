#!/usr/bin/env python3
"""MKV demuxer tests (gcc, ffmpeg, ffprobe needed)."""
import os, struct, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="mkv_")
fails = []
def sh(c, check=True):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if check and r.returncode: print("FAILED:", c, r.stderr[-1500:]); sys.exit(2)
    return r
def check(cond, msg):
    if not cond: fails.append(msg); print("  FAIL:", msg)
san = "-O1 -g -fsanitize=address,undefined -Wall -Wextra"
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/tsinfo.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c -o {T}/tsinfo")
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/fuzz_mkv.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c -o {T}/fuzz")
def info(path, *a):
    d = {}
    for l in sh(f"{T}/tsinfo {path} {' '.join(a)}").stdout.splitlines():
        k, _, v = l.partition("="); d[k] = v
    return d
SRC = "-f lavfi -i testsrc2=size={s}:rate={r} -f lavfi -i sine=frequency=440:sample_rate={sr} -t 5"
gen = {
 "bframes": f"{SRC.format(s='1280x720', r=30, sr=48000)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2",
 "live":    f"{SRC.format(s='1280x720', r=25, sr=44100)} -c:v libx264 -g 50 -pix_fmt yuv420p -c:a aac -ac 1 -live 1",
 "small":   f"{SRC.format(s='640x360', r=25, sr=48000)} -c:v libx264 -profile:v baseline -g 25 -pix_fmt yuv420p -c:a aac -ac 2 -cluster_time_limit 200",
 "hevc":    f"{SRC.format(s='640x360', r=25, sr=48000)} -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac",
 "mp3":     f"{SRC.format(s='640x360', r=25, sr=48000)} -c:v libx264 -pix_fmt yuv420p -c:a libmp3lame",
}
for n, args in gen.items():
    sh(f"ffmpeg -loglevel error -y {args} -f matroska pipe:1 > {T}/{n}.mkv")

print("== real files vs ffprobe")
for n in ("bframes", "live", "small"):
    p = f"{T}/{n}.mkv"
    i = info(p, f"-o {T}/{n}")
    pk = lambda sel: int(sh(f"ffprobe -v error -count_packets -select_streams {sel} -show_entries stream=nb_read_packets -of csv=p=0 {p}").stdout.split()[0].strip(','))
    check(i["container"] == "mkv" and i["video_codec"] == "h264" and i["audio_codec"] == "aac", f"{n}: codecs {i['video_codec']}/{i['audio_codec']}")
    check(int(i["video_aus"]) == pk("v:0"), f"{n}: {i['video_aus']} pictures vs {pk('v:0')}")
    check(int(i["audio_frames"]) == pk("a:0"), f"{n}: {i['audio_frames']} audio frames vs {pk('a:0')}")
    for ch in ("1", "187", "4096", "0"):
        o = info(p, f"-chunk {ch}")
        check(all(o[k] == i[k] for k in i), f"{n}: chunk {ch} differs")
    dec = sh(f"ffmpeg -v error -f h264 -i {T}/{n}.video -f null -", check=False)
    check(dec.returncode == 0 and not dec.stderr.strip(), f"{n}: video does not decode cleanly: {dec.stderr[:200]}")
    nf = int(sh(f"ffprobe -v error -f h264 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 {T}/{n}.video").stdout.split()[0].strip(','))
    check(nf == int(i["video_aus"]), f"{n}: decoded {nf} pictures, expected {i['video_aus']}")
    dec = sh(f"ffmpeg -v error -i {T}/{n}.audio -f null -", check=False)
    check(dec.returncode == 0 and not dec.stderr.strip(), f"{n}: audio does not decode cleanly")
    print(f"  {n}: {i['width']}x{i['height']} {i['fps']} fps, {i['video_aus']} pictures, {i['keyframes']} keyframes, "
          f"{i['audio_frames']} AAC frames {i['aac_rate']} Hz {i['aac_channels']} ch")
i = info(f"{T}/hevc.mkv"); check(i["video_codec"] == "hevc", "hevc not recognised"); print("  hevc: video", i["video_codec"])
i = info(f"{T}/mp3.mkv"); check(i["audio_codec"] == "mpeg_audio", "mp3 not recognised"); print("  mp3: audio", i["audio_codec"])

print("== laced audio blocks (Xiph and EBML lacing), written by hand")
sh(f"ffmpeg -loglevel error -y -f lavfi -i sine=frequency=330:sample_rate=48000 -t 4 -c:a aac -ac 2 -f adts {T}/s.aac")
adts = open(f"{T}/s.aac", "rb").read()
frames, pos = [], 0
while pos + 7 <= len(adts):
    fl = ((adts[pos+3] & 3) << 11) | (adts[pos+4] << 3) | (adts[pos+5] >> 5)
    hl = 7 if adts[pos+1] & 1 else 9
    frames.append(adts[pos+hl:pos+fl]); pos += fl
prof, sfi, chn = (adts[2] >> 6) + 1, (adts[2] >> 2) & 15, ((adts[2] & 1) << 2) | (adts[3] >> 6)
asc = bytes([(prof << 3) | (sfi >> 1), ((sfi & 1) << 7) | (chn << 3)])
def vint(n):  # 8-byte size
    return bytes([0x01]) + n.to_bytes(7, "big")
def el(idb, payload): return idb + vint(len(payload)) + payload
def uint(idb, v): return el(idb, v.to_bytes(4, "big"))
def lace_xiph(fr):
    h = bytes([len(fr) - 1])
    for f in fr[:-1]:
        s = len(f); h += b"\xff" * (s // 255) + bytes([s % 255])
    return 0x02, h
def lace_ebml(fr):
    h = bytes([len(fr) - 1]) + bytes([0x40 | (len(fr[0]) >> 8), len(fr[0]) & 0xFF])
    for k in range(1, len(fr) - 1):
        d = len(fr[k]) - len(fr[k-1]) + 8191
        h += bytes([0x40 | (d >> 8), d & 0xFF])
    return 0x06, h
tracks = el(b"\x16\x54\xAE\x6B", el(b"\xAE", uint(b"\xD7", 1) + uint(b"\x83", 2) + el(b"\x86", b"A_AAC") + el(b"\x63\xA2", asc)
          + el(b"\xE1", el(b"\xB5", struct.pack(">d", 48000.0)) + uint(b"\x9F", chn))))
info_el = el(b"\x15\x49\xA9\x66", uint(b"\x2A\xD7\xB1", 1000000))
clusters, k, ms_per = b"", 0, 1024 * 1000 / 48000
blocks = 0
while k < len(frames):
    group = frames[k:k+4]
    tc = int(k * ms_per)
    flag, head = (lace_xiph if blocks % 2 == 0 else lace_ebml)(group) if len(group) > 1 else (0, b"")
    blk = bytes([0x81]) + (0).to_bytes(2, "big", signed=True) + bytes([0x80 | flag]) + head + b"".join(group)
    clusters += b"\x1F\x43\xB6\x75" + bytes([0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]) + uint(b"\xE7", tc) + el(b"\xA3", blk)
    k += 4; blocks += 1
mkv = el(b"\x1A\x45\xDF\xA3", el(b"\x42\x82", b"matroska")) + b"\x18\x53\x80\x67" + bytes([0x01] + [0xFF]*7) + info_el + tracks + clusters
open(f"{T}/laced.mkv", "wb").write(mkv)
i = info(f"{T}/laced.mkv", f"-o {T}/laced")
check(int(i["audio_frames"]) == len(frames), f"laced: {i['audio_frames']} frames, expected {len(frames)}")
dec = sh(f"ffmpeg -v error -i {T}/laced.audio -f null -", check=False)
check(dec.returncode == 0 and not dec.stderr.strip(), "laced: output does not decode")
check(open(f"{T}/laced.audio", "rb").read() != b"", "laced: no output")
print(f"  {blocks} laced blocks (Xiph/EBML alternating), {i['audio_frames']} of {len(frames)} frames recovered, {i['aac_rate']} Hz {i['aac_channels']} ch")

print("== fuzz")
r = sh(f"{T}/fuzz 120 {T}/bframes.mkv {T}/live.mkv {T}/laced.mkv {T}/hevc.mkv", check=False)
print(r.stdout.strip())
check(r.returncode == 0 and "ERROR" not in r.stderr, "fuzz: " + r.stderr[-800:])
print("FAILURES: %d" % len(fails) if fails else "ALL MKV TESTS PASSED")
sys.exit(1 if fails else 0)
