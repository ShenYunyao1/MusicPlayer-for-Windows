"""Release gate regressions; no compiler, credentials or network required."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("client_release", ROOT / "scripts/client-release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
COMMIT = "a" * 40


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for source in ("MusicPlayer2/language", "MusicPlayer2/skins", "third_party/runtime/common/Encoder",
                       "third_party/runtime/defaults", "Documents"):
            (self.root / source).mkdir(parents=True)
        self.config = release.load_config(ROOT)
        (self.root / "client-release.json").write_text(json.dumps(self.config), encoding="utf-8")
        (self.root / "Documents/client-release-notes.md").write_text("# " + self.config["version"], encoding="utf-8")
        (self.root / "third_party/runtime/defaults/global_cfg.ini").write_bytes(b"portable-fixture")
        pe = bytearray(128)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3c, 64)
        pe[64:68] = b"PE\0\0"
        struct.pack_into("<H", pe, 68, 0x8664)
        self.entries = {name: b"fixture" for name in release.expected_entries(self.root)}
        self.entries.update({"MusicPlayer2/" + name: bytes(pe) for name in release.CORE})
        self.entries["MusicPlayer2/global_cfg.ini"] = b"portable-fixture"
        self.entries["MusicPlayer2/PROGRAM-FILES.json"] = b"{}"
        self.entries["MusicPlayer2/BUILD-INFO.txt"] = (
            f"Release: {self.config['version']}\nPlatform: windows-x64\nGit: {COMMIT}\n"
            f"MusicPlayer2.exe SHA-256: {hashlib.sha256(pe).hexdigest()}\n").encode()

    def write_package(self):
        archive = self.root / "MusicPlayer2.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for name, content in self.entries.items():
                output.writestr(name, content)
        (self.root / "MusicPlayer2.zip.sha256").write_text(
            hashlib.sha256(archive.read_bytes()).hexdigest() + "  MusicPlayer2.zip\n", encoding="utf-8")

    def verify(self, commit=COMMIT):
        return release.verify_package(self.root, self.root, self.config, commit)

    def test_valid_release_and_tag(self):
        self.write_package()
        self.assertEqual(self.verify()["commit"], COMMIT)
        release.load_config(self.root, "refs/tags/v" + self.config["version"])
        release.load_config(self.root, "refs/heads/master")

    def test_wrong_tag(self):
        with self.assertRaisesRegex(ValueError, "tag differs"):
            release.load_config(self.root, "refs/tags/v999.0.0")

    def test_wrong_release_notes(self):
        (self.root / "Documents/client-release-notes.md").write_text("# old release", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "notes version"):
            release.load_config(self.root)

    def test_extra_config_fields(self):
        config = dict(self.config, unexpected="injected-output")
        (self.root / "client-release.json").write_text(json.dumps(config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "configuration fields"):
            release.load_config(self.root)

    def test_unpinned_server_and_output_injection(self):
        for field, value in (("server_commit", "main"), ("version", "1.0.0\nextra=x"),
                             ("server_repository", "owner/repo\nextra=x"), ("server_schema", True)):
            config = copy.deepcopy(self.config)
            config[field] = value
            (self.root / "client-release.json").write_text(json.dumps(config), encoding="utf-8")
            with self.subTest(field=field), self.assertRaises(ValueError):
                release.load_config(self.root)

    def test_checksum_and_commit(self):
        self.write_package()
        with self.assertRaisesRegex(ValueError, "commit mismatch"):
            self.verify("b" * 40)
        (self.root / "MusicPlayer2.zip.sha256").write_text("bad", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.verify()

    def test_private_data_and_missing_runtime(self):
        for name in ("MusicPlayer2/remote-library.json", "MusicPlayer2/../secret"):
            self.entries[name] = b"fixture-secret"
            self.write_package()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "ZIP entries"):
                self.verify()
            del self.entries[name]
        del self.entries["MusicPlayer2/bass.dll"]
        self.write_package()
        with self.assertRaisesRegex(ValueError, "ZIP entries"):
            self.verify()

    def test_wrong_architecture(self):
        content = bytearray(self.entries["MusicPlayer2/bass.dll"])
        struct.pack_into("<H", content, 68, 0x14c)
        self.entries["MusicPlayer2/bass.dll"] = content
        self.write_package()
        with self.assertRaisesRegex(ValueError, "x64 PE"):
            self.verify()

    def test_build_must_match_package(self):
        self.write_package()
        build = self.root / "build"
        build.mkdir()
        (build / "MusicPlayer2.exe").write_bytes(b"different-build")
        with self.assertRaisesRegex(ValueError, "accepted build"):
            release.verify_package(self.root, self.root, self.config, COMMIT, build)

    def test_changed_executable_and_personal_profile(self):
        self.entries["MusicPlayer2/MusicPlayer2.exe"] += b"modified"
        self.write_package()
        with self.assertRaisesRegex(ValueError, "executable checksum"):
            self.verify()
        self.entries["MusicPlayer2/MusicPlayer2.exe"] = self.entries["MusicPlayer2/bass.dll"]
        self.entries["MusicPlayer2/global_cfg.ini"] = b"personal-profile"
        self.write_package()
        with self.assertRaisesRegex(ValueError, "personal portable"):
            self.verify()

    def test_package_release_identity(self):
        self.write_package()
        self.config["version"] = "999.0.0"
        with self.assertRaisesRegex(ValueError, "release mismatch"):
            self.verify()

    def test_portable_profile_line_endings(self):
        (self.root / "third_party/runtime/defaults/global_cfg.ini").write_bytes(b"[config]\r\nportable_mode=true\r\n")
        self.entries["MusicPlayer2/global_cfg.ini"] = b"[config]\nportable_mode=true\n"
        self.write_package()
        self.verify()


if __name__ == "__main__":
    unittest.main()
