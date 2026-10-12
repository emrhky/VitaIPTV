#!/usr/bin/env python3
"""FLV demuxer tests (gcc, ffmpeg, ffprobe needed): files made by ffmpeg, compared with ffprobe."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="flv_")
fails = []
def sh(c, check=True):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if check and r.returncode: print("FAILED:", c, r.stderr[-1500:]); sys.exit(2)
    return r
def check(cond, msg):
    if not cond: fails.append(msg); print("  FAIL:", msg)
san = "-O1 -g -fsanitize=address,undefined -Wall -Wextra"
SRCS = f"{ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c"
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/tsinfo.c {SRCS} -o {T}/tsinfo")
sh(f"gcc {san} -I{ROOT}/src {ROOT}/tests/fuzz_flv.c {SRCS} -o {T}/fuzz")
def info(path, *a):
    d = {}
    for l in sh(f"{T}/tsinfo {path} {' '.join(a)}").stdout.splitlines():
        k, _, v = l.partition("="); d[k] = v
    return d
SRC = "-f lavfi -i testsrc2=size={s}:rate={r} -f lavfi -i sine=frequency=440:sample_rate={sr} -t 5"
gen = {
 "bframes": f"{SRC.format(s='1280x720', r=30, sr=44100)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2",
 "live":    f"{SRC.format(s='1280x720', r=25, sr=44100)} -c:v libx264 -tune zerolatency -g 50 -pix_fmt yuv420p -c:a aac -ac 1",
 "small":   f"{SRC.format(s='640x360', r=25, sr=48000)} -c:v libx264 -profile:v baseline -g 25 -pix_fmt yuv420p -c:a aac -ac 2",
 "mp3":     f"{SRC.format(s='640x360', r=25, sr=44100)} -c:v libx264 -pix_fmt yuv420p -c:a libmp3lame -ar 44100",
}
for n, args in gen.items():
    sh(f"ffmpeg -loglevel error -y {args} -f flv {T}/{n}.flv")

print("== files made by ffmpeg vs ffprobe")
for n in ("bframes", "live", "small"):
    p = f"{T}/{n}.flv"
    i = info(p, f"-o {T}/{n}")
    pk = lambda sel: int(sh(f"ffprobe -v error -count_packets -select_streams {sel} -show_entries stream=nb_read_packets -of csv=p=0 {p}").stdout.split()[0].strip(','))
    check(i["container"] == "flv" and i["video_codec"] == "h264" and i["audio_codec"] == "aac", f"{n}: codecs {i['video_codec']}/{i['audio_codec']}")
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
i = info(f"{T}/mp3.flv"); check(i["audio_codec"] == "mpeg_audio", "mp3 not recognised"); print("  mp3: audio", i["audio_codec"], i["audio_frames"], "frames")

print("== a header, then a stream cut in the middle of a tag (must find the next tag)")
data = open(f"{T}/live.flv", "rb").read()
cut = data[:13] + data[len(data) // 3:]
open(f"{T}/mid.flv", "wb").write(cut)
i = info(f"{T}/mid.flv")
check(int(i["video_aus"]) > 30 and int(i["audio_frames"]) > 30, f"mid-stream: {i['video_aus']} pictures, {i['audio_frames']} audio frames")
print(f"  {i['video_aus']} pictures, {i['audio_frames']} audio frames recovered, {i['sync_losses']} resyncs")

print("== HEVC is recognised")
r = sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', r=25, sr=48000)} -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac -f flv {T}/hevc.flv", check=False)
if r.returncode == 0:
    i = info(f"{T}/hevc.flv"); check(i["video_codec"] == "hevc", "hevc not recognised: " + i["video_codec"]); print("  hevc: video", i["video_codec"])
else:
    print("  (this ffmpeg cannot write HEVC in FLV; skipped)")

print("== fuzz")
r = sh(f"{T}/fuzz 150 {T}/bframes.flv {T}/live.flv {T}/mp3.flv", check=False)
print(r.stdout.strip())
check(r.returncode == 0 and "ERROR" not in r.stderr, "fuzz: " + r.stderr[-800:])
print("FAILURES: %d" % len(fails) if fails else "ALL FLV TESTS PASSED")
sys.exit(1 if fails else 0)
