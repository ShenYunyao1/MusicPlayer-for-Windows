"""Verify the distribution allowlist against a deliberately dirty runtime tree."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

from importlib.util import module_from_spec, spec_from_file_location


def main():
    root = Path(__file__).resolve().parents[1]
    results = root / "test-results"
    results.mkdir(exist_ok=True)
    core = ("MusicPlayer2.exe", "MusicPlayerUpdater.exe", "bass.dll", "bass_fx.dll", "tag.dll", "SciLexer.dll")
    spec = spec_from_file_location("build_layout", Path(__file__).with_name("test-build-layout.py"))
    layout = module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(layout)
    with tempfile.TemporaryDirectory(prefix="package check ", dir=results) as temporary:
        temporary = Path(temporary)
        build, output = temporary / "build", temporary / "dist"
        build.mkdir()
        for name in core:
            shutil.copy2(root / "build" / name, build / name)
        for name in ("config.ini", "remote-library.json", "MusicPlayer2.pdb", "global_cfg.ini",
                     "tests/private.json", "obj/private.obj", "playlist/private.playlist",
                     "remote-cache/private.wav", "preserved-data/private.json", "language/private.ini"):
            path = build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("DO_NOT_DISTRIBUTE_fixture_secret", encoding="utf-8")
        environment = dict(os.environ, MUSIC_PLAYER_NO_PAUSE="1")

        def package(wrapper=False, clean_caller=False):
            if clean_caller:
                quote = lambda value: "'" + str(value).replace("'", "''") + "'"
                script = (f"& {quote(root / 'package.ps1')} -BuildDirectory {quote(build)} -OutputDirectory {quote(output)}; "
                          "if ($LASTEXITCODE -ne 0) { throw 'Packaging did not return explicit success' }")
                return subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                                      cwd=temporary, env=environment, capture_output=True, timeout=90)
            if wrapper:
                command = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "call", str(root / "package.bat")]
            else:
                command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(root / "package.ps1")]
            return subprocess.run(command + ["-BuildDirectory", str(build), "-OutputDirectory", str(output)],
                                  cwd=temporary, env=environment, capture_output=True, timeout=90)

        result = package(wrapper=True)
        assert result.returncode == 0, result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        assert b"PACKAGE SUCCEEDED" in result.stdout
        clean_result = package(clean_caller=True)
        assert clean_result.returncode == 0, clean_result.stdout.decode(errors="replace") + clean_result.stderr.decode(errors="replace")
        archive_path = output / "MusicPlayer2.zip"
        expected = {"MusicPlayer2/" + name for name in core}
        expected.update("MusicPlayer2/" + name for name in ("global_cfg.ini", "LICENSE", "README.txt", "BUILD-INFO.txt", "PROGRAM-FILES.json"))
        for source, target in (("MusicPlayer2/language", "language"), ("MusicPlayer2/skins", "skins"),
                               ("third_party/runtime/common/Encoder", "Encoder")):
            expected.update("MusicPlayer2/" + target + "/" + p.relative_to(root / source).as_posix()
                            for p in (root / source).rglob("*") if p.is_file())
        with zipfile.ZipFile(archive_path) as archive:
            assert set(archive.namelist()) == expected, "unexpected or missing distribution entries"
            for name in core:
                assert archive.read("MusicPlayer2/" + name) == (build / name).read_bytes()
            assert archive.read("MusicPlayer2/global_cfg.ini") == (root / "third_party/runtime/defaults/global_cfg.ini").read_bytes()
            embedded = layout.compile_resource(build / "MusicPlayer2.exe").splitlines()[1].strip().decode()
            assert "Git: " + embedded in archive.read("MusicPlayer2/BUILD-INFO.txt").decode()
            for name in archive.namelist():
                assert b"DO_NOT_DISTRIBUTE_fixture_secret" not in archive.read(name), name
        digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        assert (output / "MusicPlayer2.zip.sha256").read_text().split()[0] == digest
        (build / "bass.dll").unlink()
        assert package().returncode != 0, "incomplete runtime was packaged"
        assert hashlib.sha256(archive_path.read_bytes()).hexdigest() == digest, "failure replaced a good ZIP"
        data = bytearray((root / "build/bass.dll").read_bytes())
        offset = struct.unpack_from("<I", data, 0x3c)[0]
        struct.pack_into("<H", data, offset+4, 0x14c)
        (build / "bass.dll").write_bytes(data)
        assert package().returncode != 0, "Win32 DLL accepted in x64 package"
        assert hashlib.sha256(archive_path.read_bytes()).hexdigest() == digest
        print("Package checks passed: explicit clean-shell success, BAT from foreign cwd/space paths, runtime allowlist, no private data, embedded Git, SHA-256, missing/wrong-architecture failure preserves existing ZIP.")


if __name__ == "__main__":
    main()
