"""Black-box WinHTTP/cache regression against a temporary local fixture server."""

import argparse
import base64
import hashlib
import json
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
PNG2 = PNG + b"fixture second artwork"
GIF = base64.b64decode("R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==")
AUDIO = b"audio fixture verified by content hash" * 8192
MODES = {
    "png": (PNG, "image/png"), "png2": (PNG2, "image/png"),
    "gif": (GIF, "image/gif"), "none": (b"", ""),
    "removed": (b"", ""),
    "corrupt": (b"expected image", "image/png"),
    "oversized": (b"a" * ((8 << 20) + 1), "image/png"),
    "denied": (PNG + b"denied artwork", "image/png"),
}


def digest(value):
    return hashlib.sha256(value).hexdigest()


class FixtureServer(ThreadingHTTPServer):
    mode = "png"
    revision = 1
    auth_failures = 0
    requests = 0
    scenario = ""
    sync_calls = 0

    def __init__(self, *args):
        super().__init__(*args)
        self.connection_lock = threading.Lock()
        self.connection_active = self.connection_maximum = 0

    def track(self):
        data, mime = MODES[self.mode]
        return {
            "id": "fixture", "title": "fixture", "artist": "fixture artist",
            "lyrics": "[00:01.00]fixture lyrics", "size": len(AUDIO),
            "content_hash": digest(AUDIO), "format": "mp3",
            "duration_ms": 12345, "bitrate_kbps": 320, "sample_rate": 44100,
            "channels": 2, "bits_per_sample": 16,
            "cover_id": digest(data) if data else "", "cover_mime": mime,
            "cover_source": "external" if data else "",
        }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, body, status=200, mime="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.headers.get("X-API-Key") != "fixture-key":
            self.server.auth_failures += 1
            return self.reply({}, 401)
        path = urlsplit(self.path)
        if path.path.startswith("/api/v1/"):
            return self.reply({"code": "LEGACY_REMOVED"}, 410)
        if path.path == "/api/v2/playlists":
            with self.server.connection_lock:
                self.server.connection_active += 1
                self.server.connection_maximum = max(self.server.connection_maximum, self.server.connection_active)
            try:
                time.sleep(.1)
                self.reply([])
            finally:
                with self.server.connection_lock:
                    self.server.connection_active -= 1
            return
        if path.path == "/api/v2/tracks/fixture" and parse_qs(path.query).get("mode") == ["stats"]:
            value = self.server.track()
            value["path"] = str(self.server.requests)
            return self.reply(value)
        self.server.requests += 1
        if path.path == "/api/v2/tracks/fixture":
            mode = parse_qs(path.query).get("mode", ["png"])[0]
            if mode.startswith("case-"):
                self.server.scenario = mode[5:]
                self.server.sync_calls = 0
                return self.reply(self.server.track())
            self.server.scenario = ""
            if mode == "stop":
                self.reply(self.server.track())
                threading.Thread(target=self.stop_server, daemon=True).start()
                return
            self.server.mode = mode
            self.server.revision += 1
            return self.reply(self.server.track())
        if path.path == "/api/v2/media/fixture":
            return self.reply(AUDIO, mime="audio/mpeg")
        if path.path.startswith("/api/v2/covers/"):
            expected = self.server.track()["cover_id"]
            if path.path.rsplit("/", 1)[1] != expected or self.server.mode == "denied":
                return self.reply({}, 403)
            data, mime = MODES[self.server.mode]
            if self.server.mode == "corrupt":
                data = PNG
            return self.reply(data, mime=mime)
        if path.path == "/api/v2/sync":
            if self.server.scenario:
                return self.protocol_page(parse_qs(path.query).get("cursor", [""])[0])
            removed = self.server.mode == "removed"
            return self.reply({
                "revision": self.server.revision, "full_snapshot": True, "reset": True,
                "cursor": "fixture-" + str(self.server.revision), "has_more": False,
                "changed_tracks": [] if removed else [self.server.track()],
                "playlists": [{"id": "fixture-playlist", "name": "fixture", "track_ids": [] if removed else ["fixture"]}],
            })
        self.reply({}, 404)

    def protocol_page(self, cursor):
        scenario = self.server.scenario
        self.server.sync_calls += 1
        calls = self.server.sync_calls
        page = {"revision": 100, "full_snapshot": False, "reset": False,
                "cursor": "done-" + scenario, "has_more": False,
                "changed_tracks": [], "playlists": []}
        if scenario in ("unauthorized", "forbidden", "limited"):
            return self.reply({"error": {"code": "DENIED"}},
                              {"unauthorized": 401, "forbidden": 403, "limited": 429}[scenario])
        if scenario == "restart-always" or scenario == "restart" and calls == 1:
            return self.reply({"error": {"code": "SYNC_RESTART"}}, 409)
        if scenario == "bad-json":
            return self.reply(b"not json")
        if scenario in ("paged", "bad-last", "restart", "network-last", "cycle"):
            if calls == 1 or scenario == "restart" and calls == 2:
                page.update(full_snapshot=True, reset=True, has_more=True, cursor="page+2/=?",
                            changed_tracks=[dict(self.server.track(), id="a")])
            else:
                assert cursor == "page+2/=?", "cursor was not URL encoded or not followed"
                if scenario == "bad-last":
                    return self.reply({"error": {"code": "FAIL"}}, 500)
                if scenario == "network-last":
                    self.connection.close()
                    return
                if scenario == "cycle":
                    page.update(has_more=True, cursor="page+2/=?")
                else:
                    page.update(full_snapshot=True, changed_tracks=[dict(self.server.track(), id="b")],
                                playlists=[{"id": "p", "name": "Paged", "sort_order": 1,
                                            "version": 7, "subscription_version": 9,
                                            "track_ids": ["b", "a"], "track_notes": {"a": "keep note"}},
                                           {"id": "q", "name": "First", "sort_order": 0, "track_ids": []}])
        elif scenario == "noop":
            assert cursor == "done-paged", "empty delta did not use committed cursor"
        elif scenario == "delta":
            assert cursor == "done-noop", "delta did not use renewed cursor"
            page.update(changed_tracks=[dict(self.server.track(), id="a", title="Updated")],
                        playlists=[{"id": "p", "name": "Renamed", "sort_order": 1,
                                    "track_ids": ["a", "b"], "track_notes": {"b": "retained"}}],
                        deleted_tracks=["b"], deleted_playlists=["q"])
        elif scenario == "restore":
            page.update(changed_tracks=[dict(self.server.track(), id="b")])
        elif scenario == "empty":
            page.update(full_snapshot=True, reset=True)
        return self.reply(page)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()
    server = FixtureServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="musicplayer2-cover-") as directory:
            result = subprocess.run([args.exe, f"http://127.0.0.1:{server.server_port}", directory], timeout=45)
            if result.returncode:
                raise SystemExit(result.returncode)
            if server.auth_failures:
                raise SystemExit("client did not authenticate all fixture requests")
            if server.connection_maximum != 1:
                raise SystemExit("connection checks overlapped: " + str(server.connection_maximum))
            print("PASS serialized connection checks across client instances", flush=True)
    finally:
        if thread.is_alive():
            server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
