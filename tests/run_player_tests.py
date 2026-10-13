#!/usr/bin/env python3
"""Integration test for src/tsplayer.c with a fake decoder (needs gcc, ffmpeg)."""
import os, random, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.environ.get("TSP_DIR") or tempfile.mkdtemp(prefix="tsp_")   # TSP_DIR: reuse the test files of an earlier run
os.makedirs(T, exist_ok=True)
def sh(c):
    if os.environ.get("TSP_DIR") and c.startswith("ffmpeg") and os.path.exists(c.split()[-1]): return None
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r
SRC = "-f lavfi -i testsrc2=size={s}:rate={r} -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5"
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/s720.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1920x1080', r=25)} -c:v libx264 -profile:v main -g 50 -pix_fmt yuv420p -c:a aac -f mpegts {T}/s1080.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='640x360', r=25)} -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac -f mpegts {T}/hevc.ts")
raw = open(f"{T}/s720.ts", "rb").read()
pk = [raw[o:o + 188] for o in range(0, len(raw) - 187, 188)]
open(f"{T}/midgop.ts", "wb").write(b"".join(pk[len(pk) // 3:]))        # starts in the middle of a GOP
vid = [k for k, x in enumerate(pk) if ((x[1] & 0x1F) << 8 | x[2]) == 256 and not (x[1] & 0x40)]
drop = set(random.Random(5).sample(vid, 8))
open(f"{T}/dropped.ts", "wb").write(b"".join(x for k, x in enumerate(pk) if k not in drop))
# I-pictures that are not IDR and carry no recovery SEI: every IDR after the first is
# relabelled as a plain slice (NAL type 5 -> 1), SEI removed, then cut mid-GOP.
# (The relabelled stream is only for the demuxer/player logic; a real decoder would object.)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -t 8 -c:v libx264 -bf 0 "
   f"-x264-params keyint=50:min-keyint=50:scenecut=0 -pix_fmt yuv420p -f h264 {T}/og.h264")
es = open(f"{T}/og.h264", "rb").read()
starts = [i for i in range(len(es) - 3) if es[i:i + 3] == b"\0\0\1"]
bounds = starts + [len(es)]
keep, seen_idr = bytearray(), 0
for a, b in zip(bounds, bounds[1:]):
    t = es[a + 3] & 0x1F if a + 3 < len(es) else 0
    if t == 6: continue
    nal = bytearray(es[a:b])
    if t == 5:
        seen_idr += 1
        if seen_idr > 1: nal[3] = (nal[3] & 0xE0) | 1
    keep += nal
open(f"{T}/og_nosei.h264", "wb").write(bytes(keep))
sh(f"ffmpeg -loglevel error -y -fflags +genpts -r 25 -f h264 -i {T}/og_nosei.h264 -c copy -f mpegts {T}/og.ts")
info = sh(f"ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 {T}/og.ts").stdout.split()
print("open-GOP test stream:", len(info), "pictures")
ograw = open(f"{T}/og.ts", "rb").read()
ogpk = [ograw[o:o + 188] for o in range(0, len(ograw) - 187, 188)]
open(f"{T}/opengop_mid.ts", "wb").write(b"".join(ogpk[len(ogpk) // 3:]))
sh(f"ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 -c:a aac -f mpegts {T}/audio.ts")
sc = bytearray(raw)
for o in range(0, len(sc) - 187, 188):
    if sc[o] == 0x47 and ((sc[o + 1] & 0x1F) << 8 | sc[o + 2]) == 256: sc[o + 3] |= 0xC0
open(f"{T}/scrambled.ts", "wb").write(bytes(sc))
# audio packets moved later / earlier in the file than their timestamps
def shift_audio(src, dst, seconds):
    d = open(src, "rb").read()
    p = [d[o:o + 188] for o in range(0, len(d) - 187, 188)]
    per_s = len(p) / 5.0
    keyed = []
    for k, x in enumerate(p):
        pid = (x[1] & 0x1F) << 8 | x[2]
        # never in front of the PAT/PMT at the start, or the demuxer cannot know the stream yet
        keyed.append((max(3, k + seconds * per_s) if pid == 257 else k, k, x))
    keyed.sort(key=lambda e: (e[0], e[1]))
    open(dst, "wb").write(b"".join(x for _, _, x in keyed))
shift_audio(f"{T}/s720.ts", f"{T}/alate.ts", 1.5)
shift_audio(f"{T}/s720.ts", f"{T}/aearly.ts", -1.5)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 5 "
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a aac -ac 1 -f mpegts {T}/mono441.ts")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5 "
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a ac3 -f mpegts {T}/ac3.ts")
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2 -f matroska {T}/bframes.mkv")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5 "
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a mp2 -b:a 192k -f mpegts {T}/mp2.ts")
# HLS: the 720p test stream cut into 1 s segments, a master playlist with a 1080p and a 720p variant,
# and a "live" copy of the media playlist (no #EXT-X-ENDLIST)
os.makedirs(f"{T}/hls", exist_ok=True)
sh(f"ffmpeg -loglevel error -y {SRC.format(s='1280x720', r=30)} -c:v libx264 -profile:v high -bf 3 -g 30 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/s720g30.ts")
sh(f"ffmpeg -loglevel error -y -i {T}/s720g30.ts -c copy -f hls -hls_time 1 -hls_list_size 0 -hls_playlist_type vod "
   f"-hls_segment_filename {T}/hls/seg%03d.ts {T}/hls/index.m3u8")
open(f"{T}/hls/master.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080\nhd/big.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.640020,mp4a.40.2\"\nindex.m3u8?token=1\n")
open(f"{T}/hls/master2.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nmissing720.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=854x480\nindex.m3u8\n")
live = open(f"{T}/hls/index.m3u8").read().replace("#EXT-X-ENDLIST", "").replace("#EXT-X-PLAYLIST-TYPE:VOD\n", "")
open(f"{T}/hls/live.m3u8", "w").write(live)
VSRC = ("-f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 "
        "-c:v libx264 -bf 2 -g 25 -keyint_min 25 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -ac 2")
sh(f"ffmpeg -loglevel error -y {VSRC} {T}/film.mp4")
sh(f"ffmpeg -loglevel error -y {VSRC} {T}/film.mkv")
sh(f"ffmpeg -loglevel error -y {VSRC} -f mpegts {T}/film.ts")
sh(f"ffmpeg -loglevel error -y {VSRC} -f flv {T}/film.flv")
sh(f"ffmpeg -loglevel error -y -i {T}/s720.ts -c copy -f flv {T}/s720.flv")
# a stream where three AAC frames in the middle claim 7 channels (seen live after a skipped HLS segment)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5 "
   f"-c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/glitch.ts")
g = bytearray(open(f"{T}/glitch.ts", "rb").read())
hits, seen = 0, 0
for o in range(0, len(g) - 188 + 1, 188):
    pid = ((g[o + 1] & 0x1F) << 8) | g[o + 2]
    if pid != 0x101: continue
    i = g.find(b"\xff\xf1", o + 4, o + 188)
    if i < 0 or i + 4 > o + 188: continue
    seen += 1
    if seen in (60, 61, 62):
        g[i + 2] = (g[i + 2] & 0xFE) | 1; g[i + 3] = (g[i + 3] & 0x3F) | (3 << 6); hits += 1   # channel config 7
assert hits == 3, hits
open(f"{T}/glitch.ts", "wb").write(g)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 "
   f"-c:v libx264 -flags +ilme+ildct -top 1 -g 25 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/i1080.ts")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=24000 -t 5 "
   f"-c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -profile:a aac_main -ac 2 -f mpegts {T}/aacmain.ts")
# a live stream whose sound is 3 s ahead of the picture in the file (seen on the device), 12 s
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 "
   f"-c:v libx264 -preset ultrafast -bf 2 -g 50 -b:v 1500k -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/a12.ts")
def shift_audio_n(src, dst, seconds, dur):
    d = open(src, "rb").read()
    p = [d[o:o + 188] for o in range(0, len(d) - 187, 188)]
    per_s = len(p) / float(dur)
    keyed = [((max(3, k + seconds * per_s) if ((x[1] & 0x1F) << 8 | x[2]) == 257 else k), k, x) for k, x in enumerate(p)]
    keyed.sort(key=lambda e: (e[0], e[1]))
    open(dst, "wb").write(b"".join(x for _, _, x in keyed))
shift_audio_n(f"{T}/a12.ts", f"{T}/aahead.ts", -3, 12)
# HLS 1080p as on the device: 10 s segments, the sound 8 s ahead of the picture in the stream, 30 s
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 30 "
   f"-c:v libx264 -preset ultrafast -bf 2 -g 50 -b:v 2000k -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/a30hd.ts")
shift_audio_n(f"{T}/a30hd.ts", f"{T}/a30ahead.ts", -8, 30)
os.makedirs(f"{T}/hls4", exist_ok=True)
ah4 = open(f"{T}/a30ahead.ts", "rb").read()
npk4 = len(ah4) // 188
pl4 = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n"
for i in range(3):
    a, b = npk4 * i // 3, npk4 * (i + 1) // 3
    open(f"{T}/hls4/t{i}.ts", "wb").write(ah4[a * 188:b * 188])
    pl4 += f"#EXTINF:10.0,\nt{i}.ts\n"
open(f"{T}/hls4/t.m3u8", "w").write(pl4 + "#EXT-X-ENDLIST\n")
# the same as HLS: 2 s segments cut out of that file (the sound stays 3 s ahead inside them)
os.makedirs(f"{T}/hls2", exist_ok=True)
ah = open(f"{T}/aahead.ts", "rb").read()
npk = len(ah) // 188
pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
for i in range(6):
    a, b = npk * i // 6, npk * (i + 1) // 6
    open(f"{T}/hls2/s{i}.ts", "wb").write(ah[a * 188:b * 188])
    pl += f"#EXTINF:2.0,\ns{i}.ts\n"
open(f"{T}/hls2/slow.m3u8", "w").write(pl + "#EXT-X-ENDLIST\n")
# HLS with a 1080p variant whose segments stop answering after one (404) and a 720p variant that plays
os.makedirs(f"{T}/hls3", exist_ok=True)
for i in range(6):
    open(f"{T}/hls3/s{i}.ts", "wb").write(open(f"{T}/hls2/s{i}.ts", "rb").read())
    if i == 3: open(f"{T}/hls3/h{i}.ts", "wb").write(open(f"{T}/hls2/s{i}.ts", "rb").read())   # live starts at #3
open(f"{T}/hls3/master.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2200000,RESOLUTION=1920x1080\nhd.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1050000,RESOLUTION=1280x720\nsd.m3u8\n")
for name, pre in (("hd", "h"), ("sd", "s")):
    open(f"{T}/hls3/{name}.m3u8", "w").write("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n" +
        "".join(f"#EXTINF:2.0,\n{pre}{i}.ts\n" for i in range(6)))
# 1080p60 with B-pictures (more than the Vita decoder manages), 8 s
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=60 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 "
   f"-c:v libx264 -preset ultrafast -bf 2 -x264-params b-pyramid=0 -g 120 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/f1080_60.ts")
for name, ac in (("fast60", "-c:a aac -ac 2"), ("fast60_ac3", "-c:a ac3")):
    sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=60 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 "
       f"-c:v libx264 -preset ultrafast -bf 2 -x264-params b-pyramid=0 -g 60 -pix_fmt yuv420p {ac} -f mpegts {T}/{name}.ts")
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -DSTALL_US=1500000ULL -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_tsplayer.c {ROOT}/src/tsplayer.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c {ROOT}/src/mpadec.c {ROOT}/src/hls.c {ROOT}/src/curlio.c {ROOT}/src/vod.c {M}/mock_rt.c -lm -o {T}/test_tsplayer")
r = subprocess.run(f"{T}/test_tsplayer {T}/s720.ts {T}/s1080.ts {T}/hevc.ts {T}/midgop.ts {T}/dropped.ts "
                   f"{T}/opengop_mid.ts {T}/audio.ts {T}/scrambled.ts {T}/alate.ts {T}/aearly.ts {T}/mono441.ts {T}/ac3.ts {T}/bframes.mkv {T}/s1080.ts {T}/mp2.ts {T}/hls {T}/film.mp4 {T}/film.mkv {T}/film.ts {T}/film.flv {T}/s720.flv {T}/i1080.ts {T}/glitch.ts {T}/aacmain.ts {T}/fast60.ts {T}/fast60_ac3.ts {T}/aahead.ts {T}/hls2 {T}/hls3 {T}/hls4 {T}/f1080_60.ts",
                   shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
