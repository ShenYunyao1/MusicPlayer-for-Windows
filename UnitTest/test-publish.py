"""Exercise publishing with isolated Git repositories and fake network commands."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REAL_GIT = shutil.which("git")
POWERSHELL = shutil.which("powershell.exe")
OPENSSL = shutil.which('openssl') or next(str(p / 'usr/bin/openssl.exe') for p in Path(REAL_GIT).resolve().parents if (p / 'usr/bin/openssl.exe').is_file())

STUB = r'''
import hashlib, json, os, pathlib, subprocess, sys, zipfile
kind, args = sys.argv[1], sys.argv[2:]
root = pathlib.Path(os.environ["PUBLISH_FIXTURE"])
scenario = os.environ.get("PUBLISH_SCENARIO", "")
with (root / "calls.jsonl").open("a", encoding="utf-8") as log:
    log.write(json.dumps([kind, *args]) + "\n")
def option(name): return args[args.index(name) + 1]
def head(): return subprocess.check_output([os.environ["PUBLISH_REAL_GIT"], "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
def trigger(event):
    counter = root / "test-results/counter"
    counter.write_text(str(int(counter.read_text()) + 1 if counter.exists() else 1))
    (root / "triggered").write_text(event)
if kind == "git":
    clean = list(args)
    while clean and clean[0] in ("-C", "-c"):
        clean = clean[2:]
    if clean[:2] == ["credential", "fill"]:
        print("username=fixture\npassword=fixture-secret")
    elif clean and clean[0] == "ls-remote":
        if "--tags" in clean and scenario == "remote-tag": print("a" * 40 + "\trefs/tags/v0.1.0")
        elif "--heads" in clean and scenario in ("behind", "up-to-date"):
            commit = "a" * 40 if scenario == "behind" else head()
            print(commit + "\trefs/heads/master")
    elif clean and clean[0] == "fetch": pass
    elif clean[:2] == ["merge-base", "--is-ancestor"] and scenario == "behind": sys.exit(1)
    elif clean and clean[0] == "push":
        if scenario == "push-failure": sys.exit(1)
        trigger("push")
    else:
        sys.exit(subprocess.run([os.environ["PUBLISH_REAL_GIT"], *args]).returncode)
else:
    if os.environ.get("GH_HOST") != "github.com": raise SystemExit("wrong GitHub host")
    if args[:2] == ["repo", "view"]:
        print(json.dumps({"viewerPermission": "READ" if scenario == "readonly" else "ADMIN"}))
    elif args[:2] == ["release", "list"]:
        print(json.dumps([{"tagName": "v9.0.0", "isDraft": False, "isPrerelease": False}] if scenario == "old-version" else []))
    elif args[:2] == ["workflow", "run"]:
        trigger("workflow_dispatch")
    elif args[:2] == ["run", "view"]:
        names = ["test", "build-x86", "verify-artifact"]
        jobs = [{"name": name, "conclusion": "success"} for name in names]
        if scenario == "skipped-test": jobs[0]["conclusion"] = "skipped"
        jobs.append({"name": "release", "conclusion": "success" if (root / "release-mode").exists() else "skipped"})
        print(json.dumps({"headSha": head(), "jobs": jobs}))
    elif args[:2] == ["release", "download"]:
        directory = pathlib.Path(option("--dir")); directory.mkdir(parents=True, exist_ok=True)
        exe = b"fixture executable"
        digest = hashlib.sha256(exe).hexdigest()
        info = f"Release: 0.1.0\nPlatform: windows-x64\nGit: {head()}\nMusicPlayer2.exe SHA-256: {digest}\n"
        with zipfile.ZipFile(directory / "MusicPlayer2.zip", "w") as archive:
            archive.writestr("MusicPlayer2/BUILD-INFO.txt", info)
            archive.writestr("MusicPlayer2/MusicPlayer2.exe", exe)
        archive = (directory / "MusicPlayer2.zip").read_bytes()
        config = json.loads((root / "client-release.json").read_text())
        manifest = {"version": "0.1.0", "commit": "b" * 40 if scenario == "wrong-manifest" else head(),
                    "platform": "windows-x64", "executable_sha256": digest,
                    "server_baseline": {k[7:]: v for k, v in config.items() if k.startswith("server_")},
                    "assets": [{"name": "MusicPlayer2.zip", "size": len(archive), "sha256": hashlib.sha256(archive).hexdigest()}]}
        (directory / "client-release-manifest.json").write_text(json.dumps(manifest))
        subprocess.run([os.environ['PUBLISH_OPENSSL'], 'pkeyutl', '-sign', '-rawin', '-inkey',
                        str(root / 'test-results/sign-private.pem'), '-in', str(directory / 'client-release-manifest.json'),
                        '-out', str(directory / 'client-release-manifest.json.sig')], check=True, capture_output=True)
        if scenario == 'wrong-signature': (directory / 'client-release-manifest.json.sig').write_bytes(b'X' * 64)
        (directory / "MusicPlayer2.zip.sha256").write_text(hashlib.sha256(archive).hexdigest() + "  MusicPlayer2.zip\n")
        if scenario == "corrupt-download": (directory / "MusicPlayer2.zip").write_bytes(archive + b"corrupt")
    elif args[0] == "api" and "/actions/runs?" in args[1]:
        runs = []
        if (root / "triggered").exists():
            ref = "v0.1.0" if (root / "release-mode").exists() else "master"
            run = {"id": 123 + int((root / "test-results/counter").read_text()), "head_sha": head(), "head_branch": ref,
                   "event": (root / "triggered").read_text(), "path": ".github/workflows/windows-release.yml",
                   "status": "completed", "conclusion": "failure" if scenario == "ci-failure" else "success",
                   "html_url": "https://github.com/fixture/player/actions/runs/123"}
            stale = dict(run, id=122, head_sha="c" * 40, conclusion="success")
            runs = [stale, run]
        print(json.dumps({"workflow_runs": runs}))
    elif args[0] == "api" and "/releases/tags/" in args[1]:
        names = ["MusicPlayer2.zip", "MusicPlayer2.zip.sha256", "client-release-manifest.json", "client-release-manifest.json.sig"]
        if scenario == "missing-asset": names.pop()
        print(json.dumps({"tag_name": "v0.1.0", "draft": False, "prerelease": False,
                          "html_url": "https://github.com/fixture/player/releases/tag/v0.1.0",
                          "assets": [{"name": name, "size": 1} for name in names]}))
    else: raise SystemExit("unexpected fake CLI command: " + repr(args))
'''


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="publish fixture ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.git("init", "-b", "master")
        self.git("config", "user.name", "Publish fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("remote", "add", "origin", "https://github.com/fixture/player.git")
        for name in ("publish.ps1", "publish.bat", "client-release.json", "Documents/client-release-notes.md", ".github/workflows/windows-release.yml", "scripts/client-release.py"):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, destination)
        config = json.loads((self.root / 'client-release.json').read_text())
        config['version'] = '0.1.0'
        (self.root / 'client-release.json').write_text(json.dumps(config))
        (self.root / 'Documents/client-release-notes.md').write_text('# fixture release 0.1.0')
        (self.root / 'test-results').mkdir()
        public = self.root / 'updater/internal/trust/public.pem'
        public.parent.mkdir(parents=True)
        subprocess.run([OPENSSL, 'genpkey', '-algorithm', 'ED25519', '-out', str(self.root / 'test-results/sign-private.pem')], check=True, capture_output=True)
        subprocess.run([OPENSSL, 'pkey', '-in', str(self.root / 'test-results/sign-private.pem'), '-pubout', '-out', str(public)], check=True, capture_output=True)
        (self.root / ".gitignore").write_text("/test-results/\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-m", "Fixture")
        tools = self.root / "test-results/tools"
        tools.mkdir(parents=True)
        (tools / "fake-cli.py").write_text(STUB, encoding="utf-8")
        for kind in ("git", "gh"):
            (tools / (kind + ".bat")).write_text(
                f'@echo off\r\n"{sys.executable}" "%~dp0fake-cli.py" {kind} %*\r\nexit /b %errorlevel%\r\n')
        self.environment = dict(os.environ, PUBLISH_FIXTURE=str(self.root), PUBLISH_REAL_GIT=REAL_GIT,
                                PUBLISH_OPENSSL=OPENSSL,
                                GH_TOKEN="fixture-secret", MUSIC_PLAYER_NO_PAUSE="1")

    def git(self, *arguments):
        return subprocess.check_output([REAL_GIT, "-C", str(self.root), *arguments], stderr=subprocess.STDOUT, text=True).strip()

    def calls(self):
        path = self.root / "calls.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def invoke(self, mode="Build", scenario="", *extra):
        environment = dict(self.environment, PUBLISH_SCENARIO=scenario)
        if mode == "Release": (self.root / "release-mode").touch()
        # Fixture bookkeeping must never make the source workspace dirty.
        self.git("config", "core.excludesFile", str(self.root / "test-results/excludes"))
        (self.root / "test-results/excludes").write_text("calls.jsonl\ntriggered\nrelease-mode\n")
        command = [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(self.root / "publish.ps1"),
                   "-GitPath", str(self.root / "test-results/tools/git.bat"),
                   "-GhPath", str(self.root / "test-results/tools/gh.bat"), "-PollSeconds", "1", *extra]
        if mode: command += ["-Mode", mode]
        return subprocess.run(command, env=environment, cwd=self.root.parent, capture_output=True, text=True, timeout=35)

    def assert_no_push(self):
        self.assertFalse(any("push" in call for call in self.calls()))

    def test_build_and_exact_commit_run(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        pushes = [call for call in self.calls() if "push" in call]
        self.assertEqual(len(pushes), 1)
        self.assertNotIn("refs/tags/v0.1.0:refs/tags/v0.1.0", pushes[0])
        self.assertNotIn("fixture-secret", result.stdout + result.stderr)

    def test_release_and_atomic_refs(self):
        result = self.invoke("Release", "", "-Version", "0.1.0")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        push = next(call for call in self.calls() if "push" in call)
        self.assertIn("--atomic", push)
        self.assertIn("refs/heads/master:refs/heads/master", push)
        self.assertIn("refs/tags/v0.1.0:refs/tags/v0.1.0", push)
        self.assertEqual(self.git("rev-parse", "v0.1.0^{}"), self.git("rev-parse", "HEAD"))

    def test_dry_run_has_no_network_or_tag(self):
        result = self.invoke("Release", "", "-Version", "0.1.0", "-DryRun")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_no_push()
        self.assertFalse(any(call[0] == "gh" or "ls-remote" in call or "fetch" in call for call in self.calls()))
        self.assertEqual(self.git("tag", "--list"), "")

    def test_dirty_workspace(self):
        (self.root / "uncommitted.txt").write_text("fixture")
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assert_no_push()

    def test_version_mismatch(self):
        self.assertNotEqual(self.invoke("Release", "", "-Version", "9.9.9").returncode, 0)
        self.assert_no_push()

    def test_local_tag_exists(self):
        self.git("tag", "v0.1.0")
        self.assertNotEqual(self.invoke("Release").returncode, 0)
        self.assert_no_push()

    def test_preflight_denials(self):
        for scenario, message in (("remote-tag", "Remote release tag already exists"), ("readonly", "No write permission"),
                                  ("old-version", "must exceed published"), ("behind", "ahead or diverged")):
            with self.subTest(scenario=scenario):
                result = self.invoke("Release", scenario)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stdout + result.stderr)
                self.assert_no_push()

    def test_ci_failures_and_skipped_tests(self):
        for scenario in ("ci-failure", "skipped-test"):
            with self.subTest(scenario=scenario):
                result = self.invoke("Build", scenario)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Actions failed" if scenario == "ci-failure" else "Required Actions job did not pass", result.stdout)

    def test_release_integrity_failures(self):
        for scenario, message in (("wrong-manifest", "manifest identity mismatch"), ("missing-asset", "assets are incomplete"),
                                  ("corrupt-download", "ZIP hash or size mismatch")):
            with self.subTest(scenario=scenario):
                # Each release needs a fresh local tag namespace.
                self.git("tag", "-d", "v0.1.0") if self.git("tag", "--list", "v0.1.0") else None
                result = self.invoke("Release", scenario)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(message, result.stdout + result.stderr)

    def test_bad_release_signature(self):
        result = self.invoke('Release', 'wrong-signature')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('RELEASE VERIFIED', result.stdout)

    def test_no_wait_and_up_to_date_build(self):
        result = self.invoke("Build", "up-to-date", "-NoWait")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TRIGGERED ONLY", result.stdout)
        self.assertTrue(any(call[0] == "gh" and call[1:3] == ["workflow", "run"] for call in self.calls()))

    def test_no_arguments_and_bat_help(self):
        result = self.invoke(None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls(), [])
        result = subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "call", str(self.root / "publish.bat"), "-Help"],
                                env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Usage:", result.stdout)

    def test_push_failure_retains_local_tag(self):
        result = self.invoke("Release", "push-failure")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Local tag retained", result.stdout)
        self.assertEqual(self.git("rev-parse", "v0.1.0^{}"), self.git("rev-parse", "HEAD"))
        self.assertFalse(any(call[0] == "gh" and "/releases/tags/" in " ".join(call) for call in self.calls()))

    def test_multiple_push_urls_are_rejected(self):
        self.git("config", "--add", "remote.origin.pushurl", "https://github.com/fixture/player.git")
        self.git("config", "--add", "remote.origin.pushurl", "https://github.com/unrelated/project.git")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one fetch URL and one push URL", result.stdout)
        self.assert_no_push()

    def test_shallow_and_unsupported_branch(self):
        self.git("switch", "-c", "development")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Current branch must be master or main", result.stdout)
        self.git("switch", "master")
        (self.root / ".git/shallow").write_text(self.git("rev-parse", "HEAD") + "\n")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Shallow clone", result.stdout)
        self.assert_no_push()

    def test_credential_manager_fallback(self):
        self.environment.pop("GH_TOKEN", None)
        self.environment.pop("GITHUB_TOKEN", None)
        result = self.invoke("Build", "", "-NoWait")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(any("credential" in call for call in self.calls()))
        self.assertNotIn("fixture-secret", result.stdout + result.stderr)

    def test_external_host_is_not_given_github_credentials(self):
        self.environment["GH_HOST"] = "example.invalid"
        result = self.invoke("Build", "", "-NoWait")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
