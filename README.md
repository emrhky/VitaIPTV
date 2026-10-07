<p align="center"><img src="docs/logo.png" width="128" alt="Vita IPTV logo"></p>

# Vita IPTV

A native IPTV player for the PlayStation Vita and PS TV (HENkaku / Ensō). Watch live TV and listen to radio
from your own M3U playlists or Xtream accounts, decoded by the Vita's hardware H.264 decoder.

<p align="center"><img src="https://github.com/user-attachments/assets/010c908b-58ff-4be5-982d-5e6c4701ef0c" width="640" alt="Vita IPTV screenshot"></p>

**Version 0.1** - first public release. It is a hobby project, provided as is, without support or a roadmap.

This app was developed with the help of AI (Anthropic's Claude), and tested on a real PS Vita.

> Vita IPTV is only a player. It does not include, sell or point to any channels or playlists.
> Use it with playlists and services you are allowed to watch.

## Features

- **Playlists:** M3U / M3U8 links, Xtream Codes accounts (server + username + password), single stream links,
  and playlist files copied to the memory card.
- **Add playlists inside the app** with the on-screen keyboard. Pasting an Xtream `get.php?username=...&password=...`
  link fills in the server, user and password by itself.
- **Streams:** MPEG-TS over HTTP, **HLS (.m3u8)** live and on-demand, MKV, MP4.
- **HTTPS** with a built-in TLS stack (mbedTLS, TLS 1.2 / 1.3), so modern https playlists, short links
  (tinyurl and the like) and https/HLS channels work. The Vita's own TLS is too old for most sites today.
- **Video:** H.264 up to 1280x720 through the hardware decoder, audio/video sync to the sound, automatic
  recovery from decoder errors.
- **Audio:** AAC (hardware), MP2 / MP3 (software). Many TV channels use MP2.
- **Radio:** audio-only streams get a radio screen; START turns the screen off while the music keeps playing.
- **Two-pane channel browser:** categories on the left, channels on the right. Left stick scrolls categories,
  right stick scrolls channels. Search across all channels (case- and accent-insensitive).
- **Settings:** language (English / Turkish, follows the console by default), hide adult channels,
  hide 1080p / FHD / 4K channels (the Vita cannot decode them), optional transcoding server.
- **Optional transcoding server** (`tools/vita_iptv_proxy.py`, needs Python 3 and ffmpeg on a PC):
  plays what the Vita cannot - 1080p, HEVC, interlaced SD, AC-3 - by converting it to 720p H.264 + AAC.

## Install

1. Download `VitaIPTV.vpk` from the [Releases](../../releases) page.
2. Copy it to the Vita (e.g. with VitaShell over USB or FTP) and install it.
3. Start Vita IPTV. Its data folder is `ux0:data/VitaIPTV/`.

## Adding channels

**In the app:** on the Playlists screen press **Square** (Add) and choose a type:

| Type | What to enter |
|---|---|
| Xtream | Server address (e.g. `http://example.com:8080`), username, password - or paste a full `get.php` link |
| M3U playlist | The playlist link (`http://` or `https://`, short links work); username/password only if the server asks |
| Single stream | A direct stream link |

**Triangle** edits, **Select** deletes, **START** opens the menu (Settings, Reload, About, Exit).

**From a PC (many at once):**

- Copy `.m3u` / `.m3u8` files into `ux0:data/VitaIPTV/` - they appear in the list automatically.
- Or edit `ux0:data/VitaIPTV/sources.txt`, one playlist per line:

```
# name           | type   | address                          | user | password
My playlist      | m3u    | https://example.com/list.m3u
Locked playlist  | m3u    | http://example.com/list.m3u      | user | pass
My provider      | xtream | http://example.com:8080          | user | pass
One channel      | stream | http://example.com/live/1.ts
```

- Video files (`.ts`, `.mp4`, `.mkv`) in the same folder are listed as local sources.

## Controls

| Screen | Controls |
|---|---|
| Playlists | X open, Square add, Triangle edit, Select delete, START menu |
| Channels | Left stick / D-pad: categories, right stick: channels, X play, L/R page, Square search, O back |
| Playing | Up/Down change channel, X stream info, Triangle stream analysis, O back |
| Radio | Up/Down change station, START screen off, O back |

## What plays and what does not

| Works | Does not work (without the transcoding server) |
|---|---|
| H.264 up to 1280x720, AAC, MP2, MP3 | 1080p / 1080i and above (hardware limit) |
| MPEG-TS, HLS (MPEG-TS segments), MKV, MP4 | HEVC / H.265 |
| http and https | Interlaced SD broadcasts (576i), AC-3 / E-AC-3 audio |
| | Encrypted (AES) HLS, HLS with fMP4 segments |

If a channel does not open, the screen tells you why, and `ux0:data/VitaIPTV/log.txt` has the details.

**HTTP 407 / 403** comes from the IPTV provider, not from the Vita: usually the account is already in use
elsewhere or the channel is not in your package.

## Building

Needs [VitaSDK](https://vitasdk.org) (with `vita2d`, installed by `vdpm`'s `install-all.sh`).

```
export VITASDK=/usr/local/vitasdk
cmake -B build && cmake --build build      # -> build/VitaIPTV.vpk
```

`-DVITAIPTV_HTTPS=OFF` builds without the built-in TLS. PC tests are in `tests/` (gcc, ffmpeg, Python 3),
e.g. `python3 tests/run_player_tests.py`.

## Credits

- [vita2d](https://github.com/xerpi/libvita2d) - drawing
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0) - TLS, in `third_party/mbedtls`
- [minimp3](https://github.com/lieff/minimp3) (CC0) - MP2 / MP3 decoding
- The VitaSDK and HENkaku teams
