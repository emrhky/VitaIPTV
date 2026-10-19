#!/usr/bin/env python3
"""
Vita IPTV transcoding server.

Runs on a PC / laptop / Raspberry Pi in the same network as the Vita and converts
any channel the Vita cannot play (1080p, HEVC/H.265, MKV, MP2 or AC-3 audio...)
into what it can: H.264 up to 720p, at most 30 fps, AAC stereo, in MPEG-TS.

Needs Python 3.7+ and ffmpeg (https://ffmpeg.org) in the PATH.

    python3 vita_iptv_proxy.py                 # listens on port 8090
    python3 vita_iptv_proxy.py --port 9000 --height 576 --bitrate 1800k

Then on the Vita: Select > Settings > Transcoding server = http://<this PC's IP>:8090

Endpoints
    GET /play?url=<url-encoded stream address>   the converted stream
        &start=<s>   a film from that second;  &film=1   also send the film's length (X-Content-Duration)
    GET /ping                                    "ok" (used by the app's "Test" button)
    GET /                                        short status page
"""
import argparse
import collections
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ARGS = None
DURATIONS = {}                     # film address -> length in seconds (asked once per film)
DURATIONS_LOCK = threading.Lock()
ACTIVE = {}
ACTIVE_LOCK = threading.Lock()


def split_headers(url):
    """link|User-Agent=...&Referer=... (from #EXTVLCOPT lines in M3U lists) -> link, user agent, referer"""
    from urllib.parse import unquote
    if "|" not in url:
        return url, "", ""
    link, _, tail = url.partition("|")
    ua = ref = ""
    for part in tail.split("&"):
        k, _, v = part.partition("=")
        if k.lower() == "user-agent":
            ua = unquote(v)
        elif k.lower() in ("referer", "referrer"):
            ref = unquote(v)
    return link, ua, ref


def safe(url):
    """The address for the console without account details (Xtream /live/user/pass/..., user:pass@, ?password=)."""
    url = re.sub(r"//[^/@]+@", "//***@", url)
    url = re.sub(r"/(live|movie|series)/[^/]+/[^/]+/", r"/\1/***/***/", url)
    return re.sub(r"(?i)((?:username|password)=)[^&|]*", r"\1***", url)


def ffmpeg_command(url, start=0):
    a = ARGS
    url, link_ua, link_ref = split_headers(url)
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "+genpts"]
    if start > 0:
        cmd += ["-ss", str(start)]                                    # a film from the middle
    if url.startswith(("http://", "https://")):
        cmd += ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
                "-rw_timeout", "15000000"]                         # a source that stops answering: give up after 15 s
        if link_ua or a.user_agent:
            cmd += ["-user_agent", link_ua or a.user_agent]
        if link_ref:
            cmd += ["-referer", link_ref]
    cmd += ["-i", url,
            "-map", "0:v:0?", "-map", "0:a:0?",                     # first video and audio, either may be missing
            "-c:v", "libx264", "-preset", a.preset, "-tune", "zerolatency",
            "-profile:v", "main", "-level", "3.1", "-pix_fmt", "yuv420p",
            "-vf", "scale=-2:'min(%d,ih)':flags=bicubic" % a.height,
            "-fpsmax", str(a.max_fps),
            "-g", str(a.max_fps * 2), "-keyint_min", str(a.max_fps),  # a keyframe every 2 s: quick channel start
            "-b:v", a.bitrate, "-maxrate", a.bitrate, "-bufsize", a.bitrate,
            "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "48000",
            "-f", "mpegts", "-muxdelay", "0.1", "pipe:1"]
    return cmd


def ffprobe_path():
    """ffprobe next to the ffmpeg in use (same folder, or the PATH)"""
    import os
    f = ARGS.ffmpeg
    d, b = os.path.split(f)
    name = b.replace("ffmpeg", "ffprobe") if "ffmpeg" in b else "ffprobe"
    p = os.path.join(d, name) if d else name
    return p if (d and os.path.exists(p)) or shutil.which(p) else None


def film_duration(url):
    """The film's length in seconds (0 if unknown), for the Vita's time line. Asked before ffmpeg starts (not at
    the same time: many IPTV accounts allow one connection), and kept: a jump does not ask again."""
    with DURATIONS_LOCK:
        if url in DURATIONS:
            return DURATIONS[url]
    exe = ffprobe_path()
    dur = 0.0
    if exe:
        src, link_ua, link_ref = split_headers(url)
        cmd = [exe, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1"]
        if src.startswith(("http://", "https://")):
            cmd += ["-rw_timeout", "10000000"]
            if link_ua or ARGS.user_agent:
                cmd += ["-user_agent", link_ua or ARGS.user_agent]
            if link_ref:
                cmd += ["-referer", link_ref]
        cmd.append(src)
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout
            dur = float(out.decode("ascii", "replace").strip().splitlines()[0])
        except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
            dur = 0.0
    with DURATIONS_LOCK:
        DURATIONS[url] = dur
    return dur


class Handler(BaseHTTPRequestHandler):
    server_version = "VitaIPTVProxy/1.0"

    def log_message(self, fmt, *args):
        if ARGS.verbose:
            from urllib.parse import unquote
            sys.stderr.write("%s - %s\n" % (self.address_string(), safe(unquote(fmt % args))))

    def _text(self, code, body):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/ping":
            return self._text(200, "ok")
        if u.path == "/":
            with ACTIVE_LOCK:
                n = len(ACTIVE)
            return self._text(200, "Vita IPTV transcoding server\nactive streams: %d\nsettings: %dp, %d fps max, %s\n"
                              % (n, ARGS.height, ARGS.max_fps, ARGS.bitrate))
        if u.path != "/play":
            return self._text(404, "unknown path")
        url = parse_qs(u.query).get("url", [""])[0]
        if not url:
            return self._text(400, "missing url parameter")

        try:
            start = max(0, int(parse_qs(u.query).get("start", ["0"])[0]))
        except ValueError:
            start = 0
        film = parse_qs(u.query).get("film", ["0"])[0] == "1"
        duration = film_duration(url) if film else 0.0       # (the Vita shows where the film is)
        cmd = ffmpeg_command(url, start)
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        except OSError as e:
            return self._text(500, "cannot start ffmpeg: %s" % e)

        # ffmpeg's messages are read all the time (kept: the last 50 lines). Before, they were read only when it
        # failed at the start: a stream that kept printing errors filled the pipe (64 KB, on Windows much less),
        # ffmpeg stopped and the picture on the Vita froze.
        errs = collections.deque(maxlen=50)

        def drain():
            for line in proc.stderr:
                errs.append(line.decode("utf-8", "replace").rstrip())
        drainer = threading.Thread(target=drain, daemon=True)
        drainer.start()

        # the first bytes tell whether ffmpeg could open the source at all
        first = proc.stdout.read(188 * 64)
        if not first:
            proc.wait()
            drainer.join(2)                                             # its last words
            err = " / ".join(errs)[-300:]
            sys.stderr.write("cannot convert %s: %s\n" % (safe(url), err))
            return self._text(502, "conversion failed: " + (err or "no data"))

        key = id(self)
        with ACTIVE_LOCK:
            ACTIVE[key] = url
        sys.stderr.write("[%s] start  %s\n" % (time.strftime("%H:%M:%S"), safe(url)))
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
        if duration > 0:
            self.send_header("X-Content-Duration", "%.1f" % duration)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        sent = 0
        try:
            chunk = first
            while chunk:
                self.wfile.write(chunk)
                sent += len(chunk)
                chunk = proc.stdout.read(65536)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass                                                     # the Vita changed channel or left
        finally:
            proc.kill()
            proc.wait()
            with ACTIVE_LOCK:
                ACTIVE.pop(key, None)
            sys.stderr.write("[%s] stop   %s (%d KB)\n" % (time.strftime("%H:%M:%S"), safe(url), sent // 1024))


def local_ips():
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))                                   # no packet is sent, this only picks the LAN address
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ips) or ["<this-computer-ip>"]


def main():
    global ARGS
    p = argparse.ArgumentParser(description="Vita IPTV transcoding server")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--height", type=int, default=720, help="maximum picture height (default 720)")
    p.add_argument("--max-fps", type=int, default=30, help="maximum frame rate (default 30)")
    p.add_argument("--bitrate", default="2500k", help="video bitrate (default 2500k)")
    p.add_argument("--preset", default="veryfast", help="x264 preset; use ultrafast on slow computers")
    p.add_argument("--user-agent", default="", help="User-Agent sent to the IPTV server")
    p.add_argument("--ffmpeg", default="ffmpeg", help="path of the ffmpeg program")
    p.add_argument("--verbose", action="store_true")
    ARGS = p.parse_args()

    if not shutil.which(ARGS.ffmpeg):
        sys.exit("ffmpeg was not found. Install it from https://ffmpeg.org and try again.")
    srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler)
    srv.daemon_threads = True
    print("Vita IPTV transcoding server is running.")
    for ip in local_ips():
        print("  On the Vita: Select > Settings > Transcoding server = http://%s:%d" % (ip, ARGS.port))
    print("  Output: H.264 up to %dp, %d fps max, %s, AAC stereo. Ctrl+C to stop." % (ARGS.height, ARGS.max_fps, ARGS.bitrate))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
