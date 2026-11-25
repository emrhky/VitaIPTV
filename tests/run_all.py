#!/usr/bin/env python3
"""1.2.2: all PC tests in one go (GitHub Actions runs this, see .github/workflows/build.yml).
   Needs gcc, python3, ffmpeg / ffprobe (with libx264), openssl, zlib. Optional: FFMPEG_PREFIX (an FFmpeg built
   with tools/build_ffmpeg_pc.sh: AC-3 / AAC / Opus / Vorbis / DTS decoding is then tested too), libjpeg / libpng /
   Pillow (posters), OpenCV (the QR code): without them those tests say SKIPPED.
   ONLY=unit runs only the quick ones. Exit code 1 when any test failed."""
import os, subprocess, sys, tempfile, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="all_")
FAST = os.environ.get("ONLY") == "unit"
SAN = "-O1 -g -fsanitize=address,undefined -Wall"
UNIT = [   # name, sources (after tests/test_<name>.c), extra flags
    ("audiofx", "src/audiofx.c", ""), ("aac51", "src/aac51.c src/vsurr.c", ""), ("avpref", "src/avpref.c", ""),
    ("epg", "src/epg.c", ""), ("fav", "src/fav.c src/iptv.c", ""), ("host", "src/iptv.c src/lang.c", ""),
    ("httpio", "src/httpio.c", "-Itests/mock"), ("listcache", "src/listcache.c", ""),
    ("mylist", "src/mylist.c src/fav.c src/iptv.c", ""), ("srt", "src/srt.c", ""), ("websrc", "src/websrc.c src/iptv.c", ""),
    ("hls", "src/hls.c", ""), ("guide", "src/guide.c src/epg.c src/iptv.c src/catchup.c -lz", "-DGUIDE_ZLIB"),
]
SCRIPTS = ["check_lang.py", "run_hlsaes_test.py", "run_ts_tests.py", "run_mkv_tests.py", "run_flv_tests.py", "run_vod_tests.py", "run_ps_tests.py",
           "run_spsfix_test.py", "run_a52_test.py", "run_probe_tests.py", "run_curlio_test.py", "run_dl_test.py",
           "run_poster_test.py", "run_qr_test.py", "run_https_tests.py", "run_player_tests.py"]
QUICK = {"check_lang.py", "run_hlsaes_test.py"}
results = []
def run(name, cmd, cwd, timeout):
    t0 = time.time()
    try:
        r = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        out, ok = (r.stdout + r.stderr), r.returncode == 0
    except subprocess.TimeoutExpired as e:
        out, ok = f"timed out after {timeout} s\n" + ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")), False
    last = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
    state = "SKIPPED" if ok and "SKIPPED" in out else "ok" if ok else "FAILED"
    results.append((name, state, time.time() - t0))
    print(f"{state:8} {name:24} {time.time() - t0:6.1f} s  {last[0][:110]}", flush=True)
    if not ok: print("    " + "\n    ".join(out.strip().splitlines()[-40:]), flush=True)
for name, srcs, fl in UNIT:
    run("test_" + name, f"gcc {SAN} {fl} -Isrc tests/test_{name}.c {srcs} -lm -o {T}/t_{name} && cd {T} && ./t_{name}", ROOT, 300)
for s in SCRIPTS:
    if FAST and s not in QUICK: continue
    env = "TSP_DIR=1 " if s == "run_player_tests.py" else ""
    run(s, f"{env}{sys.executable} {s}", f"{ROOT}/tests", 3600)
bad = [n for n, s, _ in results if s == "FAILED"]
print(f"\n{len(results)} tests: {len(results) - len(bad)} passed or skipped, {len(bad)} failed" + (": " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
