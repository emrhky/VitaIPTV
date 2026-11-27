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
# 1.2.4d: films whose address answers with an HLS playlist (TS segments: each packet starts with 0x47)
import random as _rnd
_rnd.seed(7)
def _ts_seg(n):
    return b"".join(b"\x47" + bytes(_rnd.getrandbits(8) for _ in range(187)) for _ in range(n))
SEGS = [_ts_seg(1500 + 37 * k) for k in range(12)]           # about 280 KB each
open(f"{T}/hls_plain.ts", "wb").write(b"".join(SEGS))
KEY = bytes(range(16))
def _aes(seg, k):
    iv = k.to_bytes(16, "big")                               # (no IV given: the sequence number)
    open(f"{T}/seg.in", "wb").write(seg)
    subprocess.run(f"openssl enc -aes-128-cbc -K {KEY.hex()} -iv {iv.hex()} -in {T}/seg.in -out {T}/seg.out", shell=True, check=True)
    return open(f"{T}/seg.out", "rb").read()
AESSEGS = [_aes(sg, k) for k, sg in enumerate(SEGS)]
PL403 = {"n": 0}
SEG403 = {"n": 0}
def _media(prefix, key=False, fmp4=False):
    t = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
    if key: t += '#EXT-X-KEY:METHOD=AES-128,URI="../key.bin"\n'
    if fmp4: t += '#EXT-X-MAP:URI="init.mp4"\n'
    for k in range(len(SEGS)): t += f"#EXTINF:4.0,\n{prefix}seg{k}.ts\n"
    return (t + "#EXT-X-ENDLIST\n").encode()

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
        if p.startswith("/hls"):                   # 1.2.4d
            kind = p.split("/")[1]                 # hls, hlsaes, hlsfmp4, hls403, hlsslow, hlslive
            rest = p.split("/", 2)[2]
            body, ctype = None, "application/vnd.apple.mpegurl"
            if rest == "film.mkv":
                body = ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360\nlow/index.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1280x720\nhi/index.m3u8\n").encode()
            elif rest == "hi/index.m3u8" and kind == "hlsbusy" and PL403["n"] < 2:   # 1.2.4t: right after a stream
                PL403["n"] += 1
                self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers(); return
            elif rest == "hi/index.m3u8":
                body = _media("", key=kind == "hlsaes", fmp4=kind == "hlsfmp4")
                if kind == "hlslive": body = body.replace(b"#EXT-X-ENDLIST\n", b"")
            elif rest == "key.bin": body, ctype = KEY, "application/octet-stream"
            elif rest.startswith("hi/seg"):
                k = int(rest[6:-3])
                if kind == "hls403" and k == 2 and SEG403["n"] < 2:
                    SEG403["n"] += 1
                    self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers(); return
                body, ctype = (AESSEGS if kind == "hlsaes" else SEGS)[k], "video/mp2t"
                if kind == "hlsslow": time.sleep(0.4)
            if body is None:
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
            self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers()
            try: self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError): pass
            return
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
   f"{ROOT}/src/curlio.c {ROOT}/src/hls.c {ROOT}/src/hlsaes.c {M}/mock_rt.c {MLIB} -lm -o {T}/test_dl")
os.makedirs(f"{T}/root")
env = dict(os.environ, DL_BASE=f"http://127.0.0.1:{srv.server_address[1]}")
r = subprocess.run([f"{T}/test_dl", SRC, f"{T}/root/downloads", f"{T}/hls_plain.ts"], capture_output=True, text=True, env=env, timeout=600)
print(r.stdout); print(r.stderr[-3000:])
sys.exit(r.returncode)
