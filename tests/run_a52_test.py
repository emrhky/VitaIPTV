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
   f"{ROOT}/src/a52dec.c {ROOT}/src/vsurr.c {P}/lib/libavcodec.a {P}/lib/libavutil.a -lm -lpthread -o {T}/t")
# 0.6.0: 5.1 and 7.1 AAC (ADTS) through FFmpeg's aac decoder. The reference: ffmpeg's decode of all channels,
# mixed here like src/a52dec.c does it (left / right + centre and surrounds at -3 dB, LFE left out, scaled by
# 1 / (1 + 2 * 0.7071)); 7.1 takes the side channels as the surrounds
import array, math
def mix_ref(f32, out, nch):
    a = array.array("f"); a.frombytes(open(f32, "rb").read())
    k = 0.7071; norm = 1 / (1 + 2 * k)
    sl, sr = (6, 7) if nch == 8 else (3, 4) if nch == 5 else (4, 5)   # (5: 5.0 side, no LFE)
    o = array.array("h")
    for i in range(0, len(a) - nch + 1, nch):
        c = a[i + 2] * k
        for v in ((a[i] + c + a[i + sl] * k) * norm, (a[i + 1] + c + a[i + sr] * k) * norm):
            x = int(round(v * 32767)); o.append(max(-32768, min(32767, x)))
    open(out, "wb").write(o.tobytes())
SRC = "-f lavfi -i sine=frequency=440:sample_rate={r} -f lavfi -i sine=frequency=660:sample_rate={r} -f lavfi -i sine=frequency=880:sample_rate={r} -t 4"
for name, codec, rate, layout in (("ac3_51.ac3", "ac3", 48000, "5.1"), ("eac3_51.eac3", "eac3", 48000, "5.1"),
                                  ("ac3_st.ac3", "ac3", 44100, "stereo"), ("eac3_71.eac3", "eac3", 48000, "7.1")):
    sh(f"ffmpeg -loglevel error -y {SRC.format(r=rate)} -filter_complex \"[0][1][2]amerge=inputs=3,pan={layout}|c0=c0|c1=c1"
       + ("|c2=c2|c3=c0|c4=c1|c5=c2" if layout != "stereo" else "") + ("|c6=c0|c7=c1" if layout == "7.1" else "") + "\" "
       f"-c:a {codec} -b:a 384k {T}/{name}")
    if layout == "stereo":
        sh(f"ffmpeg -loglevel error -y -i {T}/{name} -f s16le -acodec pcm_s16le {T}/{name}.s16")
    else:                                                    # 1.2: all channels, mixed as the app does (below)
        sh(f"ffmpeg -loglevel error -y -i {T}/{name} -f f32le -acodec pcm_f32le {T}/{name}.f32")
        nch = int(subprocess.run(f"ffprobe -v error -show_entries stream=channels -of csv=p=0 {T}/{name}", shell=True,
                                 capture_output=True, text=True).stdout.strip())   # (ffmpeg's eac3 encoder makes 7.1 into 5.0)
        mix_ref(f"{T}/{name}.f32", f"{T}/{name}.s16", nch)
    r = subprocess.run(f"{T}/t {T}/{name} {T}/{name}.s16 {rate}", shell=True, capture_output=True, text=True)
    print(r.stdout, r.stderr[-3000:])
    if r.returncode: sys.exit(1)
for name, layout, nch in (("aac_51.aac", "5.1", 6), ("aac_71.aac", "7.1", 8)):
    sh(f"ffmpeg -loglevel error -y {SRC.format(r=48000)} -filter_complex \"[0][1][2]amerge=inputs=3,pan={layout}|c0=c0|c1=c1"
       + "|c2=c2|c3=c0|c4=c1|c5=c2" + ("|c6=c0|c7=c1" if layout == "7.1" else "") + "\" "
       f"-c:a aac -b:a 384k -f adts {T}/{name}")
    sh(f"ffmpeg -loglevel error -y -i {T}/{name} -f f32le -acodec pcm_f32le {T}/{name}.f32")
    mix_ref(f"{T}/{name}.f32", f"{T}/{name}.s16", nch)
    r = subprocess.run(f"{T}/t {T}/{name} {T}/{name}.s16 48000", shell=True, capture_output=True, text=True)
    print(r.stdout, r.stderr[-3000:])
    if r.returncode: sys.exit(1)
# 1.1: "clear dialogue": all channels from the decoder, mixed with the centre at full level, the surrounds at
# half, scaled by 0.6 and peaks above 0.9 rounded off (as src/a52dec.c does it then)
def mix_dialog(f32, out, nch):
    a = array.array("f"); a.frombytes(open(f32, "rb").read())
    sl, sr = (6, 7) if nch == 8 else (4, 5)
    def soft(v):
        x = abs(v)
        if x <= 0.9: return v
        y = 0.9 + 0.1 * math.tanh((x - 0.9) / 0.1)
        return -y if v < 0 else y
    o = array.array("h")
    for i in range(0, len(a) - nch + 1, nch):
        c = a[i + 2]
        for v in ((a[i] + c + a[i + sl] * 0.5) * 0.6, (a[i + 1] + c + a[i + sr] * 0.5) * 0.6):
            x = int(round(soft(v) * 32767)); o.append(max(-32768, min(32767, x)))
    open(out, "wb").write(o.tobytes())
for name, nch, rate in (("ac3_51.ac3", 6, 48000), ("eac3_51.eac3", 6, 48000), ("aac_51.aac", 6, 48000)):
    sh(f"ffmpeg -loglevel error -y -i {T}/{name} -f f32le -acodec pcm_f32le {T}/{name}.all.f32")
    mix_dialog(f"{T}/{name}.all.f32", f"{T}/{name}.dlg.s16", nch)
    r = subprocess.run(f"DIALOG=1 {T}/t {T}/{name} {T}/{name}.dlg.s16 {rate}", shell=True, capture_output=True, text=True)
    print("clear dialogue:", r.stdout, r.stderr[-3000:])
    if r.returncode: sys.exit(1)
print("ALL A52 TESTS PASSED")
