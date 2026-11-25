#!/usr/bin/env python3
"""src/mpadec.c (minimp3) against ffmpeg's decode: MP2 and MP3, mono and stereo (1.2.4n: test_mpadec.c was not run)."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="mpa_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r.stdout
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -I{ROOT}/src {ROOT}/tests/test_mpadec.c {ROOT}/src/mpadec.c -lm -o {T}/t")
ok = True
for codec, ext, rate, ch in (("mp2", "mp2", 48000, 2), ("libmp3lame", "mp3", 44100, 1)):
    sh(f"ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate={rate} -t 4 -ac {ch} -c:a {codec} -b:a 128k {T}/a.{ext}")
    sh(f"ffmpeg -loglevel error -y -i {T}/a.{ext} -f s16le -ac {ch} -ar {rate} {T}/a.{ext}.s16")
    r = subprocess.run(f"{T}/t {T}/a.{ext} {T}/a.{ext}.s16 {rate} {ch}", shell=True, capture_output=True, text=True)
    print(ext, r.stdout.strip(), r.stderr[-2000:])
    ok = ok and r.returncode == 0
print("ALL MPADEC TESTS PASSED" if ok else "MPADEC TESTS FAILED")
sys.exit(0 if ok else 1)
