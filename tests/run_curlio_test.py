#!/usr/bin/env python3
"""curlio.c (own HTTP client + mbedTLS from third_party) against a local HTTPS server (needs gcc, openssl)."""
import os, ssl, subprocess, sys, tempfile, threading, time, base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbedtls_pc
MCF, MLIB = mbedtls_pc.build()
T = tempfile.mkdtemp()
# 0.6.0: the receive buffer search (a system that refuses sizes above a limit, like the Vita at 1 MB)
# 1.2.4m: the compilers' warnings kept out of the output (on GitHub they filled the 40 lines shown of a failure)
def cc(cmd):
    c = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if c.returncode: print(c.stdout, c.stderr[-4000:]); sys.exit(1)
cc(f"gcc -O1 -g -fsanitize=address,undefined -Wall {MCF} -I{ROOT}/src {ROOT}/tests/test_rcvbuf.c {ROOT}/src/hls.c {MLIB} -o {T}/test_rcvbuf")
r = subprocess.run(f"{T}/test_rcvbuf", shell=True, capture_output=True, text=True)
print(r.stdout, r.stderr[-2000:])
if r.returncode: sys.exit(1)
subprocess.run(f"openssl req -x509 -newkey rsa:2048 -nodes -keyout {T}/k.pem -out {T}/c.pem -days 2 -subj /CN=localhost",
               shell=True, check=True, capture_output=True)
data = os.urandom(300000)
open(f"{T}/file.ts", "wb").write(data)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/file.ts":
            self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
        elif self.path == "/redirect":
            self.send_response(302); self.send_header("Location", "/file.ts"); self.send_header("Content-Length", "0"); self.end_headers()
        elif self.path == "/auth":
            ok = self.headers.get("Authorization") == "Basic " + base64.b64encode(b"user:p@ss").decode()
            self.send_response(200 if ok else 401); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
        elif self.path == "/hdr":                           # a CDN that checks Referer and User-Agent
            ok = self.headers.get("Referer") == "https://site.example/page" and self.headers.get("User-Agent", "").startswith("Mozilla/5.0 (Test")
            self.send_response(200 if ok else 403); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
        elif self.path == "/slow":                          # a live stream: chunked, never ends
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            try:
                for _ in range(400):
                    blk = os.urandom(8192)
                    self.wfile.write(b"%x\r\n" % len(blk) + blk + b"\r\n"); self.wfile.flush(); time.sleep(0.05)
            except Exception: pass
        else:
            self.send_response(404); self.send_header("Content-Length", "3"); self.end_headers(); self.wfile.write(b"no\n")

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(f"{T}/c.pem", f"{T}/k.pem")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]
# a second server that speaks only TLS 1.2 (many CDNs still do)
srv12 = ThreadingHTTPServer(("127.0.0.1", 0), H)
ctx12 = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx12.load_cert_chain(f"{T}/c.pem", f"{T}/k.pem")
ctx12.maximum_version = ssl.TLSVersion.TLSv1_2
srv12.socket = ctx12.wrap_socket(srv12.socket, server_side=True)
threading.Thread(target=srv12.serve_forever, daemon=True).start()
cc(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra {MCF} -I{ROOT}/src "
   f"{ROOT}/tests/test_curlio.c {ROOT}/src/curlio.c {ROOT}/src/hls.c {MLIB} -o {T}/test_curlio")
# 1.5.1: a server that drops the first SYNs (its queue full for 4.6 s): how long one socket alone waits, then the app
import socket as so
def busy_server(free_after):
    s = so.socket(); s.setsockopt(so.SOL_SOCKET, so.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(0)
    fill = []
    for _ in range(3):
        c = so.socket(); c.setblocking(False)
        try: c.connect(s.getsockname())
        except BlockingIOError: pass
        fill.append(c)
    time.sleep(0.3)
    def run():
        time.sleep(free_after)
        while True:
            a, _ = s.accept()
            def h(a):
                try:
                    a.settimeout(5)
                    if a.recv(4096): a.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
                except Exception: pass
                a.close()
            threading.Thread(target=h, args=(a,), daemon=True).start()
    threading.Thread(target=run, daemon=True).start()
    return s.getsockname()[1], fill
p1, keep1 = busy_server(4.6)
t0 = time.time(); c = so.socket(); c.settimeout(30); c.connect(("127.0.0.1", p1)); plain = time.time() - t0; c.close()
p2, keep2 = busy_server(4.6)
# 1.2.4m: TLS servers whose handshake fails (a fatal alert) the first time / every time
def tls_flaky(fail_first):
    s = so.socket(); s.setsockopt(so.SOL_SOCKET, so.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(8)
    n = [0]
    def run():
        while True:
            a, _ = s.accept()
            n[0] += 1
            def h(a, k):
                try:
                    a.settimeout(5)
                    if k <= fail_first:
                        a.recv(4096)
                        a.sendall(b"\x15\x03\x03\x00\x02\x02\x50")   # alert: fatal, internal_error
                        a.close(); return
                    t = ctx.wrap_socket(a, server_side=True)
                    if t.recv(4096): t.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
                    t.close()
                except Exception: pass
            threading.Thread(target=h, args=(a, n[0]), daemon=True).start()
    threading.Thread(target=run, daemon=True).start()
    return s.getsockname()[1]
pf1, pfa = tls_flaky(1), tls_flaky(1000)
r = subprocess.run([f"{T}/test_curlio", str(port), f"{T}/file.ts", str(srv12.server_address[1]), str(p2), "%.2f" % plain, str(pf1), str(pfa)],
                   capture_output=True, text=True, timeout=120)
print(r.stderr[-3000:]); print(r.stdout)                  # (the test's own lines last: they are what a failure shows)
if r.returncode: print(f"test_curlio ended with {r.returncode}" + (" (a signal)" if r.returncode < 0 else ""))
sys.exit(1 if r.returncode else 0)
