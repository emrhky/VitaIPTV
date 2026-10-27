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
   f"-c:v libx264 -g 50 -pix_fmt yuv420p -c:a ac3 -ac 6 -f mpegts {T}/ac3.ts")
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
# 0.3.0: a 540p variant is chosen first (automatic); one picked from the list by its bit rate
open(f"{T}/hls/master3.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080\nhd/big.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nindex.m3u8?v=720\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1600000,RESOLUTION=960x540\nindex.m3u8?v=540\n")
# 0.4.0: a channel whose address redirects to another server (Star TV): the variants are relative to the
# address that answered, not to the one in the list
open(f"{T}/hls/master_dyg.m3u8", "w").write("#EXTM3U\n#EXT-X-VERSION:3\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=3000000,RESOLUTION=1920x1080\nlive_1080p3000000kbps/index.m3u8?st=K1&e=1\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1500000,RESOLUTION=1280x720\nlive_720p1500000kbps/index.m3u8?st=K1&e=1\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=900000,RESOLUTION=854x480\nlive_480p900000kbps/index.m3u8?st=K1&e=1\n")
# 0.4.0: a variant with sound only (Haber61's "480p")
os.makedirs(f"{T}/hls/nov", exist_ok=True)
for i in range(5):
    sh(f"ffmpeg -loglevel error -y -i {T}/hls/seg{i:03d}.ts -vn -c:a copy -copyts -f mpegts {T}/hls/nov/seg{i:03d}.ts")
open(f"{T}/hls/nov.m3u8", "w").write("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
    + "".join(f"#EXTINF:1.000000,\nnov/seg{i:03d}.ts\n" for _ in range(3) for i in range(5)) + "#EXT-X-ENDLIST\n")
open(f"{T}/hls/master_nov.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=210000,RESOLUTION=854x480\nnov.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nindex.m3u8\n")
# 0.4.0: 720p25 whose SPS has no VUI (no bitstream_restriction), IBBP with 2 references (Ciftci TV): the
# player adds the restriction for the decoder
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi "
   f"-i sine=frequency=440:sample_rate=48000 -t 10 -c:v libx264 -profile:v main -bf 2 -b-pyramid none -refs 2 "
   f"-g 50 -pix_fmt yuv420p -c:a aac -f mpegts {T}/nvx.ts")
sh(f"python3 {ROOT}/tests/ts_strip_vui.py {T}/nvx.ts {T}/hls/novui.ts")
# 0.4.0: a "480p" variant that sends video units but no keyframe (Haber61 on its second test): IDR, SPS and
# PPS taken out; the player must go on to another variant
os.makedirs(f"{T}/hls/nok", exist_ok=True)
for i in range(5):
    sh(f"ffmpeg -loglevel error -y -i {T}/hls/seg{i:03d}.ts -c copy -bsf:v \"filter_units=remove_types=5|7|8\" -copyts "
       f"-f mpegts {T}/hls/nok/seg{i:03d}.ts")
open(f"{T}/hls/nok.m3u8", "w").write("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
    + "".join(f"#EXTINF:1.000000,\nnok/seg{i:03d}.ts\n" for _ in range(3) for i in range(5)) + "#EXT-X-ENDLIST\n")
open(f"{T}/hls/master_nok.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=854x480\nnok.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nindex.m3u8\n")
# 0.4.0: 720p25 with NAL HRD, pic_struct and buffering period / picture timing SEI (Ciftci TV); the mock
# decoder refuses such units like the Vita did, the player's second try leaves the timing out
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi "
   f"-i sine=frequency=440:sample_rate=48000 -t 10 -c:v libx264 -profile:v main -bf 2 -b-pyramid none -refs 2 "
   f"-g 50 -b:v 1200k -maxrate 1200k -bufsize 600k -x264-params nal-hrd=vbr:pic-struct=1 -pix_fmt yuv420p "
   f"-c:a aac -f mpegts {T}/hls/timing.ts")
# 0.5.0: 720p25 with 4 reference pictures and the timing information: the HD decoder from the start
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi "
   f"-i sine=frequency=440:sample_rate=48000 -t 6 -c:v libx264 -preset ultrafast -profile:v main -bf 2 -refs 4 "
   f"-g 50 -b:v 1500k -maxrate 1500k -bufsize 750k -x264-params nal-hrd=vbr:pic-struct=1 -pix_fmt yuv420p "
   f"-c:a aac -f mpegts {T}/hls/timing4.ts")
# 0.4.1: the same at 720p50 (the HD decoder is chosen for the frame rate)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=50 -f lavfi "
   f"-i sine=frequency=440:sample_rate=48000 -t 10 -c:v libx264 -preset ultrafast -profile:v main -bf 2 -b-pyramid none -refs 2 "
   f"-g 50 -b:v 1500k -maxrate 1500k -bufsize 750k -x264-params nal-hrd=vbr:pic-struct=1 -pix_fmt yuv420p "
   f"-c:a aac -f mpegts {T}/hls/timing50.ts")
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
# 0.5.0: two audio tracks, Turkish AAC and English AC-3 (TS with ISO 639 descriptors, MKV with Language)
for ext, fmt in (("ts", "mpegts"), ("mkv", "matroska")):
    sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 "
       f"-f lavfi -i sine=frequency=880:sample_rate=48000 -t 6 -map 0:v -map 1:a -map 2:a -c:v libx264 -g 25 -pix_fmt yuv420p "
       f"-c:a:0 aac -c:a:1 ac3 -metadata:s:a:0 language=tur -metadata:s:a:1 language=eng -f {fmt} {T}/hls/twoaudio.{ext}")
# 0.6.0: HLS with the sound in playlists of its own (#EXT-X-MEDIA): picture-only TS segments, Turkish sound as
# audio-only TS segments, English as packed AAC (.aac with an ID3 time stamp, as Apple's packager makes them;
# the times are those of the TS segments, which the HLS muxer moved)
os.makedirs(f"{T}/alt", exist_ok=True)
sh(f"ffmpeg -loglevel error -y -i {T}/s720g30.ts -an -c copy -copyts -f hls -hls_time 1 -hls_list_size 0 -hls_playlist_type vod "
   f"-hls_segment_filename {T}/alt/av%03d.ts {T}/alt/altv.m3u8")
sh(f"ffmpeg -loglevel error -y -i {T}/s720g30.ts -vn -c copy -copyts -f hls -hls_time 1 -hls_list_size 0 -hls_playlist_type vod "
   f"-hls_segment_filename {T}/alt/aa%03d.ts {T}/alt/alttr.m3u8")
sh(f"ffmpeg -loglevel error -y -i {T}/s720g30.ts -vn -c copy -f adts {T}/alt/all.aac")
st = subprocess.run(f"ffprobe -v error -select_streams a:0 -show_entries stream=start_pts -of csv=p=0 {T}/alt/aa000.ts",
                    shell=True, capture_output=True, text=True).stdout.strip()
apts = int(st.split()[0].split(",")[0]) if st else 126000
adts = open(f"{T}/alt/all.aac", "rb").read()
frames, i = [], 0
while i + 7 <= len(adts):
    ln = ((adts[i + 3] & 3) << 11) | (adts[i + 4] << 3) | (adts[i + 5] >> 5)
    frames.append(adts[i:i + ln]); i += ln
def id3_ts(pts):
    owner = b"com.apple.streaming.transportStreamTimestamp\0"
    body = owner + (pts & 0x1FFFFFFFF).to_bytes(8, "big")
    frame = b"PRIV" + len(body).to_bytes(4, "big") + b"\0\0" + body
    n = len(frame)
    size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])
    return b"ID3\x04\x00\x00" + size + frame
per = 47                                                     # about 1 s of 48 kHz AAC frames
pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
for k in range(0, len(frames), per):
    seg = id3_ts(apts + k * 1024 * 90000 // 48000) + b"".join(frames[k:k + per])
    open(f"{T}/alt/ae{k // per:03d}.aac", "wb").write(seg)
    pl += f"#EXTINF:{len(frames[k:k + per]) * 1024 / 48000:.3f},\nae{k // per:03d}.aac\n"
open(f"{T}/alt/alten.m3u8", "w").write(pl + "#EXT-X-ENDLIST\n")
open(f"{T}/alt/altm.m3u8", "w").write("#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",LANGUAGE=\"tr\",NAME=\"Turkce\",DEFAULT=YES,AUTOSELECT=YES,URI=\"alttr.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",LANGUAGE=\"en\",NAME=\"English\",DEFAULT=NO,AUTOSELECT=YES,URI=\"alten.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.640020,mp4a.40.2\",AUDIO=\"aud\"\naltv.m3u8\n")
for f in ("altv", "alttr"):                                  # live copies (no #EXT-X-ENDLIST): both start 3 s from the end
    open(f"{T}/alt/{f}_l.m3u8", "w").write(open(f"{T}/alt/{f}.m3u8").read().replace("#EXT-X-ENDLIST\n", "").replace("#EXT-X-PLAYLIST-TYPE:VOD\n", ""))
open(f"{T}/alt/altml.m3u8", "w").write("#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"tr\",NAME=\"Turkce\",DEFAULT=YES,URI=\"alttr_l.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,AUDIO=\"a\"\naltv_l.m3u8\n")
# 0.6.0: a 40 s film for the fast / slow line tests (the queues' sizes)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 40 "
   f"-c:v libx264 -g 50 -b:v 600k -pix_fmt yuv420p -c:a aac -ac 2 -f matroska {T}/hls/long.mkv")
# 0.6.0: 5.1 AAC (the Vita's decoder takes two channels; FFmpeg's aac decoder mixes it down) in TS, MKV, MP4
for ext, fmt in (("ts", "mpegts"), ("mkv", "matroska"), ("mp4", "mp4")):
    sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 "
       f"-c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 6 -b:a 256k -f {fmt} {T}/hls/aac51.{ext}")
# 0.7.0: a film with two text subtitle tracks (Turkish SRT, English ASS "SDH")
open(f"{T}/hls/subs_tr.srt", "w").write("1\n00:00:01,000 --> 00:00:02,000\nBir\n\n2\n00:00:03,000 --> 00:00:04,000\nİki\n\n")
open(f"{T}/hls/subs_en.ass", "w").write("[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,0\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\i1}One{\\i0}\\Nline two\nDialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,Two\n")
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -i {T}/hls/subs_tr.srt -i {T}/hls/subs_en.ass "
   f"-map 0 -map 1 -map 2 -map 3 -t 5 -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 2 -c:s:0 srt -c:s:1 ass "
   f"-metadata:s:s:0 language=tur -metadata:s:s:1 language=eng -metadata:s:s:1 title=SDH {T}/hls/subs.mkv")
# 1.0: a stream whose 33-bit TS clock wraps after about 6 s (2^33 / 90000 = 95443.7 s)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 "
   f"-c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -ac 2 -output_ts_offset 95436 -f mpegts {T}/wrap.ts")
M = f"{ROOT}/tests/mock_rt"
# with FFMPEG_PREFIX (an FFmpeg with the ac3/eac3 decoders, see run_a52_test.py) the AC-3 audio is decoded too
FP = os.environ.get("FFMPEG_PREFIX", "")
A52 = f"-DHAVE_A52 -I{FP}/include" if FP and os.path.exists(f"{FP}/lib/libavcodec.a") else ""
A52L = f"{FP}/lib/libavcodec.a {FP}/lib/libavutil.a" if A52 else ""
print("AC-3 decoding:", "with FFmpeg from " + FP if A52 else "not built in")
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -DSTALL_US=1500000ULL {A52} -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_tsplayer.c {ROOT}/src/tsplayer.c {ROOT}/src/spsfix.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c {ROOT}/src/mpadec.c {ROOT}/src/a52dec.c {ROOT}/src/hls.c {ROOT}/src/curlio.c {ROOT}/src/vod.c {M}/mock_rt.c {A52L} -lm -o {T}/test_tsplayer")
r = subprocess.run(f"{T}/test_tsplayer {T}/s720.ts {T}/s1080.ts {T}/hevc.ts {T}/midgop.ts {T}/dropped.ts "
                   f"{T}/opengop_mid.ts {T}/audio.ts {T}/scrambled.ts {T}/alate.ts {T}/aearly.ts {T}/mono441.ts {T}/ac3.ts {T}/bframes.mkv {T}/s1080.ts {T}/mp2.ts {T}/hls {T}/film.mp4 {T}/film.mkv {T}/film.ts {T}/film.flv {T}/s720.flv {T}/i1080.ts {T}/glitch.ts {T}/aacmain.ts {T}/fast60.ts {T}/fast60_ac3.ts {T}/aahead.ts {T}/hls2 {T}/hls3 {T}/hls4 {T}/f1080_60.ts {T}/wrap.ts",
                   shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
