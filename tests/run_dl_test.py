#!/usr/bin/env python3
"""1.3: the downloads (src/dl.c) against a local HTTP server: ranges, a dropping connection, a server
without ranges, a slow one, 404. Needs gcc (the TLS of curlio is third_party/mbedtls, as in run_https_tests)."""
import os, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbedtls_pc
MCF, MLIB = mbedtls_pc.build()
T = tempfile.mkdtemp(prefix="dlt_")
SRC = f"{T}/src.bin"
with open(SRC, "wb") as f:
    f.write(os.urandom(5 * 1024 * 1024 + 12345))
POSTER = b"\xff\xd8\xff\xe0" + os.urandom(2000)
DROPPED = {}

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        p = self.path
        if p == "/film.srt":                       # 1.3.1: a subtitle file beside film.mkv (not beside film2.mkv)
            b = "1\n00:00:01,000 --> 00:00:02,000\nMerhaba\n".encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers()
            self.wfile.write(b); return
        if p == "/poster.jpg":
            self.send_response(200); self.send_header("Content-Length", str(len(POSTER))); self.end_headers()
            self.wfile.write(POSTER); return
        if p.startswith("/empty/"):                # 1.5.1: 200, no size, nothing (1.5.0: "finished (0 MB)")
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.close_connection = True; return
        if p.startswith("/e405/"):                 # 1.4.1: a server that does not give this file (or: the account is busy)
            self.send_response(405); self.send_header("Content-Length", "0"); self.end_headers(); return
        drop = p.startswith("/drop/"); norange = "/norange/" in p; slow = "/slow/" in p
        name = p.rsplit("/", 1)[-1]
        if name not in ("film.mkv", "film2.mkv", "film3.mkv"):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        data = open(SRC, "rb").read()
        size = len(data)
        a, b = 0, size - 1
        rng = self.headers.get("Range")
        if rng and not norange:
            x, _, y = rng.split("=", 1)[1].partition("-")
            a = int(x); b = int(y) if y else size - 1
            if a >= size:
                self.send_response(416); self.send_header("Content-Range", f"bytes */{size}"); self.send_header("Content-Length", "0"); self.end_headers(); return
            self.send_response(206); self.send_header("Content-Range", f"bytes {a}-{b}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(b - a + 1)); self.end_headers()
        sent, limit = 0, (1024 * 1024 if drop else None)
        try:
            pos = a
            while pos <= b:
                n = min(65536, b - pos + 1)
                if limit is not None and sent + n > limit:
                    self.wfile.write(data[pos:pos + (limit - sent)]); self.wfile.flush()
                    self.close_connection = True
                    return                         # (cut: the client asks again from there)
                self.wfile.write(data[pos:pos + n]); pos += n; sent += n
                if slow: time.sleep(0.02)
        except (BrokenPipeError, ConnectionResetError): pass

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
M = f"{ROOT}/tests/mock_rt"
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -pthread {MCF} -I{ROOT}/src -I{M} {ROOT}/tests/test_dl.c {ROOT}/src/dl.c "
   f"{ROOT}/src/curlio.c {ROOT}/src/hls.c {M}/mock_rt.c {MLIB} -lm -o {T}/test_dl")
os.makedirs(f"{T}/root")
env = dict(os.environ, DL_BASE=f"http://127.0.0.1:{srv.server_address[1]}")
r = subprocess.run([f"{T}/test_dl", SRC, f"{T}/root/downloads"], capture_output=True, text=True, env=env, timeout=600)
print(r.stdout); print(r.stderr[-3000:])
sys.exit(r.returncode)
