"""Opt-in progress-bar desktop regression using only an isolated portable profile.

Requires an interactive Windows desktop and Pillow. Never touches a real server.
"""
import argparse
from collections import Counter
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import threading
import time
from PIL import Image, ImageGrab
from remote_playback_desktop import Desktop, Fixture, Handler, prepare, until, wav, u, k

u.SetProcessDPIAware()  # Keep Win32 window rectangles and captured physical pixels aligned.
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.SetForegroundWindow.argtypes = [w.HWND]
u.ScreenToClient.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]


class SlowHandler(Handler):
    def do_GET(self):
        number = self.path.rsplit("/", 1)[-1]
        gate = self.server.stream_gates.get(number)
        if not self.path.startswith("/api/v2/media/") or gate is None:
            return super().do_GET()
        assert self.headers.get("X-API-Key") == "desktop-fixture-key"
        with self.server.lock:
            self.server.requests.append(self.path)
        body = self.server.audio[number]
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            cut = len(body) // 4
            self.wfile.write(body[:cut])
            self.wfile.flush()
            self.server.partial.set()
            gate.wait(40)
            self.wfile.write(body[cut:])
        except (BrokenPipeError, ConnectionResetError):
            pass


def capture(hwnd, output, name, color):
    u.ShowWindow(hwnd, 9)
    u.SetForegroundWindow(hwnd)
    # Foreground activation can be denied when a user is interacting elsewhere.
    # Temporarily raise only our isolated test window for a reliable capture.
    u.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x53)
    time.sleep(.6)
    rect = w.RECT()
    assert u.GetWindowRect(hwnd, c.byref(rect))
    try:
        image = ImageGrab.grab((rect.left, rect.top, rect.right, rect.bottom))
    finally:
        u.SetWindowPos(hwnd, -2, 0, 0, 0, 0, 0x13)
    image.save(output / (name + ".png"))
    pixels = [(x, y) for y in range(image.height) for x in range(image.width)
              if image.getpixel((x, y))[:3] == color]
    assert len(pixels) >= 8, name + " has no visible transfer bar"
    return rect, pixels


def run(args):
    mutex = k.OpenMutexW(0x100000, False, "bXS1E7joK0Kh")
    if mutex:
        k.CloseHandle(mutex)
        raise RuntimeError("Close the existing player before this isolated desktop test")
    fixture = Fixture()
    fixture.RequestHandlerClass = SlowHandler
    fixture.audio["1"] = wav(1, 30)
    fixture.audio["2"] = wav(2, 30)
    fixture.stream_gates = {"1": threading.Event(), "2": threading.Event()}
    fixture.partial = threading.Event()
    thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    thread.start()
    output = prepare(args, fixture)
    if args.slider:
        skin=output / "skins" / "ui3.xml"
        skin.write_text(skin.read_text(encoding="utf-8").replace("<progressBar", "<sliderProgressBar"), encoding="utf-8")
        config=output / "config.ini"
        config.write_text(config.read_text(encoding="utf-8").replace("UI_selected=1", "UI_selected=2"), encoding="utf-8")
    process = subprocess.Popen([str(output / "MusicPlayer2.exe")], cwd=output)
    desktop = None
    try:
        desktop = Desktop(process)
        desktop.library()
        desktop.click(8110)
        assert fixture.partial.wait(12), "foreground download not started"
        desktop.close_library()
        rect, pixels = capture(desktop.main, output, "foreground-download", (54, 156, 244))
        # Click and drag the download bar: it must remain a transfer display.
        rows=Counter(y for _, y in pixels)
        y=max(row for row, count in rows.items() if count>=8)
        xs=sorted(x for x, row in pixels if row==y)
        bar_origin=(xs[0],y)
        x=xs[len(xs)//2]
        point = w.POINT(rect.left + x, rect.top + y)
        ui=desktop.control(1176)
        assert ui, "player UI child"
        u.ScreenToClient(ui, c.byref(point))
        pos = (point.y << 16) | (point.x & 0xffff)
        desktop.send(ui, 512, 0, pos)
        desktop.send(ui, 513, 1, pos)
        desktop.send(ui, 512, 1, pos + 90)
        desktop.send(ui, 514, 0, pos + 90)
        desktop.command(32788)  # Fast-forward and AB hotkeys must also be ignored while downloading.
        desktop.command(33153)
        capture(desktop.main, output, "foreground-after-click", (54, 156, 244))
        desktop.command(32872)  # Mini mode uses the same transfer snapshot.
        mini = until(lambda: next((h for h, title, _ in desktop.windows()
                                  if h != desktop.main and title.endswith("MusicPlayer2") and u.IsWindowVisible(h)), 0), "mini mode")
        capture(mini, output, "mini-download", (54, 156, 244))
        desktop.send(mini, 273, 33005)
        until(lambda: u.IsWindowVisible(desktop.main), "restore main mode")
        fixture.stream_gates["1"].set()
        until(lambda: list((output / "cache").rglob("*.wav")), "verified first audio")
        time.sleep(.5)
        desktop.command(32808)  # Loop the cached current track during offline saving.
        desktop.library()
        fixture.partial.clear()
        until(lambda: u.IsWindowEnabled(desktop.control(8109)), "offline button ready")
        desktop.click(8109)
        if not fixture.partial.wait(12):
            raise AssertionError("background download not started: " + repr(fixture.requests) + " " +
                                 repr([(title, ident) for _, title, ident in desktop.windows() if ident==8105]))
        desktop.close_library()
        capture(desktop.main, output, "background-offline", (245, 164, 64))
        fixture.stream_gates["2"].set()
        until(lambda: len(list((output / "cache").rglob("*.wav"))) == 3, "batch completes after page closes")
        desktop.command(32778)  # Pause the opened stream while the same UUID changes content.
        old_audio=next((output / "cache").rglob("1-*.wav"))
        old_bytes=old_audio.read_bytes()
        fixture.audio["1"]=wav(7, 31)
        fixture.revision+=1
        desktop.library()
        before=len(fixture.media())
        desktop.click(8108)
        until(lambda: u.IsWindowEnabled(desktop.control(8108)), "content replacement refresh")
        time.sleep(.3)
        assert old_audio.read_bytes()==old_bytes, "sync replaced the opened audio cache"
        assert len(fixture.media())==before, "sync downloaded replacement audio for a paused UUID"
        assert "测试曲目1" in desktop.title(), "same UUID was not retained after audio hash change"
        desktop.close_library()
        capture(desktop.main, output, "hash-update-paused", (0, 117, 211))
        desktop.command(32778)
        time.sleep(1.3)
        assert "测试曲目1" in desktop.title(), "resuming the old stream used the missing replacement cache path"
        capture(desktop.main, output, "hash-update-resumed", (0, 117, 211))
        def filled_width(name):
            image=Image.open(output / (name+".png"))
            x,y=bar_origin
            color=image.getpixel((x,y))
            end=x
            while end<image.width and image.getpixel((end,y))==color:
                end+=1
            return end-x
        assert filled_width("hash-update-resumed")>filled_width("hash-update-paused")+4, "old stream did not resume its playback clock"
        print("PASS blue foreground progress, click/drag isolation, mini progress, orange background batch and page-independent completion", flush=True)
        print("PASS same UUID content update keeps opened cache intact without downloading replacement", flush=True)
    finally:
        for gate in fixture.stream_gates.values():
            gate.set()
        if desktop:
            desktop.close()
        elif process.poll() is None:
            process.terminate()
            process.wait(5)
        fixture.shutdown()
        fixture.server_close()
        thread.join(2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--empty-default-playlist", action="store_true")
    parser.add_argument("--slider", action="store_true", help="Use an isolated skin with slider-style progress")
    run(parser.parse_args())
