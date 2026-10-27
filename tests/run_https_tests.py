#!/usr/bin/env python3
"""tsplayer over real HTTPS (libcurl + OpenSSL) against a local server: a plain stream and HLS.
   Needs gcc, ffmpeg, openssl (the TLS itself is third_party/mbedtls)."""
import os, ssl, subprocess, sys, tempfile, threading, time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from functools import partial
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbedtls_pc
MCF, MLIB = mbedtls_pc.build()
T = tempfile.mkdtemp(prefix="tsps_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
sh(f"ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 5 "
   f"-c:v libx264 -profile:v high -bf 3 -g 60 -pix_fmt yuv420p -c:a aac -ac 2 -f mpegts {T}/s720.ts")
os.makedirs(f"{T}/hls")
sh(f"ffmpeg -loglevel error -y -i {T}/s720.ts -c copy -f hls -hls_time 1 -hls_list_size 0 -hls_playlist_type vod "
   f"-hls_segment_filename {T}/hls/seg%03d.ts {T}/hls/index.m3u8")
VSRC = ("-f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 "
        "-c:v libx264 -bf 2 -g 25 -keyint_min 25 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -ac 2")
sh(f"ffmpeg -loglevel error -y {VSRC} {T}/film.mp4")
sh(f"ffmpeg -loglevel error -y {VSRC} {T}/film.mkv")
open(f"{T}/hls/master.m3u8", "w").write("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nindex.m3u8\n")
sh(f"openssl req -x509 -newkey rsa:2048 -nodes -keyout {T}/k.pem -out {T}/c.pem -days 2 -subj /CN=localhost")
STALLED = []                                # 0.6.0: /stall/...: the first request into the film's middle sends a little, then nothing
class Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):                       # "Range: bytes=a-b" for films, like nginx
        rng = self.headers.get("Range")
        stall = self.path.startswith("/stall/")
        if stall: self.path = self.path[6:]
        path = self.translate_path(self.path)
        if not rng or not os.path.isfile(path): return super().do_GET()
        size = os.path.getsize(path)
        a, _, b = rng.split("=", 1)[1].partition("-")
        a = int(a); b = int(b) if b else size - 1
        b = min(b, size - 1)
        self.send_response(206)
        self.send_header("Content-Length", str(b - a + 1))
        self.send_header("Content-Range", f"bytes {a}-{b}/{size}")
        self.end_headers()
        if stall and not STALLED and size * 3 // 10 < a < size * 9 // 10:
            STALLED.append(a)
            with open(path, "rb") as f:
                f.seek(a)
                try: self.wfile.write(f.read(10000)); self.wfile.flush()
                except Exception: pass
            time.sleep(25)                  # (the player gives up on this request and asks again)
            return
        with open(path, "rb") as f:
            f.seek(a)
            left = b - a + 1
            try:
                while left > 0:
                    d = f.read(min(65536, left))
                    if not d: break
                    self.wfile.write(d); left -= len(d)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError): pass
srv = ThreadingHTTPServer(("127.0.0.1", 0), partial(Quiet, directory=T))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(f"{T}/c.pem", f"{T}/k.pem")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()
# 0.6.0: the same files over plain http:// (through our own socket too)
psrv = ThreadingHTTPServer(("127.0.0.1", 0), partial(Quiet, directory=T))
threading.Thread(target=psrv.serve_forever, daemon=True).start()
M = f"{ROOT}/tests/mock_rt"
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread {MCF} -DSTALL_US=1500000ULL -I{ROOT}/src -I{M} "
   f"{ROOT}/tests/test_tsplayer.c {ROOT}/src/tsplayer.c {ROOT}/src/spsfix.c {ROOT}/src/tsdemux.c {ROOT}/src/mkvdemux.c {ROOT}/src/flvdemux.c {ROOT}/src/mpadec.c {ROOT}/src/a52dec.c {ROOT}/src/hls.c "
   f"{ROOT}/src/curlio.c {ROOT}/src/vod.c {M}/mock_rt.c {MLIB} -lm -o {T}/test_https")
env = dict(os.environ, HTTPS_BASE=f"https://127.0.0.1:{srv.server_address[1]}",
           HTTP_BASE=f"http://127.0.0.1:{psrv.server_address[1]}")
r = subprocess.run([f"{T}/test_https"], capture_output=True, text=True, env=env, timeout=300)
print(r.stdout); print(r.stderr[-3000:])
sys.exit(r.returncode)
