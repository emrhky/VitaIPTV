#!/usr/bin/env python3
"""TS demuxer tests: builds the tools, generates streams with ffmpeg, checks the
demuxer against ffprobe, then runs damage tests and a sanitizer fuzz run.
Usage: python3 tests/run_ts_tests.py   (needs gcc, ffmpeg, ffprobe)"""
import json, os, random, subprocess, sys, tempfile, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="tstest_")
fails = []

def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        print("COMMAND FAILED:", cmd, r.stderr[-800:]); sys.exit(2)
    return r

def check(cond, msg):
    if not cond:
        fails.append(msg); print("  FAIL:", msg)

san = "-O1 -g -fsanitize=address,undefined -Wall -Wextra"
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/tsinfo.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c -o {TMP}/tsinfo")
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/fuzz_ts.c {ROOT}/src/tsdemux.c -o {TMP}/fuzz_ts")

def tsinfo(path, *args):
    r = sh(f"{TMP}/tsinfo {path} {' '.join(args)}")
    d = {}
    streams = []
    for line in r.stdout.splitlines():
        k, _, v = line.partition("=")
        if k == "stream": streams.append(v)
        else: d[k] = v
    d["streams"] = streams
    return d

SRC = "-f lavfi -i testsrc2=size={w}x{h}:rate={fps} -f lavfi -i sine=frequency=440:sample_rate={sr}"
def gen(name, w, h, fps, sr, venc, aenc, extra_v="", extra_a="-ac 2", fmt="mpegts", video=True, audio=True, t=5):
    out = f"{TMP}/{name}.ts"
    ins = SRC.format(w=w, h=h, fps=fps, sr=sr)
    maps = ""
    if not video: ins = f"-f lavfi -i sine=frequency=440:sample_rate={sr}"
    v = f"-c:v {venc} {extra_v}" if video else "-vn"
    a = f"-c:a {aenc} {extra_a}" if audio else "-an"
    if video and not audio: maps = "-map 0:v"
    sh(f"ffmpeg -hide_banner -loglevel error -y {ins} -t {t} {v} {a} {maps} -f {fmt} {out}")
    return out

streams = {
 "s360":   dict(w=640, h=360, fps=25, sr=44100, venc="libx264", aenc="aac", extra_v="-profile:v main -level 3.1 -pix_fmt yuv420p -g 50"),
 "s720":   dict(w=1280, h=720, fps=30, sr=48000, venc="libx264", aenc="aac", extra_v="-profile:v high -bf 3 -pix_fmt yuv420p -g 60"),
 "s1080":  dict(w=1920, h=1080, fps=25, sr=48000, venc="libx264", aenc="aac", extra_v="-profile:v high -pix_fmt yuv420p -g 50 -b:v 3M"),
 "sbase":  dict(w=320, h=240, fps=15, sr=8000, venc="libx264", aenc="aac", extra_v="-profile:v baseline -pix_fmt yuv420p -g 30", extra_a="-ac 1"),
 "sinter": dict(w=720, h=576, fps=25, sr=48000, venc="libx264", aenc="aac", extra_v="-profile:v high -pix_fmt yuv420p -flags +ildct+ilme -x264-params tff=1 -g 50"),
 "shi10":  dict(w=640, h=360, fps=25, sr=48000, venc="libx264", aenc="aac", extra_v="-profile:v high10 -pix_fmt yuv420p10le -g 50"),
 "shevc":  dict(w=640, h=360, fps=25, sr=48000, venc="libx265", aenc="aac", extra_v="-pix_fmt yuv420p -x265-params log-level=error"),
 "smpeg2": dict(w=640, h=360, fps=25, sr=48000, venc="mpeg2video", aenc="mp2", extra_v="-b:v 2M -g 12"),
 "sac3":   dict(w=640, h=360, fps=25, sr=48000, venc="libx264", aenc="ac3", extra_v="-profile:v main -pix_fmt yuv420p -g 50"),
 "saudio": dict(w=0, h=0, fps=0, sr=44100, venc="", aenc="aac", video=False),
 "svideo": dict(w=640, h=360, fps=25, sr=44100, venc="libx264", aenc="aac", extra_v="-pix_fmt yuv420p -g 50", audio=False),
}
files = {n: gen(n, **a) for n, a in streams.items()}

PROFILE = {"Baseline": 66, "Constrained Baseline": 66, "Main": 77, "High": 100, "High 10": 110}
def probe(path):
    r = sh(f"ffprobe -v error -show_streams -of json {path}")
    return json.loads(r.stdout)["streams"]

def count(path, sel, field):
    r = sh(f"ffprobe -v error -count_packets -select_streams {sel} -show_entries stream={field} -of csv=p=0 {path}")
    return int(r.stdout.split()[0].split(",")[0])

print("== per-stream checks against ffprobe")
for name, path in files.items():
    print(name)
    info = tsinfo(path, f"-o {TMP}/{name}")
    # same result no matter how the data is chunked
    for ch in ("0", "1", "187", "189", "5000"):
        other = tsinfo(path, f"-chunk {ch}")
        diffs = [k for k in info if info[k] != other.get(k)]
        check(not diffs, f"{name}: chunk={ch} differs in {diffs}")
    st = probe(path)
    v = next((s for s in st if s["codec_type"] == "video"), None)
    a = next((s for s in st if s["codec_type"] == "audio"), None)
    if v:
        want = {"h264": "h264", "hevc": "hevc", "mpeg2video": "mpeg2video"}[v["codec_name"]]
        check(info["video_codec"] == want, f"{name}: video codec {info['video_codec']} != {want}")
        n_pk = count(path, "v:0", "nb_read_packets")
        check(int(info["video_aus"]) == n_pk, f"{name}: video AUs {info['video_aus']} != ffprobe packets {n_pk}")
        if want == "h264":
            check(int(info["width"]) == v["width"] and int(info["height"]) == v["height"],
                  f"{name}: size {info['width']}x{info['height']} != {v['width']}x{v['height']}")
            check(int(info["profile"]) == PROFILE.get(v["profile"], -1), f"{name}: profile {info['profile']} vs {v['profile']}")
            check(int(info["level"]) == int(v["level"]), f"{name}: level {info['level']} vs {v['level']}")
            check(int(info["bit_depth"]) == int(v.get("bits_per_raw_sample", 8)), f"{name}: bit depth {info['bit_depth']} vs {v.get('bits_per_raw_sample')}")
            check(int(info["sps_len"]) > 0 and int(info["pps_len"]) > 0, f"{name}: SPS/PPS missing")
            num, den = map(int, v["avg_frame_rate"].split("/"))
            check(abs(float(info["fps"]) - num / den) < 0.05 * num / den, f"{name}: fps {info['fps']} vs {num}/{den}")
            # keyframes
            kf = sh(f"ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 {path}").stdout.split()
            nk = sum(1 for x in kf if x.startswith("K"))
            check(int(info["keyframes"]) == nk, f"{name}: keyframes {info['keyframes']} != ffprobe {nk}")
            # first pts
            fp = sh(f"ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 {path}").stdout.split()
            check(int(info["first_video_pts"]) == int(fp[0].rstrip(",")), f"{name}: first pts {info['first_video_pts']} vs {fp[0]}")
            # the extracted stream must decode cleanly and give the same number of frames
            dec = sh(f"ffmpeg -v error -i {TMP}/{name}.video -f null -", check=False)
            check(dec.stderr.strip() == "", f"{name}: ffmpeg decode errors: {dec.stderr[:200]}")
            nf = count(f"{TMP}/{name}.video", "v:0", "nb_read_packets")
            check(nf == int(info["video_aus"]), f"{name}: extracted h264 has {nf} frames, expected {info['video_aus']}")
    else:
        check(info["video_codec"] == "none", f"{name}: expected no video")
    if a:
        check(a["codec_name"] in ("aac", "mp2", "ac3"), f"unexpected audio {a['codec_name']}")
        if a["codec_name"] == "aac":
            check(info["audio_codec"] == "aac", f"{name}: audio codec {info['audio_codec']}")
            check(int(info["aac_rate"]) == int(a["sample_rate"]) and int(info["aac_channels"]) == a["channels"],
                  f"{name}: aac {info['aac_rate']}Hz/{info['aac_channels']}ch vs {a['sample_rate']}/{a['channels']}")
            nf = count(f"{TMP}/{name}.audio", "a:0", "nb_read_packets")
            check(nf == int(info["audio_frames"]), f"{name}: ADTS frames {info['audio_frames']} vs ffprobe {nf}")
            dec = sh(f"ffmpeg -v error -i {TMP}/{name}.audio -f null -", check=False)
            check(dec.stderr.strip() == "", f"{name}: aac decode errors: {dec.stderr[:200]}")
        else:
            check(info["audio_codec"] in ("mpeg_audio", "ac3"), f"{name}: audio codec {info['audio_codec']}")
    else:
        check(info["audio_codec"] == "none", f"{name}: expected no audio")
    check(int(info["cc_errors"]) == 0 and int(info["sync_losses"]) == 0 and int(info["damaged_aus"]) == 0,
          f"{name}: clean file reports errors")

print("== damage tests (on s720)")
raw = open(files["s720"], "rb").read()
base = tsinfo(files["s720"])
rnd = random.Random(7)
def write(name, data):
    p = f"{TMP}/{name}.ts"; open(p, "wb").write(data); return p

# a) garbage in front and a cut start in the middle of a packet
p = write("garbage_front", bytes(rnd.randrange(256) for _ in range(333)) + raw[50:])
i = tsinfo(p)
check(int(i["video_aus"]) >= int(base["video_aus"]) - 3, f"garbage start: {i['video_aus']} AUs vs {base['video_aus']}")
check(i["width"] == base["width"], "garbage start: SPS lost")
# b) drop video packets
pk = [raw[o:o + 188] for o in range(0, len(raw) - 187, 188)]
vpid = int(base["streams"][0].split(",")[0].split(":")[1]) if "h264" in base["streams"][0] else None
vp = [k for k, x in enumerate(pk) if (((x[1] & 0x1F) << 8) | x[2]) == vpid and not (x[1] & 0x40)]
drop = set(rnd.sample(vp, 6))
p = write("dropped", b"".join(x for k, x in enumerate(pk) if k not in drop))
i = tsinfo(p)
check(int(i["cc_errors"]) >= 1, "dropped packets: no continuity error reported")
check(int(i["damaged_aus"]) >= 1, "dropped packets: no damaged AU reported")
check(int(i["video_aus"]) >= int(base["video_aus"]) - 2, f"dropped packets: {i['video_aus']} AUs vs {base['video_aus']}")
# c) a hole of 1 second in the middle
mid = len(pk) // 2
p = write("hole", b"".join(pk[:mid] + pk[mid + 600:]))
i = tsinfo(p)
check(int(i["video_aus"]) > int(base["video_aus"]) // 2, "hole: lost too much")
# d) scrambled flag on video packets
sc = bytearray(raw)
for o in range(0, len(sc) - 187, 188):
    if sc[o] == 0x47 and (((sc[o + 1] & 0x1F) << 8) | sc[o + 2]) == vpid: sc[o + 3] |= 0xC0
p = write("scrambled", bytes(sc))
i = tsinfo(p)
check(int(i["scrambled"]) > 0 and int(i["video_aus"]) == 0, f"scrambled: {i['scrambled']} packets, {i['video_aus']} AUs")

print("== 1.2.4d: H.264 parameter sets in a PES of their own")
# Every unit of s360 is cut in two PES: what comes before its first slice (AUD / SPS / PPS / SEI) and the slices.
# The Vita decoder refused such parameter-only units (0x8062000F, "Last of Us" HLS froze): they must be joined.
_xp = f"{TMP}/split_src"
tsinfo(files["s360"], "-o", _xp)
_es = open(_xp + ".video", "rb").read()
def _nals(b):
    out, k = [], 0
    while True:
        k = b.find(b"\x00\x00\x01", k)
        if k < 0: break
        out.append(k); k += 3
    return out
_starts = _nals(_es)
_aus, _cur, _seen_aud = [], [], False
for _n, _st in enumerate(_starts):
    _beg = _st - 1 if _st > 0 and _es[_st - 1] == 0 else _st
    _end = (_starts[_n + 1] - (1 if _es[_starts[_n + 1] - 1] == 0 else 0)) if _n + 1 < len(_starts) else len(_es)
    _t = _es[_st + 3] & 0x1F
    if _t == 9:
        if _cur and _seen_aud: _aus.append(_cur)
        _cur = []; _seen_aud = True
    if _seen_aud: _cur.append((_t, _es[_beg:_end]))   # (tsinfo's own SPS / PPS before the first AUD: left out)
if _cur: _aus.append(_cur)
def _pes(payload, pts):
    p5 = bytes([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE), (pts >> 7) & 0xFF, 0x01 | ((pts << 1) & 0xFE)])
    return b"\x00\x00\x01\xE0\x00\x00\x80\x80\x05" + p5 + payload
def _crc(b):
    c = 0xFFFFFFFF
    for x in b:
        c ^= x << 24
        for _ in range(8): c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c.to_bytes(4, "big")
def _section(pid, body):
    sec = body + _crc(body)
    return bytes([0x47, 0x40 | (pid >> 8), pid & 0xFF, 0x10, 0]) + sec + b"\xFF" * (183 - len(sec))
_pat = _section(0, bytes([0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0x00]))
_pmt = _section(0x100, bytes([0x02, 0xB0, 18, 0, 1, 0xC1, 0, 0, 0xE1, 0x01, 0xF0, 0, 0x1B, 0xE1, 0x01, 0xF0, 0]))
_cc = [0]
def _pkts(pes):
    out, first = [], True
    while pes:
        chunk = pes[:184]
        hdr = bytes([0x47, (0x40 if first else 0) | 0x01, 0x01])
        if len(chunk) < 184:
            pad = 184 - len(chunk) - 1
            af = bytes([pad]) + (bytes([0]) + b"\xFF" * (pad - 1) if pad > 0 else b"")
            out.append(hdr + bytes([0x30 | _cc[0]]) + af + chunk)
        else:
            out.append(hdr + bytes([0x10 | _cc[0]]) + chunk)
        _cc[0] = (_cc[0] + 1) & 15
        pes = pes[184:]; first = False
    return out
_out, _nsplit = [_pat, _pmt], 0
for _k, _au in enumerate(_aus):
    _pts = 126000 + _k * 3600
    _pre = b"".join(d for t, d in _au[:next((j for j, (t, _d) in enumerate(_au) if 1 <= t <= 5), len(_au))])
    _rest = b"".join(d for t, d in _au)[len(_pre):]
    if _pre and _rest:
        _out += _pkts(_pes(_pre, _pts)); _nsplit += 1
    if _rest: _out += _pkts(_pes(_rest, _pts))
    elif _pre: _out += _pkts(_pes(_pre, _pts))
_sp = write("split_params", b"".join(_out))
i = tsinfo(_sp, "-o", f"{TMP}/split_out")
check(int(i["joined_aus"]) == _nsplit, f"split params: {i['joined_aus']} joined, {_nsplit} parameter-only PES")
check(int(i["video_aus"]) == len(_aus), f"split params: {i['video_aus']} units, {len(_aus)} pictures")
_bk = tsinfo(files["s360"])["keyframes"]
check(i["keyframes"] == _bk, f"split params: keyframes {i['keyframes']} vs {_bk}")
_dec = sh(f"ffmpeg -hide_banner -loglevel error -f h264 -i {TMP}/split_out.video -f null -", check=False)
check(_dec.returncode == 0 and _dec.stderr.strip() == "", f"split params: joined stream decode errors: {_dec.stderr[:200]}")
check(open(f"{TMP}/split_out.video", "rb").read() == _es, "split params: the joined stream differs from the original")
print(f"  {len(_aus)} pictures, {_nsplit} parameter-only PES joined")

print("== fuzz (sanitizers)")
r = sh(f"{TMP}/fuzz_ts 140 {files['s720']} {files['sbase']} {files['sinter']} {files['shevc']} {files['smpeg2']}", check=False)
print(r.stdout.strip())
check(r.returncode == 0 and "ERROR" not in r.stderr, "fuzz: sanitizer reported a problem: " + r.stderr[:600])

print()
if fails:
    print(len(fails), "FAILURES"); sys.exit(1)
print("ALL TS DEMUXER TESTS PASSED")
