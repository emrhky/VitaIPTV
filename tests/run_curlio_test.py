#!/usr/bin/env python3
"""curlio.c (own HTTP client + mbedTLS from third_party) against a local HTTPS server (needs gcc, openssl)."""
import os, ssl, subprocess, sys, tempfile, threading, time, base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbedtls_pc
MCF, MLIB = mbedtls_pc.build()
T = tempfile.mkdtemp()
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
subprocess.run(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra {MCF} -I{ROOT}/src "
               f"{ROOT}/tests/test_curlio.c {ROOT}/src/curlio.c {ROOT}/src/hls.c {MLIB} -o {T}/test_curlio",
               shell=True, check=True)
r = subprocess.run([f"{T}/test_curlio", str(port), f"{T}/file.ts", str(srv12.server_address[1])], capture_output=True, text=True, timeout=120)
print(r.stdout); print(r.stderr[-3000:])
sys.exit(r.returncode)
