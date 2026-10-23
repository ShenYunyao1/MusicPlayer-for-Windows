"""Validate release identity and the exact allowlisted Windows x64 distribution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import shutil
import tempfile
import zipfile


CORE = ("MusicPlayer2.exe", "MusicPlayerUpdater.exe", "bass.dll", "bass_fx.dll", "tag.dll", "SciLexer.dll")
SEMVER = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_config(root, ref=""):
    config = json.loads((root / "client-release.json").read_text(encoding="utf-8"))
    require(set(config) == {"version", "server_repository", "server_commit", "server_version", "server_schema"},
            "unexpected release configuration fields")
    for field in ("version", "server_version"):
        require(isinstance(config.get(field), str) and re.fullmatch(SEMVER, config[field]), "invalid " + field)
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", config.get("server_repository", "")), "invalid server repository")
    require(re.fullmatch(r"[a-f0-9]{40}", config.get("server_commit", "")), "server baseline must be a full commit")
    require(type(config.get("server_schema")) is int and config["server_schema"] > 0, "invalid server schema")
    if ref.startswith("refs/tags/"):
        require(ref == "refs/tags/v" + config["version"], "tag differs from client release version")
    notes = (root / "Documents/client-release-notes.md").read_text(encoding="utf-8")
    require(config["version"] in notes.splitlines()[0], "release notes version mismatch")
    return config


def expected_entries(root):
    names = {"MusicPlayer2/" + name for name in (*CORE, "global_cfg.ini", "LICENSE", "README.txt", "BUILD-INFO.txt", "PROGRAM-FILES.json")}
    for source, target in (("MusicPlayer2/language", "language"), ("MusicPlayer2/skins", "skins"),
                           ("third_party/runtime/common/Encoder", "Encoder")):
        folder = root / source
        require(folder.is_dir(), "missing authoritative resources: " + source)
        names.update("MusicPlayer2/" + target + "/" + path.relative_to(folder).as_posix()
                     for path in folder.rglob("*") if path.is_file())
    return names


def verify_package(root, directory, config, commit, build=None):
    require(re.fullmatch(r"[a-f0-9]{40}", commit), "invalid accepted commit")
    archive_path = directory / "MusicPlayer2.zip"
    archive_bytes = archive_path.read_bytes()
    digest = hashlib.sha256(archive_bytes).hexdigest()
    require((directory / "MusicPlayer2.zip.sha256").read_text(encoding="utf-8").strip()
            == digest + "  MusicPlayer2.zip", "ZIP checksum mismatch")
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)) and set(names) == expected_entries(root), "unexpected, duplicate or missing ZIP entries")
        fields = dict(line.split(": ", 1) for line in archive.read("MusicPlayer2/BUILD-INFO.txt").decode("utf-8-sig").splitlines())
        require(fields.get("Git") == commit, "package commit mismatch")
        require(fields.get("Release") == config["version"], "package release mismatch")
        require(fields.get("Platform") == "windows-x64", "package platform mismatch")
        for name in CORE:
            content = archive.read("MusicPlayer2/" + name)
            require(content[:2] == b"MZ" and len(content) >= 64, "invalid PE: " + name)
            offset = struct.unpack_from("<I", content, 0x3c)[0]
            require(content[offset:offset+4] == b"PE\0\0" and struct.unpack_from("<H", content, offset+4)[0] == 0x8664,
                    "expected x64 PE: " + name)
            if build:
                require(content == (build / name).read_bytes(), "package differs from accepted build: " + name)
        executable_hash = hashlib.sha256(archive.read("MusicPlayer2/MusicPlayer2.exe")).hexdigest()
        require(fields.get("MusicPlayer2.exe SHA-256") == executable_hash, "executable checksum mismatch")
        # Git checkouts on Windows and Linux may use CRLF and LF respectively.
        # Compare the exact default text while preserving all other content.
        require(archive.read("MusicPlayer2/global_cfg.ini").replace(b"\r\n", b"\n")
                == (root / "third_party/runtime/defaults/global_cfg.ini").read_bytes().replace(b"\r\n", b"\n"),
                "package contains a personal portable profile")
        inventory = json.loads(archive.read("MusicPlayer2/PROGRAM-FILES.json"))
        expected_inventory = {name.removeprefix("MusicPlayer2/"): hashlib.sha256(archive.read(name)).hexdigest()
                              for name in sorted(names) if name.startswith(("MusicPlayer2/skins/", "MusicPlayer2/language/", "MusicPlayer2/Encoder/"))}
        require(inventory == expected_inventory, "resource baseline mismatch")
        files = [{"path": name.removeprefix("MusicPlayer2/"), "size": len(archive.read(name)),
                  "sha256": hashlib.sha256(archive.read(name)).hexdigest()} for name in sorted(names)]
    return {"protocol": 1, "product": "musicplayer-windows", "minimum_updater": 1, "key_id": "client-v1",
            "notes": (root / "Documents/client-release-notes.md").read_text(encoding="utf-8").replace("\r\n", "\n"),
            "files": files, "version": config["version"], "commit": commit, "platform": "windows-x64",
            "server_baseline": {key.removeprefix("server_"): value for key, value in config.items() if key.startswith("server_")},
            "assets": [{"name": archive_path.name, "size": len(archive_bytes), "sha256": digest}],
            "executable_sha256": executable_hash}


def verify_signature(root, directory, public_key=None):
    executable = shutil.which('openssl')
    if not executable:
        git = shutil.which('git')
        if git:
            for parent in Path(git).resolve().parents:
                candidate = parent / 'usr/bin/openssl.exe'
                if candidate.is_file():
                    executable = str(candidate)
                    break
    require(executable, 'OpenSSL is required to verify Ed25519 release signatures')
    subprocess.run([executable, 'pkeyutl', '-verify', '-pubin', '-rawin', '-inkey',
                    str(public_key or root / 'updater/internal/trust/public.pem'), '-in',
                    str(directory / 'client-release-manifest.json'), '-sigfile',
                    str(directory / 'client-release-manifest.json.sig')], check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ref", default="")
    parser.add_argument("--commit")
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--verify-manifest", action="store_true")
    parser.add_argument("--verify-signature", action="store_true")
    parser.add_argument("--signature-only", action="store_true")
    args = parser.parse_args()
    if args.signature_only:
        require(args.package, '--package required')
        verify_signature(args.root, args.package)
        print('PASS signed manifest')
        return
    config = load_config(args.root, args.ref)
    head = subprocess.check_output(["git", "-C", str(args.root), "rev-parse", "HEAD"], text=True).strip()
    require(not args.commit or args.commit == head, "accepted commit differs from checkout")
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            for key, value in dict(config, commit=head).items():
                output.write(f"{key}={value}\n")
    if args.package:
        manifest = verify_package(args.root, args.package, config, head, args.build)
        manifest_path = args.package / "client-release-manifest.json"
        if args.verify_manifest:
            require(json.loads(manifest_path.read_text(encoding="utf-8")) == manifest, "published manifest mismatch")
        else:
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if args.verify_signature:
            verify_signature(args.root, args.package)
    print(f"PASS client release {config['version']} at {head}")


if __name__ == "__main__":
    main()
