#!/usr/bin/env python3
"""
Vita IPTV - Xtream test server.

Serves video files from a folder on your PC as if they were an Xtream account, so the Live TV,
Films and Series parts of the app can be tried with your own (or freely licensed) videos.

Folder layout (names are free, only "Series" and "Live" are special):

    videos/
      Big Buck Bunny.mp4              -> film in the category "Films"
      Animation/Sintel.mkv            -> film in the category "Animation"
      Series/My Show/S01E01 Pilot.mkv -> series "My Show", season 1, episode 1
      Series/My Show/S01E02.mkv
      Live/Test channel.ts            -> live channel (the file is sent once, like a short broadcast)

Run:   python3 tools/xtream_test_server.py videos
Then add an Xtream playlist on the Vita with the address, username and password it prints.
Needs Python 3.7+ only. Files are sent with HTTP ranges, like a real server, so jumping works.
"""
import argparse
import json
import os
import re
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

VIDEO = (".mkv", ".mp4", ".m4v", ".mov", ".avi", ".ts", ".webm")
ARGS = None
DB = None


def scan(root):
    """Films, series and live channels found in the folder, with stable ids."""
    films, series, live = [], [], []
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(dirpath, root)
        parts = [] if rel == "." else rel.split(os.sep)
        for f in sorted(files):
            if not f.lower().endswith(VIDEO):
                continue
            path = os.path.join(dirpath, f)
            name, ext = os.path.splitext(f)
            ext = ext[1:].lower()
            if parts and parts[0].lower() == "live":
                live.append({"name": name, "path": path, "ext": "ts"})
            elif parts and parts[0].lower() == "series" and len(parts) >= 2:
                show = parts[1]
                m = re.search(r"[Ss](\d+)\s*[Ee](\d+)", f) or re.search(r"(\d+)x(\d+)", f)
                season, ep = (int(m.group(1)), int(m.group(2))) if m else (1, 0)
                series.append({"show": show, "season": season, "episode": ep, "title": name, "path": path, "ext": ext})
            else:
                cat = parts[0] if parts else "Films"
                films.append({"name": name, "cat": cat, "path": path, "ext": ext})
    cats = sorted({f["cat"] for f in films})
    shows = sorted({s["show"] for s in series})
    db = {"films": [], "film_cats": [], "shows": [], "episodes": {}, "live": [], "files": {}}
    for i, c in enumerate(cats):
        db["film_cats"].append({"category_id": str(i + 1), "category_name": c, "parent_id": 0})
    for i, f in enumerate(films):
        sid = 1000 + i
        db["films"].append({"num": i + 1, "name": f["name"], "stream_type": "movie", "stream_id": sid,
                            "category_id": str(cats.index(f["cat"]) + 1), "container_extension": f["ext"],
                            "rating": "", "added": "0", "direct_source": ""})
        db["files"][("movie", sid)] = f["path"]
    for i, show in enumerate(shows):
        db["shows"].append({"num": i + 1, "name": show, "series_id": 500 + i, "category_id": "1", "cover": ""})
        eps = sorted([s for s in series if s["show"] == show], key=lambda s: (s["season"], s["episode"], s["title"]))
        seasons = {}
        for k, e in enumerate(eps):
            eid = 5000 + i * 1000 + k
            n = e["episode"] or (len(seasons.get(str(e["season"]), [])) + 1)
            seasons.setdefault(str(e["season"]), []).append({
                "id": str(eid), "episode_num": n, "title": e["title"], "container_extension": e["ext"],
                "season": e["season"], "info": {}})
            db["files"][("series", eid)] = e["path"]
        db["episodes"][500 + i] = {"seasons": [{"season_number": int(k)} for k in seasons],
                                   "info": {"name": show}, "episodes": seasons}
    for i, ch in enumerate(live):
        sid = 100 + i
        db["live"].append({"num": i + 1, "name": ch["name"], "stream_type": "live", "stream_id": sid, "category_id": "1"})
        db["files"][("live", sid)] = ch["path"]
    return db


class Handler(BaseHTTPRequestHandler):
    server_version = "VitaIPTVXtreamTest/1.0"

    def log_message(self, fmt, *args):
        if ARGS.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _json(self, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _text(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _creds_ok(self, user, pw):
        return user == ARGS.username and pw == ARGS.password

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/player_api.php":
            return self.api(q)
        if u.path == "/get.php":
            return self.m3u(q)
        m = re.match(r"^/(movie|series|live)/([^/]+)/([^/]+)/(\d+)\.\w+$", u.path)
        if m:
            if not self._creds_ok(unquote(m.group(2)), unquote(m.group(3))):
                return self._text(403, "wrong username or password")
            path = DB["files"].get((m.group(1), int(m.group(4))))
            if not path:
                return self._text(404, "no such file")
            return self.send_file(path)
        return self._text(404, "unknown path")

    def api(self, q):
        if not self._creds_ok(q.get("username", ""), q.get("password", "")):
            return self._json({"user_info": {"auth": 0}})
        a = q.get("action", "")
        cat = q.get("category_id")
        if a == "":
            return self._json({"user_info": {"username": ARGS.username, "auth": 1, "status": "Active",
                                             "exp_date": None, "active_cons": "0", "max_connections": "1"},
                               "server_info": {"url": "test", "port": str(ARGS.port)}})
        if a == "get_live_categories":
            return self._json([{"category_id": "1", "category_name": "Test", "parent_id": 0}] if DB["live"] else [])
        if a == "get_live_streams":
            return self._json(DB["live"])
        if a == "get_vod_categories":
            return self._json(DB["film_cats"])
        if a == "get_vod_streams":
            return self._json([f for f in DB["films"] if not cat or f["category_id"] == cat])
        if a == "get_series_categories":
            return self._json([{"category_id": "1", "category_name": "Series", "parent_id": 0}] if DB["shows"] else [])
        if a == "get_series":
            return self._json([s for s in DB["shows"] if not cat or s["category_id"] == cat])
        if a == "get_series_info":
            try:
                return self._json(DB["episodes"][int(q.get("series_id", "0"))])
            except (KeyError, ValueError):
                return self._json({"seasons": [], "info": {}, "episodes": []})
        return self._json([])

    def m3u(self, q):
        if not self._creds_ok(q.get("username", ""), q.get("password", "")):
            return self._text(403, "wrong username or password")
        host = self.headers.get("Host", "localhost:%d" % ARGS.port)
        base = "http://%s" % host
        lines = ["#EXTM3U"]
        for c in DB["live"]:
            lines += ['#EXTINF:-1 group-title="Test",%s' % c["name"],
                      "%s/live/%s/%s/%d.ts" % (base, ARGS.username, ARGS.password, c["stream_id"])]
        cats = {c["category_id"]: c["category_name"] for c in DB["film_cats"]}
        for f in DB["films"]:
            lines += ['#EXTINF:-1 group-title="%s",%s' % (cats.get(f["category_id"], "Films"), f["name"]),
                      "%s/movie/%s/%s/%d.%s" % (base, ARGS.username, ARGS.password, f["stream_id"], f["container_extension"])]
        return self._text(200, "\n".join(lines) + "\n", "audio/x-mpegurl")

    def send_file(self, path):
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
        a, b = 0, size - 1
        if rng and rng.startswith("bytes="):
            s, _, e = rng[6:].split(",")[0].partition("-")
            try:
                a = int(s) if s else max(0, size - int(e))
                b = int(e) if s and e else size - 1
            except ValueError:
                return self._text(416, "bad range")
            b = min(b, size - 1)
            if a >= size:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, size))
        else:
            self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(b - a + 1))
        self.end_headers()
        left = b - a + 1
        try:
            with open(path, "rb") as f:
                f.seek(a)
                while left > 0:
                    d = f.read(min(262144, left))
                    if not d:
                        break
                    self.wfile.write(d)
                    left -= len(d)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass                                                # the Vita jumped or left


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 9))                           # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    global ARGS, DB
    p = argparse.ArgumentParser(description="Serve a folder of videos as an Xtream account for Vita IPTV.")
    p.add_argument("folder", help="folder with the videos (see the layout at the top of this file)")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--username", default="test")
    p.add_argument("--password", default="test")
    p.add_argument("--verbose", action="store_true", help="log every request")
    ARGS = p.parse_args()
    if not os.path.isdir(ARGS.folder):
        sys.exit("No such folder: %s" % ARGS.folder)
    DB = scan(ARGS.folder)
    print("Found %d films in %d categories, %d series, %d live channels."
          % (len(DB["films"]), len(DB["film_cats"]), len(DB["shows"]), len(DB["live"])))
    srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler)
    print("On the Vita: Square (Add) > Xtream")
    print("  Server:   http://%s:%d" % (lan_ip(), ARGS.port))
    print("  Username: %s" % ARGS.username)
    print("  Password: %s" % ARGS.password)
    print("Press Ctrl+C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
