"""Run the native v2 client against an isolated, freshly initialized real server.

No requests are made to the user's instance; credentials are generated fixture
values and are passed to the native process through stdin, never command lines.
"""
import argparse
import base64
import http.cookiejar
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import wave


def wait_for(callback, seconds=90):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = callback()
            if value:
                return value
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(.1)
    raise AssertionError("isolated server did not reach the expected state")


class Account:
    def __init__(self, base):
        self.base, self.csrf = base, ""
        self.http = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def call(self, path, method="GET", body=None, version=None):
        headers = {"Origin": self.base, "X-CSRF-Token": self.csrf, "Content-Type": "application/json"}
        if version is not None:
            headers["If-Match"] = str(version)
        request = urllib.request.Request(self.base + "/api/v2/" + path,
                                         None if body is None else json.dumps(body).encode(), headers, method=method)
        with self.http.open(request, timeout=20) as response:
            data = response.read()
            return json.loads(data) if data else None

    def login(self, username, password):
        identity = self.call("auth/login", "POST", {"username": username, "password": password})
        self.csrf = identity["csrf_token"]


def run(args):
    args.server, args.exe = args.server.resolve(), args.exe.resolve()
    parent = Path(__file__).resolve().parents[2]
    terminal = parent / "server" / "tests" / "admin-terminal.py"
    with tempfile.TemporaryDirectory(prefix="mp2-server-v2-") as directory:
        directory = Path(directory)
        media = directory / "media"
        media.mkdir()
        for number in range(105):
            audio = io.BytesIO()
            with wave.open(audio, "wb") as stream:
                stream.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
                stream.writeframes(bytes([number, 0]) * 400)
            (media / f"Track-{number:03}.wav").write_bytes(audio.getvalue())
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
        (media / "Track-000.png").write_bytes(png)
        (media / "Track-000.lrc").write_text("[00:00.00]integration lyrics", encoding="utf-8")
        database = directory / "library.sqlite"
        password = "isolated integration administrator"
        initialize = subprocess.run([sys.executable, str(terminal), str(args.server), str(database), "admin"],
                                    input=password + "\n", capture_output=True, encoding="utf-8", timeout=35)
        assert initialize.returncode == 0, initialize.stdout + initialize.stderr
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        environment = dict(os.environ)
        environment["PATH"] = r"D:\Program\ffmpeg\bin" + os.pathsep + environment["PATH"]
        with (directory / "server.log").open("wb") as log:
            server = subprocess.Popen([str(args.server), "-addr", f"127.0.0.1:{port}", "-media-root", str(media),
                                       "-data", str(database)], stdout=log, stderr=log, env=environment,
                                      creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                health = wait_for(lambda: json.load(urllib.request.urlopen(base + "/api/v2/health", timeout=2)))
                assert health["version"] == "0.10.0", "integration requires the latest server contract"
                admin = Account(base)
                admin.login("admin", password)
                tracks = wait_for(lambda: (lambda t: t if len(t) == 105 else None)(admin.call("tracks")))
                first = next(t for t in tracks if t["title"] == "Track-000")
                assert first.get("cover_id") and first.get("lyrics"), "real scan did not ingest sidecars"
                member = admin.call("admin/users", "POST", {"username": "player-test", "display_name": "Player test"})
                user = Account(base)
                user.login("player-test", member["temporary_password"])
                personal_password = "personal integration password"
                user.call("auth/password", "PUT", {"old_password": member["temporary_password"], "new_password": personal_password})
                user.login("player-test", personal_password)
                ids = [first["id"], next(t["id"] for t in tracks if t["id"] != first["id"])]
                playlist_id = str(uuid.uuid4())
                playlist = user.call("playlists", "POST", {"id": playlist_id, "name": "Private player", "track_ids": ids,
                                                            "track_notes": {first["id"]: "retained cue"}})
                key = user.call("auth/keys", "POST", {"name": "Integration player", "scopes": ["library:read", "media:read"],
                                                       "allowed_playlist_ids": ["library", playlist_id], "max_concurrency": 1, "max_downloads": 1})

                def native(local=False, download=False, secret=None):
                    request = {"url": base, "key": key["secret"] if secret is None else secret,
                               "cache": str(directory / "cache"), "local_only": local, "download": download}
                    process = subprocess.run([str(args.exe), "--server"], input=json.dumps(request),
                                             capture_output=True, encoding="utf-8", timeout=60)
                    assert process.returncode == 0, process.stderr
                    return json.loads(process.stdout)

                synced = native()
                assert synced["success"] and not synced["offline"] and len(synced["playlists"]) == 2, synced
                library = next(p for p in synced["playlists"] if p["id"] == "library")
                assert len(library["tracks"]) == 105, "native client lost a real snapshot page"
                print("PASS real 0.10.0 server: account first password, account key and 100-item cursor pagination", flush=True)
                # Narrow grants so media validation only downloads the selected private list.
                user.call("auth/keys/" + key["id"], "PATCH", {"allowed_playlist_ids": [playlist_id]})
                cached = native(download=True)
                assert cached["success"] and len(cached["playlists"]) == 1, cached
                p = cached["playlists"][0]
                assert p["track_ids"] == ids and p["notes"][ids[0]] == "retained cue"
                song = next(t for t in p["tracks"] if t["id"] == first["id"])
                assert song["cached"] and song["hash"] == first["content_hash"] and song["artwork"] and song["lyric_file"], song
                print("PASS real media hash, cover, lyrics, private order/notes and permission reset", flush=True)
                # Administrator removal retains private membership and restores under the same UUID.
                admin.call("tracks/" + first["id"], "DELETE", version=first["version"])
                removed = native(download=True)
                p = removed["playlists"][0]
                assert p["track_ids"] == ids and p["notes"][ids[0]] == "retained cue"
                assert p["tracks"][0]["unavailable"] and not p["tracks"][0]["cached"]
                tombstone = next(t for t in admin.call("admin/removed") if t["id"] == first["id"])
                admin.call("admin/removed/" + first["id"], "POST", version=tombstone["version"])
                assert native(download=True)["playlists"][0]["tracks"][0]["cached"]
                print("PASS real delta removal/restoration preserves UUID, membership and note", flush=True)
                current = next(u for u in admin.call("admin/users") if u["id"] == member["user"]["id"])
                disabled = admin.call("admin/users/" + current["id"], "PATCH", {"enabled": False}, current["version"])
                rejected = native()
                assert not rejected["success"] and rejected["blocked"] and not rejected["offline"], rejected
                assert not native(local=True)["success"], "disabled account reused saved offline snapshot"
                admin.call("admin/users/" + current["id"], "PATCH", {"enabled": True}, disabled["version"])
                assert native()["success"], "same key did not resume after account re-enable"
                user.login("player-test", personal_password)
                user.call("auth/keys/" + key["id"], "PATCH", {"allowed_playlist_ids": []})
                empty = native()
                assert empty["success"] and empty["playlists"] == [], empty
                user.call("auth/keys/" + key["id"], "PATCH", {"allowed_playlist_ids": [playlist_id]})
                assert native()["success"]
                print("PASS real disable/re-enable same key, durable denial and zero-grant reset", flush=True)
                server.terminate()
                server.wait(10)
                offline = native(download=True)
                assert offline["success"] and offline["offline"] and all(t["cached"] for t in offline["playlists"][0]["tracks"]), offline
                print("PASS real server outage and native process restart reuse complete cached snapshot/audio", flush=True)
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    run(parser.parse_args())
