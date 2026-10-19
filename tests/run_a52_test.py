#!/usr/bin/env python3
"""AC-3 / E-AC-3 decoding (src/a52dec.c) with an FFmpeg built like tools/build_ffmpeg_vita.sh, but for the PC.
Needs gcc and ffmpeg. FFMPEG_PREFIX = where that FFmpeg is installed (include/, lib/); without it the test is
skipped (the app then plays without AC-3, as before)."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.environ.get("FFMPEG_PREFIX", "")
if not P or not os.path.exists(os.path.join(P, "lib", "libavcodec.a")):
    print("SKIPPED: set FFMPEG_PREFIX to an FFmpeg with the ac3/eac3 decoders"); sys.exit(0)
T = tempfile.mkdtemp(prefix="a52_")
def sh(c): subprocess.run(c, shell=True, check=True)
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -DHAVE_A52 -I{ROOT}/src -I{P}/include {ROOT}/tests/test_a52.c "
   f"{ROOT}/src/a52dec.c {P}/lib/libavcodec.a {P}/lib/libavutil.a -lm -lpthread -o {T}/t")
SRC = "-f lavfi -i sine=frequency=440:sample_rate={r} -f lavfi -i sine=frequency=660:sample_rate={r} -f lavfi -i sine=frequency=880:sample_rate={r} -t 4"
for name, codec, rate, layout in (("ac3_51.ac3", "ac3", 48000, "5.1"), ("eac3_51.eac3", "eac3", 48000, "5.1"),
                                  ("ac3_st.ac3", "ac3", 44100, "stereo"), ("eac3_71.eac3", "eac3", 48000, "7.1")):
    sh(f"ffmpeg -loglevel error -y {SRC.format(r=rate)} -filter_complex \"[0][1][2]amerge=inputs=3,pan={layout}|c0=c0|c1=c1"
       + ("|c2=c2|c3=c0|c4=c1|c5=c2" if layout != "stereo" else "") + ("|c6=c0|c7=c1" if layout == "7.1" else "") + "\" "
       f"-c:a {codec} -b:a 384k {T}/{name}")
    sh(f"ffmpeg -loglevel error -y -downmix stereo -i {T}/{name} -ac 2 -f s16le -acodec pcm_s16le {T}/{name}.s16")   # the decoder's own downmix, as in the app
    r = subprocess.run(f"{T}/t {T}/{name} {T}/{name}.s16 {rate}", shell=True, capture_output=True, text=True)
    print(r.stdout, r.stderr[-3000:])
    if r.returncode: sys.exit(1)
print("ALL A52 TESTS PASSED")
