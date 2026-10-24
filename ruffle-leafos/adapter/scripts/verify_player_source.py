"""Audit the pinned source candidate without executing its build scripts."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def verify(source):
    lock = json.loads((ROOT/'player-source.lock.json').read_text(encoding='utf8'))
    source = Path(source).resolve()
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != lock['commit']:
        raise ValueError(f"source commit mismatch: {head}")
    subprocess.run(['git', '-C', str(source), 'diff', '--exit-code', 'HEAD', '--'], check=True)
    cargo_hash = hashlib.sha256((source/'Cargo.lock').read_bytes()).hexdigest()
    if cargo_hash != lock['cargo_lock_sha256']:
        raise ValueError('Cargo.lock mismatch')
    for name in ['main.rs', 'gl_backend.rs', 'keycodes.rs', 'navigator.rs', 'ui_backend.rs']:
        if not (source/'sdl2test-rocknix'/'src'/name).is_file():
            raise ValueError(f'missing source file: {name}')
    return {'repository': lock['repository'], 'commit': head, 'cargo_lock_sha256': cargo_hash,
            'candidate_source_verified': True, 'leaf12_binary_reproduced': False,
            'leaf_specific_patches': 'not available; must be reconstructed and device-tested'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = verify(args.source)
    text = json.dumps(result, ensure_ascii=False, indent=2)+'\n'
    if args.output:
        args.output.write_text(text, encoding='utf8')
    print(text)
