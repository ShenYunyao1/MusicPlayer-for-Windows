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
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
u.SetWindowTextW.restype = w.BOOL
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
k.OpenMutexW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
k.OpenMutexW.restype = w.HANDLE


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


def remote_manifests(output, playlist_id):
    matches = []
    for path in (output / "playlist").glob("server-*.playlist.remote.json"):
        try:
            if json.loads(path.read_text(encoding="utf-8")).get("id") == playlist_id:
                matches.append(path)
        except (OSError, json.JSONDecodeError):
            pass
    return matches


def protect_key(secret):
    data = (c.c_ubyte * len(secret)).from_buffer_copy(secret)
    blob, encrypted = Blob(len(data), data), Blob()
    assert crypt.CryptProtectData(c.byref(blob), "MusicPlayer2 server key", None, None, None, 1, c.byref(encrypted))
    try:
        return list(c.string_at(encrypted.data, encrypted.size))
    finally:
        k.LocalFree(encrypted.data)


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
        self.auth_status = 0
        self.empty_grants = False
        self.unavailable = set()

    def snapshot(self):
        tracks = [{"id": n, "title": "测试曲目" + n, "artist": "测试作者",
                   "lyrics": "[00:00.00]服务器歌词" + n, "path": "original-" + n + ".wav",
                   "format": "wav", "size": len(data), "content_hash": hashlib.sha256(data).hexdigest(),
                   "duration_ms": 3000, "bitrate_kbps": 128, "sample_rate": 8000,
                   "channels": 1, "bits_per_sample": 16} for n, data in self.audio.items()]
        long_tracks = [dict(tracks[0], id="browse-" + str(n), title="浏览曲目" + str(n)) for n in range(60)]
        result = {"revision": self.revision, "full_snapshot": True, "reset": True,
                "cursor": "desktop-" + str(self.revision), "has_more": False,
                "changed_tracks": [t for t in tracks + long_tracks if t["id"] not in self.unavailable],
                "playlists": [{"id": "main", "name": "完整测试歌单", "sort_order": 0, "track_ids": self.ids},
                              {"id": "browse", "name": "浏览状态测试", "sort_order": 1, "track_ids": [t["id"] for t in long_tracks]},
                              {"id": "race", "name": "下载竞态测试", "sort_order": 2, "track_ids": ["4", "5", "6"]},
                              {"id": "outage", "name": "断网混合缓存", "sort_order": 3, "track_ids": ["4", "1", "6"]}]}
        for playlist in result["playlists"]:
            playlist["unavailable_track_ids"] = [n for n in playlist["track_ids"] if n in self.unavailable]
            playlist["track_notes"] = {n: "保留备注" for n in playlist["track_ids"] if n in self.unavailable}
        if self.empty_grants:
            result.update(changed_tracks=[], playlists=[])
        return result

    def media(self):
        with self.lock:
            return [path.rsplit("/", 1)[1] for path in self.requests if path.startswith("/api/v2/media/")]


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
            if self.path.startswith("/api/v2/sync"):
                if self.server.auth_status:
                    body, status = b'{"error":{"code":"DENIED"}}', self.server.auth_status
                else:
                    body, status = json.dumps(self.server.snapshot()).encode(), 200
            elif self.path.startswith("/api/v2/media/"):
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
        assertions = [name for _, name, _ in windows if "Debug Assertion Failed" in name]
        if assertions:
            raise AssertionError("Player debug assertion: " + "\n".join(assertions))
        return windows

    def control(self, number):
        return next((hwnd for hwnd, _, ident in self.windows() if ident == number), 0)

    def send(self, hwnd, message, wp=0, lp=0):
        result = c.c_size_t()
        if not u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 5000, c.byref(result)):
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

    def library(self, expect_lists=True):
        self.command(32793)
        until(lambda: self.control(1083), "library tab")
        # The tab HWND exists while OnInitDialog is still creating earlier
        # pages. Sending a selection/close during that nested initialization can
        # destroy the dialog before its remaining child pointers are used.
        until(lambda: self.control(8110) and u.GetParent(u.GetParent(self.control(8110))) == self.control(1083),
              "server page registered in initialized tab control")
        self.server_tab(expect_lists)

    def server_tab(self, expect_lists=True):
        tab = self.control(1083)
        count = self.send(tab, 4868)
        # Let the tab control generate its own in-process notifications.
        self.send(tab, 4912, count - 1)  # TCM_SETCURFOCUS
        if expect_lists:
            until(lambda: self.send(self.control(8106), 395) > 0, "server playlists")

    def select_playlist(self, row):
        hwnd = self.control(8106)
        self.send(hwnd, 390, row)
        self.send(u.GetParent(hwnd), 273, 8106 | (1 << 16), hwnd)

    def local_playlist_tracks(self, name):
        tab = self.control(1083)
        self.send(tab, 4912, 1)  # Media Library's Playlist tab.
        songs = until(lambda: next((hwnd for hwnd, _, ident in self.windows()
                                   if ident == 1159 and u.IsWindowVisible(u.GetParent(hwnd))), 0), "local playlist song list")
        playlists = u.GetDlgItem(u.GetParent(songs), 1002)
        row = next((i for i in range(self.send(playlists, 4100))
                    if self.text_on(playlists, i, 1) == name), -1)
        assert row >= 0, "linked local playlist visible"
        search = u.GetDlgItem(u.GetParent(songs), 1103)
        assert u.SetWindowTextW(search, name), "filter linked playlist"
        until(lambda: self.send(songs, 4100) > 0, "local playlist tracks")
        return songs

    def text_on(self, hwnd, row, col=0):
        item = Item(mask=1, row=row, col=col, text=self.buffer + 256, capacity=1000)
        self.write(item)
        self.send(hwnd, 4171, row, self.buffer)
        value = c.create_unicode_buffer(1000)
        assert k.ReadProcessMemory(self.handle, self.buffer + 256, value, c.sizeof(value), None)
        return value.value

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
    build = Path(__file__).resolve().parents[1] / "build"
    if output == build or build in output.parents:
        raise ValueError("Desktop test output must be outside build/; use test-results/desktop-<name>.")
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
    protected = protect_key(b"desktop-fixture-key")
    (output / "remote-library.json").write_text(json.dumps({"url": f"http://127.0.0.1:{fixture.server_port}",
        "cache_directory": str(output / "cache"), "protected_key": protected}), encoding="utf-8")
    (output / "playlist").mkdir()
    if args.empty_default_playlist:
        (output / "playlist" / "default_playlist.playlist").write_text("", encoding="utf-8-sig")
    else:
        seed = output / "seed.wav"
        seed.write_bytes(wav(0))
        (output / "playlist" / "default_playlist.playlist").write_text(str(seed) + "\n", encoding="utf-8-sig")
    return output


def run(args):
    mutex = k.OpenMutexW(0x100000, False, "bXS1E7joK0Kh")
    if mutex:
        k.CloseHandle(mutex)
        raise RuntimeError("Close the existing MusicPlayer2 instance before desktop regression (global single-instance mutex).")
    fixture = Fixture()
    thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    thread.start()
    output = prepare(args, fixture)
    process = subprocess.Popen([str(output / "MusicPlayer2.exe")], cwd=output)
    desktop = None
    try:
        desktop = Desktop(process)
        desktop.library()
        if args.empty_default_playlist:
            assert desktop.send(desktop.control(1015), 4100) == 0, "empty default playlist"
            print("PASS empty default playlist startup without debug assertion", flush=True)
        assert desktop.send(desktop.control(8107), 4100) == 3
        assert fixture.media() == [], "browsing must not download audio"
        fixture.gates["1"] = threading.Event()
        desktop.click(8110)
        until(lambda: fixture.media() == ["1"], "first demand download")
        assert desktop.send(desktop.control(1015), 4100) == 3, "full playback queue"
        assert "测试" in desktop.text(1015, 0, 1), "friendly filename display"
        if args.empty_default_playlist:
            print("PASS empty default playlist switches to complete server queue without debug assertion", flush=True)
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
        main_manifests = remote_manifests(output, "main")
        assert len(main_manifests) == 1, "initial server playlist snapshot"
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
        manifest = main_manifests[0]
        original = manifest.read_bytes()
        fixture.ids = ["1", "2", "3", "4"]
        fixture.revision += 1
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8107), 4100) == 4, "updated browsing snapshot")
        until(lambda: desktop.send(desktop.control(1015), 4100) == 4, "refresh updates current playback queue")
        assert "测试曲目4" in desktop.text(1015, 3, 1), "new server track metadata shown immediately"
        assert "测试曲目1" in desktop.title(), "refresh retains current track"
        assert fixture.media() == ["1", "2", "3"], "refresh must not download new audio"
        assert remote_manifests(output, "main") == main_manifests, "refresh must reuse the linked local playlist"
        assert manifest.read_bytes() != original, "refresh must update the materialized playlist"
        local_songs = desktop.local_playlist_tracks("完整测试歌单")
        assert desktop.send(local_songs, 4100) == 4, "local Playlist tab reflects refreshed track count"
        assert "测试曲目4" in desktop.text_on(local_songs, 3, 1), "local Playlist tab resolves new track title"
        assert "测试作者" in desktop.text_on(local_songs, 3, 2), "local Playlist tab resolves new track artist"
        desktop.server_tab()
        desktop.command(32779)
        print("PASS cached zero HTTP, browse selection/scroll recovery, live linked playlist refresh", flush=True)

        fixture.unavailable = {"1", "4"}
        desktop.click(8108)
        until(lambda: "不可用" in desktop.text(8107, 0, 0), "unavailable placeholder visible")
        assert desktop.send(desktop.control(8107), 4100) == 4, "unavailable member must retain its position"
        desktop.command(32804)
        desktop.click(8110)
        until(lambda: "测试曲目2" in desktop.title(), "unavailable first track skipped to cached second track")
        desktop.command(32779)
        before = len(fixture.requests)
        desktop.click(8109)
        until(lambda: "离线保存完成" in next((name for _, name, ident in desktop.windows() if ident == 8105), ""),
              "offline save skips unavailable members")
        assert len(fixture.requests) == before, "offline save should only use cached available tracks"
        fixture.unavailable.clear()
        desktop.click(8108)
        until(lambda: desktop.text(8107, 0, 0) == "测试曲目1", "restored UUID visible")
        print("PASS unavailable placeholder retains order and skips playback; restoration reuses UUID", flush=True)

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
        assert not list((output / "cache").rglob("4-" + hashlib.sha256(fixture.audio["4"]).hexdigest() + ".wav"))
        assert not list((output / "cache").rglob("5-" + hashlib.sha256(fixture.audio["5"]).hexdigest() + ".wav"))
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

        fixture.auth_status = 401
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8106), 395) == 0, "denied key clears server browser")
        until(lambda: desktop.send(desktop.control(1015), 4100) == 0, "denied key clears current queue")
        fixture.auth_status = 0
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8106), 395) == 4, "re-enabled key refresh restores browser")
        desktop.select_playlist(0)
        desktop.click(8110)
        until(lambda: "测试曲目1" in desktop.title(), "same re-enabled key plays cached audio")
        fixture.empty_grants = True
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8106), 395) == 0, "empty grant reset clears browser")
        until(lambda: desktop.send(desktop.control(1015), 4100) == 0, "empty grant reset clears current queue")
        fixture.empty_grants = False
        desktop.click(8108)
        until(lambda: desktop.send(desktop.control(8106), 395) == 4, "regrant restores browser")
        print("PASS authorization denial and zero-grant reset clear UI/queue; same key revalidation recovers", flush=True)

        # Start from a different title: a preceding refresh can leave cached song
        # 1 selected, which otherwise satisfies the offline wait before playback.
        desktop.command(32783)
        until(lambda: "测试曲目2" in desktop.title(), "distinct track before offline transition")
        fixture.deny.clear()
        fixture.shutdown()
        fixture.server_close()
        count = len(fixture.requests)
        desktop.command(32804)
        desktop.select_playlist(3)
        desktop.click(8110)
        until(lambda: "测试曲目4" in desktop.text(1015, 0, 1), "outage playlist loaded")
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
        desktop.close()
        desktop = None
        config_path = output / "remote-library.json"
        original_config = config_path.read_bytes()
        config = json.loads(original_config)
        config["protected_key"] = protect_key(b"different-account-key")
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen([str(output / "MusicPlayer2.exe")], cwd=output)
        desktop = Desktop(process)
        desktop.library(expect_lists=False)
        until(lambda: any("HTTP" in name and "failed" in name for _, name, ident in desktop.windows() if ident == 8105),
              "new key cannot reuse another key's offline snapshot")
        assert desktop.send(desktop.control(1015), 4100) == 0, "old linked playlist opened as local audio under a new key"
        assert desktop.send(desktop.control(8106), 395) == 0, "old remote entries leaked to a new key"
        assert list((output / "cache").rglob("*.wav")), "key switching must not delete cached media bytes"
        desktop.close()
        desktop = None
        config_path.write_bytes(original_config)
        print("PASS different key cannot reopen another key's linked playlist or offline cache", flush=True)
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
        try:
            if desktop:
                desktop.close()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(5)
        fixture.shutdown()
        fixture.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New directory outside Windows Temp")
    parser.add_argument("--empty-default-playlist", action="store_true", help="Regress empty startup and switching from empty to server playback")
    run(parser.parse_args())
