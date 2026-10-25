"""Prepare the existing Windows build on a clean GitHub-hosted runner.

Compiler archives and SHA-256 values come exclusively from the repository locks.
Node.js, Python, JDK 17 and 7-Zip are supplied by the workflow/runner.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools'
DOWNLOADS = TOOLS / 'downloads'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def download(url, expected):
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOADS / url.rsplit('/', 1)[-1]
    if archive.is_file() and digest(archive) == expected:
        return archive
    for attempt in range(3):
        try:
            print('Downloading ' + url, flush=True)
            request = urllib.request.Request(url, headers={'User-Agent': 'Ruffle-LeafOS-build/1'})
            temporary = archive.with_suffix(archive.suffix + '.part')
            with urllib.request.urlopen(request, timeout=120) as source, temporary.open('wb') as target:
                shutil.copyfileobj(source, target)
            if digest(temporary) != expected:
                raise ValueError('SHA-256 mismatch: ' + url)
            temporary.replace(archive)
            return archive
        except OSError:
            if attempt == 2:
                raise
            time.sleep(3)


def unpack(archive, destination):
    if archive.suffix == '.exe':
        seven_zip = shutil.which('7z') or 'C:/Program Files/7-Zip/7z.exe'
        subprocess.run([seven_zip, 'x', '-y', '-o' + str(destination), str(archive)], check=True,
                       stdout=subprocess.DEVNULL)
    elif archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                if not (destination / item.filename).resolve().is_relative_to(destination.resolve()):
                    raise ValueError('Unsafe archive path: ' + item.filename)
            source.extractall(destination)
    else:
        with tarfile.open(archive) as source:
            source.extractall(destination, filter='data')


def install_tree(url, expected, destination):
    marker = destination / '.installed-sha256'
    if marker.is_file() and marker.read_text() == expected:
        return
    archive = download(url, expected)
    with tempfile.TemporaryDirectory(dir=TOOLS, prefix='install-') as directory:
        stage = Path(directory)
        unpack(archive, stage)
        entries = list(stage.iterdir())
        payload = entries[0] if len(entries) == 1 and entries[0].is_dir() else stage
        shutil.copytree(payload, destination, dirs_exist_ok=True)
    marker.write_text(expected, encoding='ascii')


def main():
    if os.name != 'nt':
        raise SystemExit('This setup uses the pinned Windows GNU build; run on Windows.')
    rust_lock = json.loads((ROOT / 'adapter/rust-toolchain.lock.json').read_text(encoding='utf8'))
    references = json.loads((ROOT / 'adapter/references.lock.json').read_text(encoding='utf8'))
    arm_lock = json.loads((ROOT / 'arm-gnu.lock.json').read_text(encoding='utf8'))
    TOOLS.mkdir(exist_ok=True)
    rust = TOOLS / ('rust-' + rust_lock['rust_version'])
    for component in rust_lock['components']:
        marker = rust / ('.installed-' + component['component'] + '-' + component['target'])
        if marker.is_file() and marker.read_text() == component['sha256']:
            continue
        archive = download(component['url'], component['sha256'])
        with tempfile.TemporaryDirectory(dir=TOOLS, prefix='rust-install-') as directory:
            stage = Path(directory)
            unpack(archive, stage)
            [payload] = stage.iterdir()
            for name in (payload / 'components').read_text().splitlines():
                shutil.copytree(payload / name, rust, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns('manifest.in'))
        marker.write_text(component['sha256'], encoding='ascii')
    mingw = TOOLS / 'w64devkit'
    gcc = rust_lock['host_c_linker']
    install_tree(gcc['url'], gcc['sha256'], mingw)
    zig = references['zig']
    zig_root = TOOLS / ('zig-' + zig['version'])
    install_tree(zig['windows_url'], zig['windows_sha256'], zig_root)
    install_tree(arm_lock['url'], arm_lock['sha256'], ROOT / arm_lock['root'])
    settings = {
        'host_tool_cache': str(ROOT.parent / 'work/toolchains'),
        'rust': str(rust), 'host_gcc': str(mingw), 'zig': str(zig_root / 'zig.exe'),
        'jdk': os.environ['JAVA_HOME'],
    }
    (ROOT / 'workspace.local.json').write_text(json.dumps(settings, indent=2) + '\n', encoding='utf8')
    for executable, args in [(rust / 'bin/rustc.exe', ['--version']),
                             (rust / 'bin/cargo.exe', ['--version']),
                             (mingw / 'bin/gcc.exe', ['--version']),
                             (zig_root / 'zig.exe', ['version'])]:
        subprocess.run([str(executable), *args], check=True)


if __name__ == '__main__':
    main()
