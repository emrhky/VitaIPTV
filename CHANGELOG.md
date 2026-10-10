# Changelog

## 0.2

- **1080p** is always on (no longer a setting): 1080p channels and films play through the hardware decoder.
  HLS channels with several qualities still open in 720p; Square switches to 1080p.
- **Films and series for Xtream accounts:** opening an Xtream playlist asks for Live TV, Films or Series,
  with categories, seasons and episodes.
- **Film playback:** MKV, MP4 and TS files, pause, moving through the film with L / R, the D-pad or the sticks,
  and resume (*Continue from 41:10*). Film links in M3U lists and single stream links play as films too.
- **Same controls for live TV and films:** X / START pause, Triangle information, Square 720p / 1080p,
  Select stream analysis, Up / Down previous / next, O back. Live TV can be paused for a short while.
- **Stream analysis** (Select) now works for HLS, https and MP4 / MKV films, and shows the download speed.
- Bigger buffers and recovery when an HLS stream stalls; clearer messages for videos the decoder refuses.
- Fixed a crash (GPU) after a playlist failed to load.
- `tools/xtream_test_server.py`: serve a folder of your own videos as an Xtream account to try films and series.
- The transcoding server can start films at a position.

## 0.1

- First public release: M3U / Xtream / single links, MPEG-TS and HLS, HTTPS (mbedTLS), radio, search, English / Turkish.
