# Third-party notices

Vita IPTV itself is released under the MIT License (see `LICENSE`). The VPK also contains code from the
projects below, each under its own license. Their copyright notices are kept here as those licenses ask.

| Component | Used for | License |
|---|---|---|
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | built-in TLS (https) | Apache-2.0 (chosen from Apache-2.0 OR GPL-2.0-or-later); full text in `third_party/mbedtls/LICENSE` |
| [FFmpeg](https://ffmpeg.org) 7.1 (libavcodec, libavutil) | AC-3 / E-AC-3 and 5.1 / 7.1 AAC audio decoding (only when built in) | LGPL 2.1 or later; full text in `licenses/ffmpeg-COPYING.LGPLv2.1.txt` in the VPK |
| [minimp3](https://github.com/lieff/minimp3) | MP2 / MP3 audio decoding | CC0 1.0 (public domain); `src/minimp3.LICENSE` |
| [libvita2d](https://github.com/xerpi/libvita2d) | drawing | MIT |
| [FreeType](https://freetype.org) | font rendering (through vita2d) | FreeType License (FTL) |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG pictures (through vita2d) and film posters | libpng License |
| [libjpeg-turbo](https://libjpeg-turbo.org) (libjpeg) | JPEG pictures (through vita2d) and film posters | IJG License and BSD-3-Clause |
| [zlib](https://zlib.net) | compression (through libpng) and compressed (.gz) programme guides | zlib License |
| [VitaSDK](https://vitasdk.org) | toolchain, system library stubs, C library (newlib) | various permissive licenses |

Required statements:

- Mbed TLS: Copyright The Mbed TLS Contributors. Licensed under the Apache License, Version 2.0
  (http://www.apache.org/licenses/LICENSE-2.0). Its build options are set in
  `third_party/mbedtls_user_config.h`.
- Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- This software is based in part on the work of the Independent JPEG Group.
- libjpeg-turbo: Copyright (C) the libjpeg-turbo Project authors (BSD-3-Clause for its TurboJPEG parts).
- zlib: Copyright (C) Jean-loup Gailly and Mark Adler.
- libpng: Copyright (c) the PNG Reference Library Authors.

## FFmpeg (LGPL)

When the VPK is built with AC-3 / E-AC-3 / AAC support, it contains FFmpeg 7.1's libavcodec and libavutil, linked
statically. FFmpeg is free software under the GNU Lesser General Public License, version 2.1 or later
(https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html); this build uses no GPL or non-free parts.

- Source: the unmodified FFmpeg 7.1 release, https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
- Build: `tools/build_ffmpeg_vita.sh` in this repository, configured with
  `--disable-everything --enable-decoder=ac3,eac3,aac --enable-parser=ac3 --enable-small --disable-pthreads`
  (`--disable-asm` unless `ASM=1`), cross-compiled with VitaSDK (`arm-vita-eabi-`).
- Relinking: the complete source of Vita IPTV is public (MIT, https://github.com/emrhky/VitaIPTV), so the app
  can be rebuilt with a modified FFmpeg by replacing `third_party/ffmpeg-vita` and building again.
- FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.

Dolby, Dolby Digital and Dolby Digital Plus are trademarks of Dolby Laboratories. The app only decodes the
audio with FFmpeg's open-source decoder; it is not certified by or affiliated with Dolby.

## Trademarks

"PlayStation", "PS Vita" and "PlayStation Vita" are trademarks of Sony Interactive Entertainment Inc.
Vita IPTV is an independent homebrew project and is not affiliated with, endorsed or sponsored by Sony.
Any other names that appear in the app come from the user's own playlists.
