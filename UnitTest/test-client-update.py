"""Opt-in real desktop startup/exit contract test; never reads the user's profile.

Run after an x64 build with all other player instances closed. Not an audio test.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
user = ctypes.WinDLL('user32', use_last_error=True)
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.IsWindowVisible.argtypes = [wintypes.HWND]
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def windows(pid):
    result = []
    @CALLBACK
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            result.append(hwnd)
        return True
    user.EnumWindows(visit, 0)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--desktop', action='store_true', required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--startup-check', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'test-results' / ('updater-desktop-' + str(time.time_ns()))
    install = output / '中文 安装目录'
    install.mkdir(parents=True)
    for name in ('MusicPlayer2.exe', 'MusicPlayerUpdater.exe', 'bass.dll', 'bass_fx.dll', 'tag.dll', 'SciLexer.dll'):
        shutil.copy2(args.build / name, install / name)
    for name in ('language', 'skins'):
        shutil.copytree(args.build / name, install / name)
    (install / 'global_cfg.ini').write_text('[config]\nportable_mode=true\n')
    check = 'true' if args.startup_check else 'false'
    (install / 'config.ini').write_text(f'[general]\ncheck_update_when_start={check}\n[config]\nauto_play_when_start=false\nminimize_to_notify_icon=true\n', encoding='utf-8')
    (install / 'updates').mkdir()
    (install / 'updates/check.json').write_text(json.dumps({'checked': int(time.time())}))
    # Suppress the first-run shortcut question without relying on private profiles.
    (install / 'song_data.dat').write_bytes(b'')
    (install / '.musicplayer-update').mkdir()
    token = 'a' * 64
    environment = dict(os.environ, MUSICPLAYER_UPDATE_NONCE=token)
    if args.startup_check:
        environment.pop('MUSICPLAYER_UPDATE_NONCE', None)
    player = subprocess.Popen([str(install / 'MusicPlayer2.exe')], cwd=install, env=environment)
    try:
        ready = install / '.musicplayer-update/ready.txt'
        if args.startup_check:
            ready = install / '.musicplayer-updater.lock'
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline and not ready.exists() and player.poll() is None:
            time.sleep(.2)
        assert ready.exists(), f'Player did not acknowledge startup; process={player.poll()}, test directory={output}'
        expected_version = json.loads((ROOT / 'client-release.json').read_text())['version']
        stamp = (args.build / 'obj/MusicPlayer2/x64/Release/compile_time.txt').read_text().splitlines()[1].strip()
        if not args.startup_check:
            assert ready.read_text().splitlines() == [token, expected_version, stamp], 'startup identity mismatch'
        visible = windows(player.pid)
        assert visible, 'main window not visible'
        for hwnd in visible:
            user.PostMessageW(hwnd, 0x8000 + 214, 0x4D505550, 0)
        player.wait(timeout=35)
        assert player.returncode == 0, f'normal update exit failed: {player.returncode:#x}'
        assert (install / 'config.ini').is_file() and (install / 'global_cfg.ini').is_file()
        (output / 'result.json').write_text(json.dumps({'version': expected_version, 'commit': stamp,
            'startup_acknowledged': True, 'dedicated_exit': True, 'unicode_space_path': True}, indent=2))
        print('PASS real MFC startup acknowledgement, version/commit, Unicode/space path and exit bypassing tray; no audio verification.')
        print(output)
    finally:
        if player.poll() is None:
            player.kill()
            player.wait()


if __name__ == '__main__':
    main()
