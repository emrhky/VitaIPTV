# Changelog

## 1.0.0

0.7.0, 0.8.0 and 0.8.1 were not released on their own: their changes (below) come with 1.0. This release itself
is about staying stable:

- **Your files are saved safely.** The playlists (`sources.txt`), favourites, settings, where films stopped and
  the watched episodes are first written to a new file that takes the old one's place only when it is complete
  (the one before is kept as `.bak`). A crash or an empty battery while saving can no longer leave an empty
  `sources.txt`; the app takes the complete file that is left.
- **A channel that drops after hours keeps going.** When the server refuses a reconnection with *407* (a
  one-connection account it still counts as busy with the lost connection), the player asks again after 3, 6 and
  10 seconds instead of stopping with an error (seen after 2 hours of one channel).
- **The TS clock wrapping** (every 26.5 hours, at any moment of a live channel): the picture and the sound wrap
  a moment apart, and for that moment their difference was taken as 26 hours: the picture ran on its own clock,
  the pictures around the wrap were put in the wrong order (a few were lost) and the information table showed a
  huge A/V value. Times are now compared across the wrap; a test plays through it, from a file and as a live
  stream, without losing a picture.
- Unused translations left out, small code clean-ups, the third-party notices name what the posters and the
  compressed guides use.

## 0.8.1

**Parental lock (PIN)**
- Changing or removing the PIN always asks for the current one first, even when it was given earlier for an adult
  channel. Turning *Hide adult channels* off asks for it every time too. Giving it there does not open adult content.
- A new PIN is typed twice; a PIN that is too short, or two that differ, leave the old one as it was (a too short
  one used to remove the lock).
- The lock is on as soon as the settings are saved (it used to stay open until the app was closed).
- Once given for adult content, the lock closes again when the console sleeps (it used to stay open until the app
  was closed, which on the Vita can be days).
- A forgotten PIN: delete the `pin=` line in `ux0:data/VitaIPTV/settings.txt` (README).

**Subtitles**
- **Select > Subtitles** has four more rows (Left / Right or X changes them, seen at once, kept for every film):
  *Size* (small, medium, large, very large), *Colour* (white, yellow, green, blue), *Place* (bottom, a little
  higher, top) and *Background* (light band, dark box, none).

**Theme**
- *Settings > Theme*: blue (as before), green, purple or orange - the accent, the selection and the text colours.
  It changes at once while choosing; *Cancel* puts it back.

**Programme guide**
- An M3U list without a guide address no longer opens an empty guide: Triangle is not offered in its channel list,
  and in the player it says *No programme guide for this channel*.
- In a guide, a channel with no programmes at all says *No programme guide* in its row (one with programmes on
  other days: *No programme information*).

**Log**
- While playing, the statistics lines come every 5 s only in the first 30 s or when pictures are lost; after that
  one pair a minute (the log grew by about 500 KB an hour of watching, now by about 35 KB).
- The log has a limit: past 1 MB the statistics lines stop, past 3 MB nothing more is written until the app starts
  again (the app may run for days: the console sleeps, the app is not closed).
- Old diagnostic lines left out: the first frames of the channel screen, the 12 sample names with their bytes, the
  clock settings each time (now once each way and when one fails).

**Other**
- A film's details over the picture show only *O Close* in the button bar (Triangle still closes them).

## 0.8.0

**Programme guide**
- **Triangle in a live channel list** opens the guide: channels down the side, their programmes along a time line
  (2.5 hours across), a red line at the time now. Left / Right moves through the time, L / R a day back / on,
  Select opens the categories. The selected programme is shown below with its description and how long it has left.
- **Triangle while a channel plays** opens the same guide with the picture still playing small in the corner;
  Up / Down changes channel there, X switches to the selected one.
- **Xtream:** the programmes of a channel are asked for when it comes into view (only those, a few at a time).
- **M3U:** the guide address in the list's header (`url-tvg`, `x-tvg-url`) is read (XMLTV, also `.gz`), matched to
  the channels by `tvg-id`, else by name. It is read in pieces as it arrives, only while nothing plays (a channel
  started stops it, it goes on later), with a memory limit of 6 MB (the rest is left out), and kept for 12 hours
  in `ux0:data/VitaIPTV/epg_*.bin`, so the next start does not read it again.
- The channel rows of the list show what is on now.

**Watching again (catch-up)**
- Channels with an archive get a rewind clock in the list and the guide. A past programme of such a channel asks
  *Watch from the start (archive)* or *Watch live*.
- **Square in the guide** opens the channel's schedule: day tabs (as far back as the archive goes), its
  programmes, X plays one from the start.
- Playing from the archive: the top bar says *ARCHIVE* and the programme; the time line shows clock times; Left /
  Right moves through it (not past now), Up / Down goes to the programme before / after, Square goes back live,
  Triangle opens the schedule, O goes back to the guide or schedule it came from. When it ends, the next programme
  starts after 5 s (X: now, O: stay).
- Xtream archives (`tv_archive`) and M3U `catchup` (`default`, `append`, `shift`, `flussonic`, `xc`, with
  `catchup-source` / `catchup-days`) work. The server's time zone is taken from the account, so programmes start
  at the right minute.

**Films and series**
- **Choosing a film in the list** moves the categories aside and shows its card on the right: poster, year,
  length, rating, genre, director and the start of the story (from the provider, asked for when a film stays
  selected).
- **Triangle on a film** opens its details: a big poster, the whole story, the cast, and buttons *Continue (time)*,
  *From the start*, *Favourite*. Up / Down goes to the film before / after. While a film plays, Triangle shows
  the details over the picture.
- **A series** opens on a screen of its own: poster and story on top, its seasons as tabs (L / R), the episodes
  below with their ticks and where an unfinished one was left. The selection opens on the right season.
- Posters are decoded straight to a small picture (one at a time, about 350 KB), so big pictures do not fill the memory.
- **Square in films / series** searches every film or series of the account (not only the open category).

**Subtitles**
- MP4 films and episodes show their text subtitles too (`mov_text` / `tx3g`), with the language from the file.
- Track names are cleaned up: site names and repeated language words go, *SDH* and *Forced* stay.

**Player and lists**
- **R while watching** shows the information table (was Triangle).
- **Favourites:** in the live *Favorites* category **Select** offers *Reorder favourites*; Up / Down moves the
  channel, X saves, O cancels (was Triangle).
- **Sleep timer** (Select > More): 15 min to 2 hours; a minute before it stops a note shows in the corner
  (X: 15 more minutes), then the playback stops.
- **Channel number** (Select > More, or digits typed in the search of a live list): goes straight to that channel.
- **Parental lock:** *Settings > Parental lock (PIN)* sets a 4-6 digit PIN. With it set, turning *Hide adult
  channels* off and opening adult categories, channels, films or their guide asks for it (once per start).

**Log**
- Choosing the episode to open in a series (watched / half-watched) is written to the log.

## 0.7.0

0.6.0 was not released on its own: its changes (below, from *Audio* on) come with 0.7.0.

**Subtitles (MKV)**
- Text subtitles of MKV films and episodes are shown: SRT (`S_TEXT/UTF8`) and ASS / SSA tracks, also the ones
  mkvmerge packs with zlib or header stripping. ASS styling is left out (the text is shown plain, `\N` makes a new
  line). Picture subtitles (PGS, VobSub) and subtitles of TS / MP4 files are not shown.
- **Select > Subtitles** lists the tracks (language, *SDH*, *Forced*); the choice works at once and its language is
  chosen for the next films. *Off* stays off for the next films too.
- **Sync:** Left / Right moves the subtitles 0.1 s earlier / later (held: it keeps moving; up to 60 s), X sets it back to 0.

**Player**
- The **Select menu** is in tabs (*Picture*, *Audio*, *Subtitles*, *More*): L / R or Left / Right changes the tab;
  the long list of picture sizes, sounds and the analysis is gone.
- The **time of day** is in the middle of the top bar while the information is shown (24 or 12 hours as the
  console is set).
- **Next episode:** when an episode ends, *Next episode in 5 s* shows in the corner; X plays it at once, O stays on
  the ended episode. It starts where it was left if it was started before.

**Series**
- **Watched episodes** get a tick (watched to the end, or into its last 5% / last minute). Opening a series puts the
  selection on the episode left half-watched, or on the one after the last episode watched to its end.
  Kept per playlist and series in `watched.txt` (300 series).
- **Settings > Clear watched episodes** removes the ticks (asks first). Where films stopped is kept;
  *Clear Recently Watched* is separate.

**Log**
- The list's memory: after a list is read the log says how many channels it holds and the app's memory
  (*app memory N KB of 96 MB, peak N KB*); the player's statistics line says it too. The large film queues are
  now checked against the app's own memory (the heap) instead of the system's.


**Audio**
- **5.1 and 7.1 AAC** play (films and channels, TS / MKV / MP4): the Vita's AAC decoder takes only one or two
  channels, so these said *Audio has 6 channels: not supported*. Now FFmpeg's AAC decoder plays them, mixed down to
  stereo like AC-3 (stereo AAC stays on the hardware). The information screen says *5.1 > stereo*. Building it needs
  `tools/build_ffmpeg_vita.sh` again (it adds the AAC decoder; CMake warns when the old build is there).
- **HLS with separate audio playlists** (`#EXT-X-MEDIA:TYPE=AUDIO` with a URI, one playlist per language): the sound
  is fetched from its own playlist beside the picture, TS segments or packed AAC / AC-3 (`.aac` with an ID3 time
  stamp). The languages are listed in the **Select** menu; the choice goes by the language for the next channels.
  Until now such channels played without sound (or with the variant's own one only).

**Favourites and recently watched**
- **Recently watched films and series:** the films and series (Xtream) have a *Recently Watched* category too,
  after *Favorites*: the last 20 films played and the last 20 series an episode of which was played, newest
  first. It opens at once (no loading); films go on where they were left. *Clear Recently Watched* in the
  settings empties it as well.
- **Films and series** can be favourites: START on a film or series (in the episodes: their series; while a film
  plays: the film). They get a star and a *Favorites* category before the categories, which opens at once from
  `favorites.txt` (no loading). A film saved there plays with the playlist's current password.
- A favourite channel **stays in its place** in its own category and in *All*, with its star. Only the *Favorites*
  category gathers them (Triangle still reorders them there).

**Network**
- Plain `http://` films, channels, HLS and lists now go through the app's own connection (as `https://` did), which
  asks for a 1 MB receive buffer; the system HTTP keeps a small one, so with a long round trip the speed stopped
  early. The log shows the buffer the system gave (*own socket ..., receive buffer N KB*) and, for lists, the time.
  When the own connection cannot reach a server at all, the system HTTP takes over as before. The transcoding
  server stays on the system HTTP.
- The network memory is 4 MB (was 1 MB): the socket buffers come from it.
- Radio servers that answer `ICY 200 OK` work through the own connection.
- **Much faster films:** the receive buffer stayed at 63 KB, because the Vita refuses 1 MB (ENOBUFS) and the
  request was then ignored altogether; a connection then manages at most 64 KB per round trip (about 285 KB/s from a
  server 225 ms away). The Vita takes at most 512 KB (528 KB was refused too), so 512 KB is asked for; should it be
  refused (another firmware, or the network's memory short for a moment), 256 and 128 KB are tried. On the
  device, films went from about 285 to 1700 KB/s (https, far server) and from 950 to about 3800 KB/s (http), the
  Vita's own TCP limit. The log says the buffer once (*receive buffer: 512 KB*, or what was refused) and each
  connection how long it took to connect (about one round trip) and the speed its buffer allows.
- A film whose server answers a jump with a little data and then nothing (seen with a far server: 14 KB in 10 s,
  then *No video data received*) is asked for again after 8 s, once, and waits 20 s in all before giving up.
- **Queues:** on a line at least twice as fast as the film the queues take about a minute (40 MB) at once.
  With full queues the reader takes only what is played; that rate was taken for the network's and every fast
  film said *the network is slower than the film*. Now the time spent waiting for room is left out: the log adds
  *queues full N% of the time; the line brings (or last brought) X KB/s*, measured only while the reader read most
  of the time, and the information screen shows the line's speed. The video queue holds 1800 pictures before that
  (30 s at 60 fps; was 750, 12 s at 60 fps). The wait at a film's start says *buffering at the start* instead
  of *the network is slower than the film*.

**Look**
- The button symbols (X, O, Triangle, Square and the d-pad) are drawn smooth (anti-aliased) and the four face
  buttons take the same space; before, the X was larger and the corners looked jagged.

**Removed**
- The AC-3 test modes (`ac3_test` in `settings.txt`), the AC-3 trace and the detailed timing line of the 0.5.0
  test builds (the stutter they were for is fixed).

## 0.5.0

0.4.1 and 0.4.5 were test builds and were not released; their changes are part of 0.5.0 and are listed here.

**Audio**
- **AC-3 and E-AC-3 (Dolby Digital / Dolby Digital Plus)** play, on channels (TS, HLS) and in films (MKV, MP4, TS),
  decoded in software with FFmpeg's decoder (LGPL, only the AC-3 / E-AC-3 decoders are built in). 5.1 and 7.1
  sound comes out as stereo. When a stream has AAC as well, AAC is chosen (the hardware decodes it).
  Until now these films played without sound and these channels needed the transcoding server.
- **Choosing the audio track:** a channel or film with more than one sound (TS: several audio streams, MKV: several
  audio tracks) lists them in the **Select** menu under the picture sizes, with the language when the stream says
  it (*Audio: Turkish (AAC)*, *Audio: English (E-AC-3)*). A film keeps its position when you change the track. The
  choice stays for the next channels in that language while the app runs. Without a choice the stream's default
  track plays (MKV), else the best one the Vita can play.
- The information screen (Triangle) and the radio screen show AC-3 / E-AC-3.
- 1080p films with AC-3 / E-AC-3 sound stuttered in the test builds (a third or more of the pictures dropped). The
  cause: Matroska keeps several sound frames in one block (8 frames, 256 ms) and every frame got the block's time,
  so the sound clock stood still, then jumped 256 ms, and the picture that follows it was dropped as late. Now each
  frame gets its own time. (The same could happen on TS channels with several AC-3 frames in one packet.)
- `ac3_test=1`, `2` or `3` in `settings.txt` stay as test modes for AC-3 (see the log); they also log the first 300
  sound frames.

**Transcoding server**
- *Use automatically* has three choices: *Off*, *When needed* (as before: channels the Vita cannot play) and
  *Always* (every channel through the server; files on the memory card play directly).
- **Select while watching** has a *Through the Server* line under the picture sizes (*Direct* when the stream comes
  from the server): the channel or film you are watching moves to the server, a film from where it was. Shown
  only when a server is set. Square still switches too.
- *Test Server* in the settings no longer closes the app.
- Films through the server show where they are: the server sends the film's length (with `ffprobe`, which
  comes with ffmpeg), so the time line and the jump marker work. Update `vita_iptv_proxy.py` on the PC / phone.
- Through the server, films and channels get 30 s instead of 10 s for their first picture (the server first
  finds the place in a film and starts converting; on a phone that took longer than 10 s).
- The server also runs on an Android phone with Termux (see the README).

**Favourites (START)**
- **Playlists:** START marks a playlist; favourite playlists are at the top with a star.
- **Live channels:** START on the channel list or while watching adds or removes the channel (a short note says
  which). Favourites have a star, come first in their category and in *All* (in the order you added them), and a
  *Favorites* category at the top of the list holds them all. A single stream link that is playing: START marks
  its playlist.
- Saved in `ux0:data/VitaIPTV/favorites.txt`: Xtream channels by their id (they stay favourites when the password
  changes), M3U channels by their link. Editing a playlist's address keeps its favourites.
- **Reordering:** in the *Favorites* category Triangle picks the channel up, Up / Down moves it, Triangle again
  saves the order (O leaves it as it was). The new order is also the order at the top of *All* and the categories.
- Films and series cannot be favourites.

**Recently watched**
- **Recently Watched:** a category with the last 20 channels watched in that playlist, newest first.
- *Clear Recently Watched* in the settings (with a question first) empties it for every playlist; favourites stay.

**Xtream EPG (now / next)**
- While a live channel plays, the second line of the top bar shows what is on now and next, with the times in the
  Vita's time zone. In the channel list the same line appears for the selected channel after half a second.
- Asked for one channel at a time in the background and kept for 5 minutes. Channels without a programme guide
  show nothing.

**Turkish letters**
- ğ ş ı İ ç ö ü are drawn as they are, with the Latin system font the Vita's own menus use (until now they were
  drawn as g s i I c o u). Cyrillic, Greek and other names still come from the default font. If the letters look
  wrong on a console, `plain_letters=1` in `ux0:data/VitaIPTV/settings.txt` brings back the plain ones.

**Channels that did not play well**
- **720p at 50 fps with timing information in the stream** (HRD, picture timing): the player takes the HD decoder
  for these because of the frame rate, and it gave no picture ("The Vita decoder gives no picture for this
  stream"). The second try with the H.264 header adjusted, which already helped the normal decoder in 0.4.0,
  now also happens on the HD decoder; if that still gives nothing, the normal decoder takes the stream.
- **720p with 4 reference pictures and timing information:** the normal decoder ran out of picture memory after a
  few pictures and the picture was lost for about 8 seconds until the next keyframe. These channels now open on the
  HD decoder at once.
- **720p at 50 fps on the HD decoder:** when the decoder cannot keep up, the pictures no other picture needs are left
  out from the start (as for 1080p at 50 fps), so the picture stays in time with the sound.

**Screens**
- Settings and the playlist form: everything at the size of the playlist names; the value column starts after the
  longest label, so long labels no longer run into their boxes.
- Labels start every word with a capital (*Add Playlist*, *Liste Ekle*); messages stay as they are.
- Shorter hints at the bottom: *Fav.*, *Res.* / *Çöz.*, *P. Channel* / *Ö. Kanal*.
- About shows when the build was made, as the log does.
- The hints at the bottom are grouped: X, Square, Triangle and O on the left; L/R, L (the channel before), Select
  and START on the right. Hints for the d-pad are gone (it works as before). Square in the player shows only when
  it does something (another picture size, the server, 720p for a film).
- Notes in the player, error boxes, the radio note, the status line and the delete question go on to the next
  line instead of running off the screen.
- "The Vita decoder gives no picture for this stream" was always shown in English: now translated.
- The Select menu fits longer lists (sizes, server, audio tracks, analysis) and longer names.
- Bigger letters everywhere, at the size of the settings: channel list and categories (11 rows a page instead of
  12), the Select menus, the questions and About.

**Log**
- The second line says when the build was made (`version v0.5.0, built 2026-10-19 10:45`).
- When the H.264 header (SPS) changes in the middle of a stream, the log says what changed (to find out why some
  HLS channels lose their picture after a few seconds).
- The audio tracks of a stream and which one plays.
- A detail line every 5 s: how evenly pictures come out of the decoder, how long the sound output blocks and
  whether the sound clock keeps real time.

## 0.4.0

**Channels that did not open**
- **HLS links that redirect to another server** (e.g. `.../startv4puhu?m3u8` → another host with a token): the
  qualities and segments are now looked up at the address that answered. Before, every one gave *HTTP 404*.
- **A quality that gives no picture** (an HLS channel whose "480p" sends only sound, or video without a single
  keyframe): the player switches to another quality by itself and says so. While you zap, that channel then opens
  on the quality that worked.
- **720p channels with "Decoder produced no picture":** when the decoder gives nothing from the start, the player
  tries once more with the H.264 header adjusted for the Vita before going to the HD decoder.
  - The header gets *bitstream_restriction* (how many pictures to hold) when it has none.
  - Its timing information (HRD, picture timing messages) is taken out.
  - The log and the stream analysis show whether a channel carries timing information.
- When other channels played fine, *no picture* errors no longer tell you to restart the Vita (the stream is the
  problem, and the transcoding server may play it).

**Picture size**
- **Select** opens the size menu on every channel, film and radio station: channels and films with one size show
  it, and the stream analysis is always the last entry. The *Settings* line shows the setting, e.g. *Settings (720p)*.
- A size chosen from the menu is found again when the server changes its bit rates a little (the nearest one
  within 25%, not bigger than the chosen size). Qualities wider than 1920 pixels are no longer listed.

**One button layout everywhere**
- **Select** opens the menu on the playlists and on the channel screen (Add playlist, Settings, Reload, About,
  Exit). Changing a filter in the settings from the channel screen filters the list at once.
- **Deleting a playlist** is now the red *Delete playlist* button at the end of its edit form (Triangle), with a
  question first. Select no longer deletes.
- **START** saves forms (playlists, settings).
- Moving through a film: **Left / Right** and the **left stick**. L / R and the right stick no longer do it.
- The **left stick** moves up and down in the playlists, forms and menus; **L / R** turn pages on the playlists.
- Stream analysis: **O** goes back to the picture.
- **L** while watching goes back to the channel you watched before (from another category too: the list goes to
  *All*).
- The radio screen showed a wrong hint (*Triangle: Analyze*): it is *Select*. Square on the radio no longer
  tries to turn the screen off (it never worked on the Vita).
- Film hints: *◀▶ Seek* and *Select: Resolution*.

**The Vita hung after closing the app during radio** (every version with radio): the app no longer takes the
system's "no standby" lock for the radio, which stayed behind when the app was closed with the PS button while a
station was playing. The radio still keeps the Vita awake while it plays.

**Look**
- Bigger, clearer button symbols in the hint bar (lighter circles, thicker lines).
- The Yes / No buttons of a question are inside its box.
- **New About screen:** version, what the app plays, the GitHub address with a QR code to open it on a phone.
- **No internet connection** is said at once (loading a list, opening a channel), instead of a vague error after
  a time-out.

**Settings**
- *Hide 1080p channels* is now **Hide channels named 4K / UHD** (4K, UHD, Ultra HD, 2160p, 8K): the Vita plays
  1080p, but not 4K. It starts off, also if the old setting was on.

## 0.3.0

The main changes since 0.2.2 (0.2.3 - 0.2.10 were test versions).

**Picture size**
- HLS channels that offer several qualities open at about 540p (960x540, the Vita's own screen size: sharp,
  less data, light to decode) if the channel has it, else at 720p.
- **Settings > Preferred resolution:** Automatic, 1080p, 720p, 540p, 480p or Lowest.
- **Select** while watching lists the channel's sizes with their bit rates, with *Automatic* at the top and the
  stream analysis at the end (also on the error screen). The choice is for that channel only.
- **Square** goes to the next size up; from the biggest it goes back to the smallest.
- On a connection that cannot keep up, the player steps down to the next smaller size by itself - but not when you
  chose a size (with Select / Square or in the settings), except 1080p.

**Smoother live TV**
- On a slow connection live TV waits with *Buffering...* (sound and picture together, at most about 8 s) instead
  of freezing or losing the sound sync. The number shows the seconds of video collected.
- The sound starts together with the first picture, and when the picture falls behind the sound, the sound waits
  briefly until it has caught up.
- When the decoder itself cannot keep up, pictures that no other picture needs are left out, so the picture stays
  in time with the sound. A slow network never causes this.

**1080p and 50/60 fps**
- The Vita decoder manages 1080p up to about 30 pictures a second (Sony's own player stops at H.264 level 4.1).
  1080p at 50/60 fps plays at a lower frame rate in time with the sound; HLS channels go to their 720p version,
  other streams say so on screen.
- **720p at 50/60 fps** now plays through the HD decoder: all pictures (before: about 85% at 720p60).
- Files no longer freeze in their last seconds with the sound going on.
- Fewer stalls with the HD decoder (pictures it has ready are taken out at once; no needless restarts).

**Films and series**
- **Much faster jumping:** the film's index is kept in memory while the film is open (an MP4 index of several MB
  used to be read again for every jump).
- The sound starts with the picture, also after jumps and at the start of a film.
- *Buffering...* shows the connection speed against what the film needs, and waits longer at once (fewer stops).
- Black bars above and below the picture (it was the menus' dark blue).

**Channels that did not play**
- Channels with "AAC Main" sound now have sound (most are AAC-LC with another label).
- Many 720p channels that never showed a picture ("Decoder produced no picture") now switch to the HD decoder.
- "Invalid stream" errors from the decoder no longer freeze the picture until the next keyframe.
- A channel whose server stops answering says so.

**Information**
- **Triangle** shows three rows in fixed columns: picture size, shown / stream frame rate (e.g. 43 / 50 fps),
  pictures shown and dropped; sound, A/V offset, waits, damaged pictures; connection speed against what the stream
  needs.
- Stream analysis warns about 1080p at 50/60 fps and about AAC Main sound.
- New round logo (LiveArea, start screen, loading screens).
- Transcoding server (`tools/vita_iptv_proxy.py`): a source that keeps printing errors no longer stops the picture,
  account details are no longer shown in its window, and a source that stops answering ends after 15 s.

## 0.2.2

- **Films no longer freeze after buffering:** the HD decoder sometimes answered "buffer full" when the film went on
  after a pause or after *Buffering...*; that was treated as an error and everything up to the next keyframe was
  thrown away (frozen picture, sound going on). Now the player waits a moment and gives the same picture again.
- Buffering waits longer each time it happens again (5, 10, 20, up to 30 s): fewer stops on a slow connection.
- A film's index (MKV) that arrives damaged no longer sends it back to the start: the place is found from the
  file size instead. The MP4 index shows how far it is read (it can take half a minute on a slow line).
- **1080i channels showed the picture twice, squashed:** the decoder gives the two fields one above the other;
  this is now recognised and the top field is shown full screen.
- The sound no longer switches off for good when a few damaged audio frames arrive (e.g. "7 channels").
- **Referer / User-Agent for streams that need them:** read from `#EXTVLCOPT`, `#EXTHTTP` and `link|User-Agent=...`
  in M3U lists, and two optional fields for single streams. Stream analysis and the transcoding server use them too.
- After standby the app waits for the Wi-Fi to come back instead of ending the channel with an error.
- Full CPU speed only while playing, loading or analysing; normal speed in the menus (less heat and battery).
- Stream analysis compares the download speed with what an HLS stream needs.
- New round LiveArea bubble; the start screen no longer shows an old version number under the new one.
- Smaller fixes: the old 1080p setting is gone from settings.txt; safer stopping of network requests;
  FLV films that do not start at 0 show the right position.

## 0.2.1

- **FLV:** HTTP-FLV live streams and .flv films (H.264 with AAC or MP3); stream analysis reads them too.
- **Films no longer freeze on a slow connection:** the film waits with *Buffering...* until a few seconds are
  loaded, instead of dropping pictures while the sound goes on.
- **720p channels that froze** (decoder out of memory, 4 reference pictures with B-frames) switch to the HD
  decoder with more picture memory instead of restarting and freezing again.
- https connections to a server that does not answer give up after a time limit and can be cancelled with O.
- Less data wasted when a film server does not send parts of files.
- The start screen showed "v0.1": the version is now drawn by the app.
- Smaller fixes in the player threads (stopping, the first error is the one shown).

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
