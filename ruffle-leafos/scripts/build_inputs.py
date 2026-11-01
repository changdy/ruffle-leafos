"""Bind a local player ELF to a snapshot of its project build inputs.

This is a stale-artifact guard, not a signed or reproducible-build claim. Cargo
dependencies and external compilers remain governed by the existing locks and
tool version records. No Git checkout is required; uncommitted files are hashed.
"""
from pathlib import Path
import hashlib
import json
import os


SCHEMA_VERSION = 1
INPUT_TREES = ('player', 'target-libs')
FIXED_INPUTS = (
    'arm-gnu.lock.json',
    'adapter/rust-toolchain.lock.json',
    'adapter/references.lock.json',
    'adapter/scripts/elf_info.py',
    'scripts/build_player.py',
    'scripts/clang_target.py',
    'scripts/build_inputs.py',
)
EXCLUDED_DIRS = frozenset({
    '.git', '.tools', 'build', 'target', '__pycache__', '.venv',
    'node_modules', '.idea', '.vscode', '.pytest_cache',
})
EXCLUDED_NAMES = frozenset({'workspace.local.json', '.DS_Store', 'Thumbs.db', 'Desktop.ini'})
EXCLUDED_SUFFIXES = ('.md', '.pyc', '.pyo', '.log', '.part', '.rs.bk', '.o', '.obj', '.pdb', '.exe')


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def invalidate_build_metadata(root):
    """A failed new build must not leave an older success record available."""
    (Path(root) / 'build/player-build.json').unlink(missing_ok=True)


def _excluded_file(name):
    return (name in EXCLUDED_NAMES or name.startswith('.env')
            or name.lower().endswith(EXCLUDED_SUFFIXES) or name.endswith('~'))


def _walk_error(error):
    raise error


def _input_files(root):
    for relative in FIXED_INPUTS:
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or unsupported build input: ' + relative)
        yield path
    for relative in INPUT_TREES:
        tree = root / relative
        if tree.is_symlink() or not tree.is_dir():
            raise ValueError('missing or unsupported build input directory: ' + relative)
        for directory, names, files in os.walk(tree, onerror=_walk_error, followlinks=False):
            names[:] = sorted(name for name in names if name not in EXCLUDED_DIRS)
            for name in [*names, *sorted(files)]:
                if _excluded_file(name):
                    continue
                path = Path(directory) / name
                if path.is_symlink():
                    raise ValueError('unsupported symbolic build input: ' + path.relative_to(root).as_posix())
                if path.is_file():
                    yield path


def _digest(files):
    payload = json.dumps(files, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf8')
    return hashlib.sha256(payload).hexdigest()


def snapshot_inputs(root):
    """Hash workspace source/assets/manifests and fixed link/tool inputs.

    The whole player workspace is included conservatively, including ASC,
    shaders, fonts and embedded test resources. Only known generated/local
    files and Markdown documentation are excluded. Root docs, games, pak assets
    and leaf-source.lock.json are packaging/provenance inputs, not ELF inputs.
    pkg-config files retain their actual relocated bytes: this binds one local
    workspace and is intentionally not stable across absolute-path relocation.
    """
    root = Path(root).resolve()
    files = {path.relative_to(root).as_posix(): sha256_file(path) for path in _input_files(root)}
    files = dict(sorted(files.items()))
    return {'schema_version': SCHEMA_VERSION, 'files_sha256': files, 'sha256': _digest(files)}


def _validate_snapshot(snapshot):
    if (not isinstance(snapshot, dict) or type(snapshot.get('schema_version')) is not int
            or snapshot['schema_version'] != SCHEMA_VERSION):
        raise ValueError('missing or unsupported source input snapshot; rebuild with build_player.py')
    files = snapshot.get('files_sha256')
    if not isinstance(files, dict) or not files or not all(
        isinstance(path, str) and isinstance(digest, str)
        and len(digest) == 64 and all(c in '0123456789abcdef' for c in digest)
        for path, digest in files.items()
    ) or snapshot.get('sha256') != _digest(files):
        raise ValueError('invalid source input snapshot; rebuild with build_player.py')


def require_unchanged_inputs(before, after):
    _validate_snapshot(before)
    _validate_snapshot(after)
    if before != after:
        left, right = before['files_sha256'], after['files_sha256']
        changed = sorted(path for path in left.keys() | right.keys() if left.get(path) != right.get(path))
        raise ValueError('player build inputs changed: ' + ', '.join(changed[:8])
                         + (' ...' if len(changed) > 8 else '')
                         + '; rebuild with build_player.py')


def verify_player_build(root, binary, build_info):
    """Reject missing/old records, another ELF, or inputs changed after build."""
    if not isinstance(build_info, dict):
        raise ValueError('invalid player build metadata; rebuild with build_player.py')
    recorded = build_info.get('source_inputs')
    _validate_snapshot(recorded)
    if build_info.get('binary_sha256') != sha256_file(binary):
        raise ValueError('build metadata does not match player ELF; rebuild with build_player.py')
    require_unchanged_inputs(recorded, snapshot_inputs(root))
