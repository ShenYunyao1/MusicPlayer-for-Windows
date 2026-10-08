"""Run the real x64 player against an isolated HTTP fixture and portable profile.

Requires Windows with an interactive desktop and a built MusicPlayer2.exe.
No user profile, real server or media library is changed. Test artifacts remain
under --output for inspection; only the process launched here is controlled.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


u = c.WinDLL("user32", use_last_error=True)
k = c.WinDLL("kernel32", use_last_error=True)
crypt = c.WinDLL("crypt32", use_last_error=True)
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [CALLBACK, w.LPARAM]
u.EnumChildWindows.argtypes = [w.HWND, CALLBACK, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetDlgCtrlID.argtypes = [w.HWND]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetParent.argtypes = [w.HWND]
u.GetParent.restype = w.HWND
u.IsWindowVisible.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
u.SendMessageTimeoutW.restype = w.LPARAM
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.VirtualAllocEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD, w.DWORD]
k.VirtualAllocEx.restype = c.c_void_p
k.WriteProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
k.VirtualFreeEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD]
k.CloseHandle.argtypes = [w.HANDLE]
k.LocalFree.argtypes = [c.c_void_p]


class Blob(c.Structure):
    _fields_ = [("size", w.DWORD), ("data", c.POINTER(c.c_ubyte))]


crypt.CryptProtectData.argtypes = [c.POINTER(Blob), w.LPCWSTR, c.c_void_p, c.c_void_p, c.c_void_p, w.DWORD, c.POINTER(Blob)]


class Item(c.Structure):
    _fields_ = [("mask", w.UINT), ("row", c.c_int), ("col", c.c_int),
                ("state", w.UINT), ("state_mask", w.UINT), ("text", c.c_void_p),
                ("capacity", c.c_int), ("image", c.c_int), ("param", w.LPARAM),
                ("indent", c.c_int), ("group", c.c_int), ("columns", w.UINT),
                ("column_ptr", c.c_void_p), ("format_ptr", c.c_void_p), ("group_index", c.c_int)]


class Header(c.Structure):
    _fields_ = [("window", w.HWND), ("id", c.c_size_t), ("code", w.UINT)]


def until(predicate, label, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.08)
    raise AssertionError("Timed out: " + label)


def wav(number, seconds=3):
    out = io.BytesIO()
    with wave.open(out, "wb") as f:
        f.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        # Quiet valid PCM; distinct content identities without audible noise.
        f.writeframes(bytes([number, 0]) * (8000 * seconds))
    return out.getvalue()


class Fixture(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.audio = {str(n): wav(n) for n in range(1, 7)}
        self.requests = []
        self.gates = {}
        self.deny = set()
        self.active = self.maximum = 0
        self.lock = threading.Lock()
        self.revision = 1
        self.ids = ["1", "2", "3"]

    def snapshot(self):
        tracks = [{"id": n, "title": "测试曲目" + n, "artist": "测试作者",
                   "lyrics": "[00:00.00]服务器歌词" + n, "path": "original-" + n + ".wav",
                   "format": "wav", "size": len(data), "content_hash": hashlib.sha256(data).hexdigest(),
                   "duration_ms": 3000, "bitrate_kbps": 128, "sample_rate": 8000,
                   "channels": 1, "bits_per_sample": 16} for n, data in self.audio.items()]
        long_tracks = [dict(tracks[0], id="browse-" + str(n), title="浏览曲目" + str(n)) for n in range(60)]
        return {"revision": self.revision, "full_snapshot": True, "changed_tracks": tracks + long_tracks,
                "playlists": [{"id": "main", "name": "完整测试歌单", "track_ids": self.ids},
                              {"id": "browse", "name": "浏览状态测试", "track_ids": [t["id"] for t in long_tracks]},
                              {"id": "race", "name": "下载竞态测试", "track_ids": ["4", "5", "6"]},
                              {"id": "outage", "name": "断网混合缓存", "track_ids": ["4", "1", "6"]}]}

    def media(self):
        with self.lock:
            return [path.rsplit("/", 1)[1] for path in self.requests if path.startswith("/api/v1/media/")]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.requests.append(self.path)
            self.server.active += 1
            self.server.maximum = max(self.server.maximum, self.server.active)
        try:
            assert self.headers.get("X-API-Key") == "desktop-fixture-key"
            if self.path.startswith("/api/v1/sync"):
                body, status = json.dumps(self.server.snapshot()).encode(), 200
            elif self.path.startswith("/api/v1/media/"):
                number = self.path.rsplit("/", 1)[1]
                if number in self.server.gates:
                    self.server.gates[number].wait(12)
                body, status = (b"denied", 404) if number in self.server.deny else (self.server.audio[number], 200)
            else:
                body, status = b"{}", 404
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            with self.server.lock:
                self.server.active -= 1


class Desktop:
    def __init__(self, process):
        self.process = process
        self.handle = k.OpenProcess(0x38, False, process.pid)
        assert self.handle
        self.buffer = k.VirtualAllocEx(self.handle, None, 8192, 0x3000, 4)
        assert self.buffer
        self.main = until(lambda: next((hwnd for hwnd, name, _ in self.windows()
                                       if "MusicPlayer2" in name and not name.startswith("GDI+")
                                       and u.IsWindowVisible(hwnd) and not u.GetParent(hwnd)), None), "main window")
        if not (Path(process.args[0]).parent / "song_data.dat").exists():
            # The upstream first-run desktop-shortcut prompt is modal. Decline
            # it before opening the library, and only in our isolated process.
            until(lambda: any("是否要在桌面上" in name for _, name, _ in self.windows()), "first-run prompt")
            u.PostMessageW(u.GetParent(self.control(7)), 273, 7, 0)
            until(lambda: not any("是否要在桌面上" in name for _, name, _ in self.windows()), "first-run prompt dismissed")
        until(lambda: self.control(1015), "main playlist initialized")

    def windows(self):
        windows = []
        @CALLBACK
        def callback(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, c.byref(pid))
            if pid.value == self.process.pid:
                buf = c.create_unicode_buffer(2048)
                u.GetWindowTextW(hwnd, buf, len(buf))
                windows.append((hwnd, buf.value, u.GetDlgCtrlID(hwnd)))
            return True
        u.EnumWindows(callback, 0)
        for hwnd, _, _ in list(windows):
            u.EnumChildWindows(hwnd, callback, 0)
        return windows

    def control(self, number):
        return next((hwnd for hwnd, _, ident in self.windows() if ident == number), 0)

    def send(self, hwnd, message, wp=0, lp=0):
        result = c.c_size_t()
        if not u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 1500, c.byref(result)):
            raise AssertionError("UI did not respond: " + str(message))
        return result.value

    def command(self, number):
        assert u.PostMessageW(self.main, 273, number, 0)

    def click(self, number):
        self.send(self.control(number), 245)

    def title(self):
        buf = c.create_unicode_buffer(2048)
        u.GetWindowTextW(self.main, buf, len(buf))
        return buf.value

    def write(self, value):
        assert k.WriteProcessMemory(self.handle, self.buffer, c.byref(value), c.sizeof(value), None)

    def text(self, control, row, col=0):
        item = Item(mask=1, row=row, col=col, text=self.buffer + 256, capacity=1000)
        self.write(item)
        self.send(self.control(control), 4171, row, self.buffer)
        text = c.create_unicode_buffer(1000)
        assert k.ReadProcessMemory(self.handle, self.buffer + 256, text, c.sizeof(text), None)
        return text.value

    def select_track(self, row):
        item = Item(state=3, state_mask=3)
        self.write(item)
        self.send(self.control(8107), 4139, row, self.buffer)

    def library(self):
        self.command(32793)
        tab = until(lambda: self.control(1083), "library tab")
        count = self.send(tab, 4868)
        # Let the tab control generate its own in-process notifications.
        self.send(tab, 4912, count - 1)  # TCM_SETCURFOCUS
        until(lambda: self.send(self.control(8106), 395) > 0, "server playlists")

    def select_playlist(self, row):
        hwnd = self.control(8106)
        self.send(hwnd, 390, row)
        self.send(u.GetParent(hwnd), 273, 8106 | (1 << 16), hwnd)

    def close_library(self):
        tab = self.control(1083)
        self.send(u.GetParent(tab), 273, 2)
        until(lambda: not self.control(1083), "library closed")

    def close(self):
        if self.control(1083):
            self.close_library()
        self.command(32779)
        u.PostMessageW(self.main, 16, 0, 0)
        try:
            self.process.wait(20)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(5)
        k.VirtualFreeEx(self.handle, self.buffer, 0, 0x8000)
        k.CloseHandle(self.handle)


def prepare(args, fixture):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for name in ("MusicPlayer2.exe", "bass.dll", "bass_fx.dll", "tag.dll", "SciLexer.dll"):
        shutil.copy2(args.exe.parent / name, output / name)
    source = Path(__file__).resolve().parents[1] / "MusicPlayer2"
    for name in ("language", "skins"):
        shutil.copytree(source / name, output / name)
    (output / "global_cfg.ini").write_text("[config]\nportable_mode=true\n", encoding="utf-8")
    (output / "config.ini").write_text("""[config]
auto_play_when_start=false
show_playlist=true
playlist_display_format=0
use_media_trans_control=false
volume=0
fade_effect=false
repeat_mode=0
UI_selected=1
remember_last_position=true
[other]
no_sf2_warning=true
show_close_main_window_inquery=false
debug_log=3
[general]
language_=zh-CN
check_update_when_start=false
auto_download_lyric=false
auto_download_album_cover=false
[media_lib]
update_media_lib_when_start_up=false
[desktop_lyric]
show_desktop_lyric=false
[hot_key]
hot_key_enable=false
""", encoding="utf-8")
    data = (c.c_ubyte * len(b"desktop-fixture-key")).from_buffer_copy(b"desktop-fixture-key")
    blob, encrypted = Blob(len(data), data), Blob()
    assert crypt.CryptProtectData(c.byref(blob), "MusicPlayer2 server key", None, None, None, 1, c.byref(encrypted))
    protected = list(c.string_at(encrypted.data, encrypted.size))
    k.LocalFree(encrypted.data)
    (output / "remote-library.json").write_text(json.dumps({"url": f"http://127.0.0.1:{fixture.server_port}",
        "cache_directory": str(output / "cache"), "protected_key": protected}), encoding="utf-8")
    seed = output / "seed.wav"
    seed.write_bytes(wav(0))
    (output / "playlist").mkdir()
    (output / "playlist" / "default_playlist.playlist").write_text(str(seed) + "\n", encoding="utf-8-sig")
    return output


def run(args):
    fixture = Fixture()
    thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    thread.start()
    output = prepare(args, fixture)
    process = subprocess.Popen([str(output / "MusicPlayer2.exe")], cwd=output)
    desktop = None
    try:
        desktop = Desktop(process)
        desktop.library()
        assert desktop.send(desktop.control(8107), 4100) == 3
        assert fixture.media() == [], "browsing must not download audio"
        fixture.gates["1"] = threading.Event()
        desktop.click(8110)
        until(lambda: fixture.media() == ["1"], "first demand download")
        assert desktop.send(desktop.control(1015), 4100) == 3, "full playback queue"
        assert "测试" in desktop.text(1015, 0, 1), "friendly filename display"
        desktop.command(32778)  # Pause while first download is outstanding.
        time.sleep(.3)
        fixture.gates["1"].set()
        until(lambda: list((output / "cache").rglob("*.wav")), "first cache ready")
        time.sleep(4)
        assert fixture.media() == ["1"], "download completion must preserve pause"
        desktop.close_library()
        desktop.command(32778)
        until(lambda: fixture.media() == ["1", "2", "3"], "automatic full playlist playback", 15)
        desktop.command(32779)
        print("PASS complete queue, friendly filenames, no prefetch, pause and auto-next", flush=True)

        desktop.library()
        desktop.command(32808)
        desktop.click(8110)
        until(lambda: "测试曲目1" in desktop.title(), "same snapshot loaded")
        desktop.close_library()
        desktop.command(33167)  # Remove the current song from the local queue.
        confirmation = until(lambda: next((hwnd for hwnd, _, ident in desktop.windows() if ident == 6), 0), "remove confirmation")
        desktop.send(confirmation, 245)
        until(lambda: desktop.send(desktop.control(1015), 4100) == 2, "local queue edited")
        desktop.library()
        desktop.click(8110)
        until(lambda: desktop.send(desktop.control(1015), 4100) == 3, "same server snapshot restores complete queue")
        assert "测试曲目1" in desktop.text(1015, 0, 1)
        desktop.command(32779)
        desktop.close_library()
        print("PASS replay same snapshot restores full server playlist after local queue edits", flush=True)

        desktop.library()
        desktop.select_playlist(1)
        desktop.select_track(42)
        desktop.send(desktop.control(8107), 4115, 42, 0)
        time.sleep(.2)
        top = desktop.send(desktop.control(8107), 4135)
        desktop.close_library()
        desktop.library()
        assert desktop.send(desktop.control(8106), 392) == 1
        assert desktop.send(desktop.control(8107), 4108, -1 & 0xffffffffffffffff, 2) == 42
        assert desktop.send(desktop.control(8107), 4135) == top
        desktop.select_playlist(0)
        count = len(fixture.requests)
        desktop.command(32808)  # Keep the current cached first track looping.
        desktop.click(8110)
        until(lambda: "测试曲目1" in desktop.title(), "cached first track")
        time.sleep(1)
        assert len(fixture.requests) == count, "cache playback must not use HTTP"
        manifest = next((output / "playlist").glob("server-*.remote.json"))
        original = manifest.read_bytes()
        fixture.ids = ["1", "2"]
        fixture.revision += 1
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8107), 4100) == 2, "updated browsing snapshot")
        assert desktop.send(desktop.control(1015), 4100) == 3
        assert manifest.read_bytes() == original, "refresh must not replace playing snapshot"
        desktop.click(8110)
        until(lambda: desktop.send(desktop.control(1015), 4100) == 2, "explicit updated playlist playback")
        desktop.command(32779)
        print("PASS cached zero HTTP, browse selection/scroll recovery, refresh snapshot isolation", flush=True)

        desktop.select_playlist(2)
        desktop.command(32804)
        fixture.gates["4"] = threading.Event()
        fixture.gates["5"] = threading.Event()
        before = len(fixture.media())
        desktop.click(8110)
        until(lambda: fixture.media()[before:] == ["4"], "gated old track")
        desktop.command(32783)
        until(lambda: "测试曲目5" in desktop.title(), "next track selected during download")
        fixture.gates["4"].set()
        until(lambda: fixture.media()[before:] == ["4", "5"], "new track demand download")
        desktop.command(32779)
        time.sleep(.3)
        fixture.gates["5"].set()
        time.sleep(4)
        assert "测试曲目5" in desktop.title(), "old completion must not replace the next track"
        assert fixture.media()[before:] == ["4", "5"], "stop must not restart or advance playback"
        assert not list((output / "cache").rglob(hashlib.sha256(fixture.audio["4"]).hexdigest() + ".wav"))
        assert not list((output / "cache").rglob(hashlib.sha256(fixture.audio["5"]).hexdigest() + ".wav"))
        print("PASS next/stop download races, stale results discarded", flush=True)

        fixture.gates.pop("4")
        fixture.gates.pop("5")
        fixture.deny = {"4", "5", "6"}
        for mode in (32804, 32805, 32806, 32807, 32808, 32809):
            before = len(fixture.media())
            desktop.command(mode)
            desktop.click(8110)
            until(lambda: len(fixture.media()) > before, "failed playlist started")
            time.sleep(4)
            failed = fixture.media()[before:]
            assert 1 <= len(failed) <= 3 and len(set(failed)) == len(failed), (mode, failed)
            time.sleep(1)
            assert fixture.media()[before:] == failed, "all failures must stop even in looping modes"
        print("PASS all-failed bounded stop in all six playback modes", flush=True)

        fixture.deny.clear()
        fixture.shutdown()
        fixture.server_close()
        count = len(fixture.requests)
        desktop.command(32804)
        desktop.select_playlist(3)
        desktop.click(8110)
        until(lambda: "测试曲目1" in desktop.title(), "offline missing track skipped to cached track", 20)
        desktop.close_library()
        time.sleep(4)
        assert "测试曲目6" in desktop.title(), "order playback must stop at its uncached end"
        assert len(fixture.requests) == count, "offline playback must reuse cached audio"
        desktop.library()
        assert desktop.send(desktop.control(8106), 392) == 3
        desktop.command(32779)
        desktop.close()
        desktop = None
        process = subprocess.Popen([str(output / "MusicPlayer2.exe")], cwd=output)
        desktop = Desktop(process)
        desktop.library()
        assert desktop.send(desktop.control(8106), 392) == 3
        assert desktop.send(desktop.control(8107), 4100) == 3
        assert desktop.send(desktop.control(1015), 4100) == 3
        assert len(fixture.requests) == count, "offline restart must use persistent snapshots"
        print("PASS real server outage, cached fallback, no order wrap, offline restart recovery", flush=True)
        assert fixture.maximum == 1, "API Key requests must be serialized"
        print("Artifacts:", output, flush=True)
    except Exception:
        if desktop:
            (output / "windows.json").write_text(json.dumps(desktop.windows(), ensure_ascii=False, indent=2), encoding="utf-8")
            print("Current title:", desktop.title(), flush=True)
        print("Requests:", fixture.requests, flush=True)
        print("Artifacts:", output, flush=True)
        raise
    finally:
        for gate in fixture.gates.values():
            gate.set()
        if desktop:
            desktop.close()
        elif process.poll() is None:
            process.terminate()
            process.wait(5)
        fixture.shutdown()
        fixture.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New directory outside Windows Temp")
    run(parser.parse_args())
