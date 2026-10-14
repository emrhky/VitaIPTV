# Changelog

## 0.3.0

The main changes since 0.2.2

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
