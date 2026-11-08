# Hardware test (decoder + GPU)

A small separate test app (not part of Vita IPTV).

- Part A: pictures per second from the Vita's hardware H.264 decoder (public and HD decoder, set up as
  Vita IPTV does) for 720p/1080p files at 25/50/60 fps: every picture, every second picture ("even"),
  reference pictures only. Only the decoder is timed.
- Part B: GPU speed of H.265-decoder-like pixel filters (8-tap motion compensation, deblocking-like, SAO),
  bit-exactness against the CPU, and the cost of CPU <-> GPU round trips. Shaders are compiled on the
  Vita with ur0:data/libshacccg.suprx (part B is skipped without it).
- Part C (v2/v3, runs by default): two HD decoders at the same time from one library set up for two streams.
  Result on the device: they share the same ~320 000 macroblocks a second (1080p 20.5 + 20.5 = 41 pictures/s),
  so the limit is the hardware's, not per stream. Put `all` in ux0:data/hwbench/config.txt to run A and B too.

```
./make_files.sh                         # test files (needs ffmpeg with libx264) -> hwbench_files/
cmake -B build && cmake --build build   # -> build/HwBench.vpk
```
On the Vita: copy the .h264 files to ux0:data/hwbench/, run "HW Test" (about 6-8 minutes),
results in ux0:data/hwbench/result.txt.
