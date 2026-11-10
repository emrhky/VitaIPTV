# H.265 software decoding test

A separate small app (not part of Vita IPTV) that measures how fast FFmpeg's software decoders decode H.265 (and
H.264, for comparison) on the Vita's CPU, to see which picture sizes could play without the transcoding server.

1. `export VITASDK=...` then `./build_ffmpeg_hevc.sh` (FFmpeg 7.1, H.265/H.264 decoders, threads, NEON;
   into `../../third_party/ffmpeg-hevc`). If it does not build: `ASM=0 ./build_ffmpeg_hevc.sh`.
2. `cmake -B build && cmake --build build` in this folder: `build/HevcBench.vpk` (title ID VIPTVHEV1).
3. Copy the test files (`./make_files.sh` on a PC, or your own raw `.hevc` / `.h265` / `.h264` streams, at most
   48 MB each) to `ux0:data/hevcbench/` and start *H.265 Test*. The results are on the screen and in
   `ux0:data/hevcbench/result.txt`.

Own files: `ffmpeg -i film.mkv -map 0:v:0 -c copy -t 30 -bsf:v hevc_mp4toannexb film.hevc` (H.264:
`-bsf:v h264_mp4toannexb film.h264`).

`ux0:data/hevcbench/config.txt` (optional): `threads=4` tests up to 4 decoder threads; `cores=4` lets them use the
4th CPU core too (only with a plugin that unlocks it).

Each file runs with 1, 2 and 3 frame threads and once with the loop filters left out ("fast"), 150 pictures or
25 s each; only the decoding is timed. "REAL TIME" means at least 25% above the stream's frame rate.
