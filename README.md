<p align="center"><img src="docs/logo.png" width="128" alt="Vita IPTV logo"></p>

# Vita IPTV

A native IPTV player for the PlayStation Vita and PS TV (HENkaku / Ensō). Watch live TV, films and series and
listen to radio from your own M3U playlists or Xtream accounts, decoded by the Vita's hardware H.264 decoder.

<p align="center"><img src="https://github.com/user-attachments/assets/010c908b-58ff-4be5-982d-5e6c4701ef0c" width="640" alt="Vita IPTV screenshot"></p>

**Version 1.2** - downloads to watch without the internet, Internet Archive and web page playlists, videos on the memory card, 5.1 AAC on the Vita's own decoder, Opus / Vorbis sound, themes with a black OLED background, plus live TV with a programme guide, films and series, subtitles and favourites (see [CHANGELOG.md](CHANGELOG.md)). It is a hobby project, provided as is, without support or a roadmap.

This app was developed with the help of AI (Anthropic's Claude), and tested on a real PS Vita.

> Vita IPTV is only a player. It does not include, sell or point to any channels or playlists.
> Use it with playlists and services you are allowed to watch, and download only what you have the right to keep;
> you are responsible for what you play and download with it.

## Features

- **Playlists:** M3U / M3U8 links, Xtream Codes accounts (server + username + password), single stream links,
  and playlist files copied to the memory card.
- **Add playlists inside the app** with the on-screen keyboard. Pasting an Xtream `get.php?username=...&password=...`
  link fills in the server, user and password by itself.
- **Streams:** MPEG-TS over HTTP, **HLS (.m3u8)** live and on-demand, **HTTP-FLV**, MKV, MP4.
- **Films and series (Xtream):** opening an Xtream playlist asks for *Live TV*, *Films* or *Series*. Films and
  episodes (MKV, MP4 and TS files) can be paused and jumped through, and the app remembers where you stopped
  (*Continue from 41:10* the next time).
- **Series:** watched episodes get a tick, and opening a series selects the episode left half-watched (or the one
  after the last finished one). When an episode ends, *Next episode in 5 s* shows in the corner (X: now, O: stay).
- **Film and series details:** choosing a film shows its poster, year, length, rating, genre and story beside the
  list; **Triangle** opens the full details (cast, director, *Continue* / *From the start* / *Favourite*). A series
  opens on its own screen with the seasons as tabs. **Square** searches every film or series of the account.
- **Subtitles (MKV, MP4):** SRT and ASS / SSA text subtitles of MKV films and `mov_text` subtitles of MP4 films,
  chosen in **Select > Subtitles**, with a sync setting (0.1 s steps) and their size, colour, place and background.
  Picture subtitles (PGS, VobSub) and subtitles in TS files are not shown.
- **HTTPS** with a built-in TLS stack (mbedTLS, TLS 1.2 / 1.3), so modern https playlists, short links
  (tinyurl and the like) and https/HLS channels work. The Vita's own TLS is too old for most sites today.
- **Video:** H.264 up to 1920x1080 through the hardware decoder (details below), audio/video sync to the
  sound, automatic recovery from decoder errors. MPEG-2, MPEG-4 Part 2 (DivX / Xvid) and DivX 3 are decoded in
  software (on three cores above SD; interlaced rows are mixed on the screen).
  When a picture bigger than the screen falls behind the sound, it is decoded at half size from the next keyframe
  on (the Vita's screen is 960x544, so it looks the same). With the 4th CPU core opened by a plugin, the reader of
  such streams and of HTTPS streams runs there; without it nothing changes.
- **1080p:** 1080p channels and films play through the same hardware decoder.
- **50 / 60 fps:** 720p at 50 / 60 fps plays every picture; 1080p at 50 / 60 fps a steady 25 / 30 pictures a
  second (every second picture, evenly spaced).
- **Sound (Select > Audio):** volume boost (+3 / +6 / +9 dB, with a limiter), "clear sound" for the Vita's small
  speakers, "clear speech" for 5.1 films (the centre channel louder in the stereo mix), the same volume on every
  channel, and the sound field: wider stereo on the speakers, or crossfeed and virtual surround (5.1) on headphones.
- **Picture size (HLS channels with several qualities):** they open at about 540p (the Vita's own screen size) if
  the channel offers it, else at 720p, or at the size chosen in the settings. **Select** lists the channel's
  sizes (tab *Picture*); **Square** goes to the next size up, from the biggest to the smallest.
- **Stream analysis:** shows the resolution, codecs, frame rate and download speed of the stream (TS, HLS, MP4, MKV,
  http and https) and whether the Vita can play it (**Select** > *More* while watching).
- **Audio:** AAC (stereo on the hardware; 5.1 / 7.1 AAC in software), MP2 / MP3, AC-3 / E-AC-3 (Dolby Digital /
  Dolby Digital Plus, software), DTS (its core, software) and Opus / Vorbis (in MKV / WebM, software). 5.1 and 7.1 come out as stereo. Many TV channels use MP2, many films E-AC-3.
- **Audio tracks and subtitles, remembered per stream:** a channel or film with more than one sound lists them in
  **Select** > *Audio* with their language (*Audio: Turkish (AAC)*). What you choose there (and in *Subtitles*) is
  kept for that channel or film only - for a series, for all of its episodes - and comes back the next time.
  Without a choice the sound in the app's language plays (else the stream's own), and subtitles in the app's
  language are shown only when the sound is not in it. HLS channels whose sound comes in playlists of its own
  (one per language) play too, and list those languages.
- **Radio:** audio-only streams get a radio screen.
- **Favourites:** START marks a channel, a film or a series (star). A favourite channel stays in its
  place in its category and gets its own *Favorites* category, where **Select** > *Reorder favourites* reorders them (X
  picks a channel up and puts it down, any number of them; START saves; O asks whether to save). Films
  and series (Xtream) have a *Favorites* category before the categories; it opens at once, without loading.
- **My List:** **Select** > *Add to My List* on a live channel of any playlist; a *My List* row at the top of the
  playlists then holds them together, grouped by playlist (Xtream channels with their programme guide, opened
  with the playlist's current password). Also while watching (**Select** > *More*); *Reorder My List* reorders it.
- **Fast list opening:** Xtream and M3U-link playlists open at once from the copy saved
  last time; the list is checked in the background and replaced at a quiet moment when it changed.
- **Recently watched:** a category with the last 20 channels of the playlist; films and series (Xtream) have one
  too, before their categories (the last 20 films, the last 20 series an episode of which was played).
  *Clear Recently Watched* in the settings empties them all.
- **Programme guide:** **Triangle** in a live channel list or while a channel plays opens a grid of channels and their
  programmes along a time line (in the player the picture keeps playing in the corner). Xtream accounts give it by
  themselves; M3U lists that name an XMLTV guide in their header (`url-tvg`, `.xml` or `.xml.gz`) get it too
  (M3U lists without one have no guide). The
  guide is read while nothing plays and kept for 12 hours. The programme on now is shown in the list and the player.
- **Watching again (catch-up):** channels with an archive (Xtream `tv_archive`, M3U `catchup=`) get a rewind clock;
  a past programme plays from its start, picked in the guide or in the channel's day-by-day schedule (**Square** in
  the guide). Up / Down goes to the programme before / after, **Square** back to live.
- **Sleep timer** and **channel number** (**Select** > *More* while watching; digits typed in a live list's search
  go to that channel too).
- **Parental lock:** a 4-6 digit PIN (*Settings*); adult categories and channels then ask for it, and it stays
  open until the console sleeps. Changing or removing the PIN, or turning *Hide adult channels* off, always asks
  for the current one. **Forgot the PIN?** Delete the `pin=` line in `ux0:data/VitaIPTV/settings.txt` (with
  VitaShell, for example); the lock is then off.
- **Two-pane channel browser:** categories on the left, channels on the right. The left stick scrolls the list in
  use and opens (right) or leaves (left) a category. Search across all channels (case- and accent-insensitive).
- **Settings** (**Select** on the playlists or the channels; the line at the bottom says what the chosen one does):
  language (English / Turkish, follows the console by default), colour (blue, green, purple, orange, teal, pink), background (navy, black for OLED, graphite, warm dark), preferred
  resolution (automatic, 1080p, 720p, 540p, 480p, lowest), hide adult channels, hide channels named 4K / UHD (the
  Vita cannot play 4K), parental lock (PIN), volume boost, clear sound, clear speech (5.1), the transcoding server
  (off / when needed / always; its address and a test while it is on), download the next episode (off / 1 / 2:
  when 40 percent of an episode is watched), the touch screen in the player (off by default; not on a Vita TV),
  storage (what the app keeps on the memory card, each part cleared on its own or all at once), clear recently
  watched, clear watched episodes, About. O with something changed asks whether to save.
- **Touch screen in the player** (when turned on): a touch shows or hides the bar, dragging the time line jumps
  (where it is let go), two touches pause. Lists and menus stay on the buttons.
- **Playlists' order:** **Select** > *Reorder playlists* on the playlists screen (as for the favourites); My List
  stays on top while it has channels. **O** there asks "Quit Vita IPTV?".
- **Player top bar:** the channel or film, the time of day in the middle, the picture size on the right.
- **Downloads (watch without the internet):** **Select** > *Download* on a film or an episode (or *Download this
  season*), or on a single film link of the playlists. A *Downloads* row is among the playlists while there is a
  download (it can be moved like them); it also continues a film where it was left online, and the other way round; it opens without a connection, with the films and
  each series (by season and episode), the poster and the story, and plays them like any film (jumps, *Continue
  from*, audio tracks, subtitles inside the file). **Triangle** deletes a film, an episode or a whole series. Files
  go to `ux0:data/VitaIPTV/downloads/` (one folder each: the video as the server sends it, its poster and an
  `info.txt`). A film the server gives as HLS (a playlist of TS parts) is saved as one `video.ts`; HLS with MP4
  parts, live streams and DRM cannot be downloaded. Downloads run one at a time while the app is open; a stopped one goes on where it stopped (Square), and
  the free space is checked first (200 MB are always left free). The screen may dim, but sleep or the PS button
  stops a download. A film or an episode that is downloaded plays from the memory card when it is opened in its
  list too (a sign beside it in the lists). While a channel or a film from the same server plays, the download
  waits and goes on afterwards (an account with one connection cannot do both). A download the server refuses
  stays in the list with the reason (Square tries again). A note at the top says when a download has ended.
  **Only for content you have the right to download**: whether you may keep a copy depends on your provider's
  terms and on where the content legally comes from.
  *Select > Black screen while it downloads* on the Downloads screen keeps the screen black (any button brings it
  back): less power on the first Vita's OLED screen during a long download.
  The waiting ones show their place in the line ("Waiting (2)"); *Select > Download it next / Move it up one /
  Download it last* changes it. Films and episodes watched to their end (from the Downloads, or an episode online)
  say "Watched"; *Select > Delete the watched episodes of this series* or *Delete all watched downloads* frees the
  room (it asks first).
- **Internet Archive (archive.org):** add a playlist of the type *Internet Archive* with an archive.org address (a
  collection like `https://archive.org/details/feature_films`, a film, or `https://archive.org/search?query=...`)
  or just words (`charlie chaplin`). Its films come as a list (500 at a time, with poster, year and story;
  *Next 500...* at the end adds more); when one is opened the app picks the file the Vita plays (MP4 / H.264 up
  to 1080p) and remembers it. **Square** searches the film titles (inside the list, or Select > *Search the whole
  archive*), **Select > Order** sorts it (most watched, newest, year, title). Old, free films: silent films,
  classics, documentaries.
- **Web page:** a playlist of the type *Web page* lists the videos written in a page (`<video>`, the page's video
  data, `.mp4` / `.m3u8` addresses in it, links to video files, one step into the pages shown inside it).
  JavaScript does not run on the Vita: a site that makes its video only then gives nothing, and the app says why
  (also: DRM, DASH, H.265, WebM / Ogg). A page to try it with:
  `https://www.w3schools.com/html/html5_video.asp`. `ux0:data/VitaIPTV/webcheck.txt` lists what the last page gave. **Only for pages whose videos
  you may watch this way.**
- **Search:** Square shows the last five searches first (and *New search...*).
- **Optional transcoding server** (`tools/vita_iptv_proxy.py`, needs Python 3 and ffmpeg on a PC or an Android
  phone): plays what the Vita cannot - HEVC, interlaced video, 1080p at 50/60 fps - by converting it to 720p
  H.264 + AAC. **Select** while watching moves the channel or film to the server (*Through the Server*) and back.

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
| Single stream | A direct stream link; Referer and User-Agent only if the stream needs them |

**Triangle** edits (*Delete playlist* is at the end of the edit form), **START** saves a form, **Select** opens the
menu (Add playlist, Reorder playlists, Reload playlists, Settings, Quit).

**From a PC (many at once):**

- Copy `.m3u` / `.m3u8` files into `ux0:data/VitaIPTV/` - they appear in the list automatically.
- Or edit `ux0:data/VitaIPTV/sources.txt`, one playlist per line:

```
# name           | type   | address                          | user | password
My playlist      | m3u    | https://example.com/list.m3u
Locked playlist  | m3u    | http://example.com/list.m3u      | user | pass
My provider      | xtream | http://example.com:8080          | user | pass
One channel      | stream | http://example.com/live/1.ts
With headers     | stream | https://cdn.example.com/live.flv | https://example.com/page | Mozilla/5.0 ...
```

- **Videos on the Vita:** a row of the playlists that opens the memory cards folder by folder (the VitaIPTV
  folder, the Video app's folder, `ux0:`, `uma0:`, `imc0:`...). MKV, MP4, TS, AVI, FLV, MPG / VOB and Video CD (`.dat`, the `MPEGAV` folder of a copied VCD) files play as films (pause,
  jumps, *Continue from*, audio tracks, subtitles inside MKV / MP4), also from M3U lists (`ux0:...` lines).
- **Subtitle files (.srt):** a file beside the film with the same name (`Film.srt`, `Film.tr.srt`,
  `Film.eng.srt`) is offered in **Select** > *Subtitles* as "SRT" (UTF-8 or Turkish Windows-1254; the language from
  the name or the text). A download takes the `.srt` the server has beside the film, if there is one.

**Streams that need a Referer or User-Agent:** M3U lists can give them with `#EXTVLCOPT:http-referrer=...` and
`#EXTVLCOPT:http-user-agent=...` lines before the link (as VLC reads them), with `#EXTHTTP:{"Referer":"..."}`, or
Kodi style as `link|User-Agent=...&Referer=...`. The app sends them with every request of that channel.

## What plays and what does not

| Works | Does not work (without the transcoding server) |
|---|---|
| H.264 up to 1920x1080 (see below), AAC, MP2, MP3 | Above 1080p (4K) |
| MPEG-2, MPEG-4 Part 2 (DivX / Xvid), DivX 3: in software (above the screen's size at half size when behind) | DivX 3 above SD may stutter |
| MPEG-TS, HLS, FLV, MKV, MP4, AVI, MPEG-PS (.mpg, .vob), Video CD (.dat) | HEVC / H.265 |
| Films: MKV, MP4, TS, AVI, MPG / VOB, VCD .dat, FLV files (FLV: from the start, no jumping) | WMV, RealVideo |
| AC-3 / E-AC-3, DTS, 5.1 / 7.1 AAC, Opus, Vorbis audio (as stereo) | Interlaced broadcasts (576i, 1080i may stutter) |
| http and https | TrueHD audio (DTS-HD: its DTS core plays) |
| AES-128 HLS, HLS with fMP4 / CMAF segments | SAMPLE-AES / DRM |
| Subtitles: SRT / ASS text, PGS pictures (MKV) | VobSub (DVD picture subtitles) |

**About "720p":** through the public API, the Vita's hardware decoder is limited to roughly H.264 level 3.1:
8-bit 4:2:0, progressive, at most 3600 macroblocks per picture (1280x720) and a limited number of reference
pictures - up to 5 at 720p, more at smaller sizes. Most 720p channels fit; a 720p channel encoded with more
reference pictures may play with dropped frames or not at all, while the same channel in SD plays fine.
When the decoder gives no picture from the start, the player tries once more with the H.264 header adjusted for
the Vita (*bitstream_restriction* added, timing information taken out). Some channels still give no picture; the
transcoding server plays them.

**About 1080p:** the public decoder API stops at 720p. Above 720p the app sets the decoder up through the
firmware's internal entry points instead (the same hardware; found with the test app in `tools/decprobe`).
1080p needs about twice the data of 720p. When the connection cannot keep up, live TV waits with *Buffering...*
instead of freezing; when the stream's segments come slower than they play, the player switches to the channel's
next smaller picture size by itself (not when a size was chosen with Select / Square or in the settings, except
1080p). A film the connection cannot carry waits longer at once and
shows the connection speed against what the film needs.
The Vita decoder manages about 320 000 macroblocks a second (1080p up to about 40 pictures a second, 720p about
90; measured with the test app in `tools/hwbench`). 1080p at 50/60 fps plays every second picture, evenly spaced
(the pictures no other picture needs at the other places are left out; in time with the sound); an HLS channel goes to its 720p
variant instead. The same happens when the decoder is measured to be slower than any other stream.
When the picture falls behind the sound on live TV (a stream that sends the sound seconds ahead, a burst the
decoder takes in at once), the sound waits until the picture has caught up.
R while watching shows three rows in fixed columns: picture size, shown / stream frame rate (e.g.
43 / 50 fps), pictures shown and dropped; sound, A/V offset, waits, damaged pictures; connection speed, what the
stream needs, data received, pictures not decoded.

**About films:** most films are 1080p and play directly. Many film files use
HEVC (x265) video or AC-3 / E-AC-3 sound: HEVC does not play (the server plays it), AC-3 and E-AC-3 play as
stereo. Jumping needs a server that
sends parts of files (HTTP ranges); Xtream servers do. The film's index is read once and kept while the film is
open, so jumps start quickly. When the connection is slower than the film needs, the
film stops with *Buffering...* until a few seconds are loaded (longer each time it happens again), instead of
freezing; if that happens often, the
provider sends too slowly for that file (a smaller version of the same film usually plays fine).

If a channel does not open, the screen tells you why (also when the Vita has no Wi-Fi connection), and
`ux0:data/VitaIPTV/log.txt` has the details.

**HTTP 407 / 403** comes from the IPTV provider, not from the Vita: usually the account is already in use
elsewhere or the channel is not in your package.

## The transcoding server

`tools/vita_iptv_proxy.py` converts a channel the Vita cannot play into one it can. It runs on any computer in
the same Wi-Fi network as the Vita (Windows, macOS, Linux, Raspberry Pi) with Python 3 and ffmpeg:

```
python3 vita_iptv_proxy.py            # prints the address to enter on the Vita, e.g. http://192.168.1.20:8090
```

On the Vita: **Select** > *Settings* > *Transcoding Server* = that address, *Test Server*, *Save*. *Use
Automatically*: *When Needed* sends the channels the Vita cannot play through the server, *Always* every channel.
While watching, **Select** > *Through the Server* / *Direct* switches the stream you are on.

**On an Android phone (Termux):**

1. Install [Termux](https://f-droid.org/packages/com.termux/) from F-Droid (the Play Store version is old).
2. In Termux: `pkg update && pkg install python ffmpeg`
3. Copy `vita_iptv_proxy.py` to the phone, e.g. into Download, then `termux-setup-storage` and
   `cp ~/storage/downloads/vita_iptv_proxy.py ~/`
4. Start it: `python ~/vita_iptv_proxy.py --preset ultrafast`
5. Keep Termux open (the notification's *Acquire wakelock* stops Android from pausing it) and enter the address it
   prints on the Vita as above. The phone and the Vita must be in the same Wi-Fi network.

Converting takes a lot of processor time: a phone or a small computer manages one channel at a time, a PC easily.

## Trying films and series without a provider

`tools/xtream_test_server.py` (Python 3, nothing to install) serves a folder of your own videos as an Xtream
account: files at the top are films, sub-folders are film categories, `Series/<name>/S01E01 ....mkv` are series and
`Live/*.ts` live channels. Run it on a PC in the same network and add the address, username and password it prints
as an Xtream playlist. Film links (`.mkv`, `.mp4`, `.mov` ...) in M3U lists and single stream links also play as
films (pause, jumps, resume), so a freely licensed film such as the Blender Foundation's open movies works too.
`--pace` sends live channels at their own bit rate like a broadcast, `--https` serves over HTTPS with a
self-signed certificate (needs `openssl`), for comparing the same file over HTTP and HTTPS.

## License

Vita IPTV is released under the [MIT License](LICENSE), provided as is, without any warranty. Bundled third-party
code keeps its own license (mbedTLS: Apache-2.0, minimp3: CC0, FFmpeg's audio decoders: LGPL 2.1+, vita2d and its
libraries); the notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which is also inside the VPK.

"PlayStation" and "PS Vita" are trademarks of Sony Interactive Entertainment. This is an independent homebrew
project, not affiliated with or endorsed by Sony.

## Credits

- [vita2d](https://github.com/xerpi/libvita2d) - drawing
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0) - TLS, in `third_party/mbedtls`
- [minimp3](https://github.com/lieff/minimp3) (CC0) - MP2 / MP3 decoding
- [FFmpeg](https://ffmpeg.org) (LGPL 2.1+) - AC-3 / E-AC-3, DTS, AAC 5.1, Opus, Vorbis, MPEG-2 and MPEG-4 Part 2 decoding
- The VitaSDK and HENkaku / Ensō teams

Inspired by ViTube (the PS Vita YouTube app) - its source showed that the Vita's internal decoder
entry points can play 1080p - and by [wiliwili](https://github.com/xfangfang/wiliwili). No code was copied from
either; Vita IPTV's player is its own.
