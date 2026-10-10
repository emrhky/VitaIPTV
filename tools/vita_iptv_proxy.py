#!/usr/bin/env python3
"""
Vita IPTV transcoding server.

Runs on a PC / laptop / Raspberry Pi in the same network as the Vita and converts
any channel the Vita cannot play (1080p, HEVC/H.265, MKV, MP2 or AC-3 audio...)
into what it can: H.264 up to 720p, at most 30 fps, AAC stereo, in MPEG-TS.

Needs Python 3.7+ and ffmpeg (https://ffmpeg.org) in the PATH.

    python3 vita_iptv_proxy.py                 # listens on port 8090
    python3 vita_iptv_proxy.py --port 9000 --height 576 --bitrate 1800k

Then on the Vita: START > Settings > Transcoding server = http://<this PC's IP>:8090

Endpoints
    GET /play?url=<url-encoded stream address>   the converted stream
    GET /ping                                    "ok" (used by the app's "Test" button)
    GET /                                        short status page
"""
import argparse
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ARGS = None
ACTIVE = {}
ACTIVE_LOCK = threading.Lock()


def ffmpeg_command(url, start=0):
    a = ARGS
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "+genpts"]
    if start > 0:
        cmd += ["-ss", str(start)]                                    # a film from the middle
    if url.startswith(("http://", "https://")):
        cmd += ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"]
        if a.user_agent:
            cmd += ["-user_agent", a.user_agent]
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


class Handler(BaseHTTPRequestHandler):
    server_version = "VitaIPTVProxy/1.0"

    def log_message(self, fmt, *args):
        if ARGS.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

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
        cmd = ffmpeg_command(url, start)
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        except OSError as e:
            return self._text(500, "cannot start ffmpeg: %s" % e)

        # the first bytes tell whether ffmpeg could open the source at all
        first = proc.stdout.read(188 * 64)
        if not first:
            err = proc.stderr.read().decode("utf-8", "replace").strip()[-300:]
            proc.wait()
            sys.stderr.write("cannot convert %s: %s\n" % (url, err))
            return self._text(502, "conversion failed: " + (err or "no data"))

        key = id(self)
        with ACTIVE_LOCK:
            ACTIVE[key] = url
        sys.stderr.write("[%s] start  %s\n" % (time.strftime("%H:%M:%S"), url))
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
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
            sys.stderr.write("[%s] stop   %s (%d KB)\n" % (time.strftime("%H:%M:%S"), url, sent // 1024))


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
        print("  On the Vita: START > Settings > Transcoding server = http://%s:%d" % (ip, ARGS.port))
    print("  Output: H.264 up to %dp, %d fps max, %s, AAC stereo. Ctrl+C to stop." % (ARGS.height, ARGS.max_fps, ARGS.bitrate))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
