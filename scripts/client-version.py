"""Generate C++ and RC release identity before compilation (no runtime data)."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
version = json.loads((root / 'client-release.json').read_text(encoding='utf-8'))['version']
if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
    raise SystemExit('Invalid client release version')
if any(int(n) > 65535 for n in version.split('.')):
    raise SystemExit('Version exceeds Windows resource range')
commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if not re.fullmatch('[a-f0-9]{40}', commit):
    raise SystemExit('Invalid commit')
content = (f'#pragma once\n#define CLIENT_VERSION L"{version}"\n'
           f'#define CLIENT_VERSION_A "{version}"\n#define CLIENT_COMMIT "{commit}"\n'
           f'#define CLIENT_FILE_VERSION {version.replace(".", ",")},0\n'
           f'#define CLIENT_FILE_VERSION_TEXT "{version}.0"\n')
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text() != content:
    args.output.write_text(content, encoding='utf-8')
