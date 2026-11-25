#!/usr/bin/env python3
"""1.2.4l: src/psdemux.c: Video CD .dat (RIFF CDXA), MPEG-1 and MPEG-2 (VOB, AC-3) program streams, through the TS
demuxer: every picture, the video unchanged, all of the sound, the length, jumps. Needs gcc, ffmpeg."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdxa
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="ps_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r.stdout
SRC = "-f lavfi -i testsrc2=size={s}:rate=25 -f lavfi -i sine=frequency=440:sample_rate={a} -t {t}"
sh(f"ffmpeg -loglevel error -y {SRC.format(s='352x288', a=44100, t=20)} -target pal-vcd {T}/vcd.mpg")
open(f"{T}/vcd.dat", "wb").write(cdxa.wrap(open(f"{T}/vcd.mpg", "rb").read()))
sh(f"ffmpeg -loglevel error -y {SRC.format(s='352x288', a=44100, t=16)} -c:v mpeg1video -b:v 1500k -g 15 -bf 2 -c:a mp2 -b:a 192k -f mpeg {T}/m1.mpg")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='720x576', a=48000, t=16)} -c:v mpeg2video -b:v 4M -g 12 -bf 2 -c:a ac3 -b:a 192k -f vob {T}/dvd.vob")
r = subprocess.run(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -I{ROOT}/src {ROOT}/tests/test_ps.c {ROOT}/src/psdemux.c "
                   f"{ROOT}/src/tsdemux.c -o {T}/test_ps", shell=True, capture_output=True, text=True)
if r.returncode: print(r.stderr[-3000:]); sys.exit(1)
ok = True
for f, src, secs, vfmt, afmt in (("vcd.dat", "vcd.mpg", 20, "mpeg1video", "mp2"), ("m1.mpg", "m1.mpg", 16, "mpeg1video", "mp2"),
                                 ("dvd.vob", "dvd.vob", 16, "mpeg2video", "ac3")):
    pics = int(sh(f"ffprobe -loglevel error -select_streams v -count_packets -show_entries stream=nb_read_packets -of csv=p=0 {T}/{src}").strip().strip(","))
    sh(f"ffmpeg -loglevel error -y -i {T}/{src} -map 0:v -c copy -f {vfmt} {T}/{f}.ref.es -map 0:a -c copy -f {afmt} {T}/{f}.ref.a")
    abytes = os.path.getsize(f"{T}/{f}.ref.a")
    print(f"{f}: {pics} pictures, {abytes} bytes of sound")
    r = subprocess.run(f"{T}/test_ps {T}/{f} {T}/{f}.es {pics} {abytes} {secs}", shell=True, capture_output=True, text=True)
    print(r.stdout, r.stderr[-3000:])
    if r.returncode: ok = False; continue
    same = open(f"{T}/{f}.es", "rb").read() == open(f"{T}/{f}.ref.es", "rb").read()
    print(f"  video ES the same as ffmpeg's: {same}")
    ok = ok and same
print("ALL PS TESTS PASSED" if ok else "PS TESTS FAILED")
sys.exit(0 if ok else 1)
