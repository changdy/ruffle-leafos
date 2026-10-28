"""Package the source-built Leaf frontend and its current documentation."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'adapter/scripts'))
from elf_info import inspect
from make_demo import make_movie
from build import zip_tree


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_ignore(directory, names):
    ignored = shutil.ignore_patterns(
        '.git', '.tools', 'build', 'target', '__pycache__', '.venv',
        'node_modules', '.idea', '.vscode', '.pytest_cache', 'workspace.local.json',
        '.env', '.env.*', '*.pyc', '*.pyo', '*.log', '*.o', '*.obj', '*.pdb',
        '*.exe', '*.part', '*.rs.bk')(directory, names)
    return ignored - {'.env.example', '.env.sample', '.env.template'}


def bundle_games(stage, games_root=ROOT / 'games'):
    manifest = json.loads((games_root / 'manifest.json').read_text(encoding='utf8'))
    source = games_root / 'flash'
    files = {p.relative_to(source).as_posix(): p for p in source.rglob('*') if p.is_file()}
    expected = manifest['files_sha256']
    if files.keys() != expected.keys():
        raise ValueError('bundled game files differ from manifest: missing='
                         + repr(sorted(expected.keys() - files.keys())) + ', extra='
                         + repr(sorted(files.keys() - expected.keys())))
    for relative, path in files.items():
        if sha(path) != expected[relative]:
            raise ValueError('bundled game checksum mismatch: ' + relative)
    for game in manifest['games']:
        if game['entrypoint'] not in expected:
            raise ValueError('bundled game entrypoint missing: ' + game['entrypoint'])
    shutil.copytree(source, stage / 'Roms/flash', dirs_exist_ok=True)
    return manifest['games']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--include-source', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / 'pak/pak.json').read_text(encoding='utf8'))
    version = manifest['pak_version']
    assert version == tomllib.loads((ROOT / 'player/sdl2test-rocknix/Cargo.toml').read_text(encoding='utf8'))['package']['version']
    binary = ROOT / 'build/ruffle-leaf'
    build_metadata = ROOT / 'build/player-build.json'
    assert build_metadata.is_file(), 'missing player build metadata; rebuild with build_player.py'
    build_info = json.loads(build_metadata.read_text(encoding='utf8'))
    assert build_info['binary_sha256'] == sha(binary), 'build metadata does not match player; rebuild with build_player.py'
    info = inspect(binary)
    assert info['machine'] == 183 and info['type'] == 3, 'need AArch64 PIE'
    assert sha(binary) != 'b3ecd3094f678a9be3e99602267f345191e02653a023ca6005fe3b0cf5aab342', 'source runtime required'
    assert max(tuple(map(int, v[6:].split('.'))) for v in info['glibc_versions']) <= (2, 38), 'target libc is 2.38'
    source_lock = json.loads((ROOT / 'leaf-source.lock.json').read_text(encoding='utf8'))
    assert sha(ROOT / 'player/Cargo.lock') == source_lock['leaf_cargo_lock_sha256']
    for relative, expected in source_lock.get('leaf_files_sha256', {}).items():
        assert sha(ROOT / 'player' / relative) == expected, 'source input changed: ' + relative
    reference_lock = json.loads((ROOT / 'vendor/jawaka.lock.json').read_text(encoding='utf8'))
    for relative, expected in reference_lock['files_sha256'].items():
        assert sha(ROOT / 'vendor/jawaka' / relative) == expected, 'reference input changed: ' + relative
    keyboard_lock = json.loads((ROOT / 'vendor/catastrophe.lock.json').read_text(encoding='utf8'))
    for relative, expected in keyboard_lock['files_sha256'].items():
        assert sha(ROOT / 'vendor/catastrophe' / relative) == expected, 'keyboard reference changed: ' + relative
    renderer_lock = json.loads((ROOT / 'vendor/ruffle-wgpu.lock.json').read_text(encoding='utf8'))
    for relative, expected in renderer_lock['files_sha256'].items():
        assert sha(ROOT / 'player' / relative) == expected, 'upstream renderer input changed: ' + relative
    stage = Path(tempfile.mkdtemp(prefix=f'preview-{version}-', dir=ROOT / 'build'))
    pak = stage / 'Apps/mlp1/RuffleProbe.pak'
    shutil.copytree(ROOT / 'pak', pak)
    frontend = pak / 'emulators/ruffle-probe'
    (frontend / 'bin').mkdir(exist_ok=True)
    (frontend / 'lib').mkdir(exist_ok=True)
    (frontend / 'res').mkdir(exist_ok=True)
    shutil.copyfile(binary, frontend / 'bin/ruffle')
    for rel in ['lib/libSDL2-2.0.so.0', 'lib/libasound.so.2', 'res/menu-font.ttf']:
        shutil.copyfile(ROOT / 'runtime/leaf-12' / rel, frontend / rel)
    shim = ROOT / 'build/libruffle_probe.so'
    shim_info = inspect(shim)
    assert shim_info['machine'] == 183 and not shim_info['needed']
    shutil.copyfile(shim, frontend / 'lib/libruffle_probe.so')
    make_movie(frontend / 'res/Leaf-GPU-Probe.swf')
    make_movie(stage / 'Roms/flash/Leaf-GPU-Probe.swf')
    bundled_games = bundle_games(stage)
    (stage / 'Images/flash').mkdir(parents=True)
    shutil.copyfile(ROOT / 'README.md', pak / 'README.md')
    (pak / 'games').mkdir()
    shutil.copyfile(ROOT / 'games/README.md', pak / 'games/README.md')
    shutil.copytree(ROOT / 'docs', pak / 'docs',
                    ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copyfile(ROOT / 'docs/THIRD_PARTY.md', pak / 'THIRD_PARTY.md')
    shutil.copyfile(ROOT / 'adapter/LICENSE', pak / 'LICENSE')
    licenses = pak / 'licenses'
    licenses.mkdir()
    shutil.copyfile(ROOT / 'player/LICENSE.md', licenses / 'RUFFLE-LICENSE.md')
    shutil.copyfile(ROOT / 'vendor/jawaka/LICENSE', licenses / 'JAWAKA-LICENSE.txt')
    shutil.copyfile(ROOT / 'vendor/catastrophe/LICENSE', licenses / 'CATASTROPHE-LICENSE.txt')
    local = ROOT / 'workspace.local.json'
    settings = json.loads(local.read_text(encoding='utf8')) if local.exists() else {}
    cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains')) / 'cargo-home'
    for crate in (cache / 'registry/src/index.crates.io-1949cf8c6b5b557f').iterdir():
        if crate.is_dir():
            for license_path in crate.iterdir():
                if license_path.is_file() and license_path.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE', 'UNLICENSE')):
                    destination = licenses / 'dependencies' / crate.name / license_path.name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(license_path, destination)
    provenance = pak / 'provenance'
    provenance.mkdir()
    shutil.copyfile(ROOT / 'leaf-source.lock.json', provenance / 'leaf-source.lock.json')
    shutil.copyfile(ROOT / 'arm-gnu.lock.json', provenance / 'arm-gnu.lock.json')
    shutil.copyfile(ROOT / 'adapter/runtime.lock.json', provenance / 'runtime.lock.json')
    shutil.copyfile(ROOT / 'vendor/jawaka.lock.json', provenance / 'jawaka.lock.json')
    shutil.copyfile(ROOT / 'vendor/catastrophe.lock.json', provenance / 'catastrophe.lock.json')
    shutil.copyfile(ROOT / 'vendor/ruffle-wgpu.lock.json', provenance / 'ruffle-wgpu.lock.json')
    shutil.copyfile(ROOT / 'games/manifest.json', provenance / 'bundled-games.json')
    # Validate against the pinned official contract, then the new behavior.
    contracts = ROOT / 'vendor/leaf-contracts/contracts'
    sys.path[:0] = [str(contracts / 'leaf-content/scripts'), str(contracts / 'leaf-services/scripts')]
    import content_model
    import minischema
    manifest = json.loads((pak / 'pak.json').read_text(encoding='utf8'))
    schema = json.loads((contracts / 'leaf-content/content-paks-v1.schema.json').read_text(encoding='utf8'))
    valid, reason = minischema.is_valid(manifest, schema)
    assert valid, reason
    assert not content_model.validate_manifest(manifest, str(pak), {'install_lane': 'platform', 'source_id': 'primary'})
    assert manifest['min_leaf_version'] == '0.13.0'
    assert manifest['provides']['cores'][0]['supports_menu'] is True
    assert manifest['provides']['cores'][0]['supports_savestate'] is False
    for path in pak.rglob('*.sh'):
        assert b'\r' not in path.read_bytes(), str(path)
    report = {
        'version': version, 'runtime_rebuilt_from_source': True,
        'runtime_sha256': sha(binary), 'source': source_lock,
        'player_build': build_info,
        'bundled_games': bundled_games,
        'target': 'Leaf 0.13.0 / AArch64 / glibc 2.38 / GLES 3 / experimental Vulkan',
        'upstream_wgpu_renderer': renderer_lock,
        'keyboard_reference': keyboard_lock,
        'elf': info, 'diagnostic_shim_changed': False,
        'diagnostic_shim_sha256': sha(shim),
        'diagnostic_shim_change': f'Unchanged in {version}; retains the earlier Menu ownership log correction',
        'scope': ['Leaf/Jawaka source-referenced in-game menu, live theme colors and pill selection',
                  'five native SharedObject progress profiles, checked atomic writes, backup recovery and timestamps',
                  'startup profile picker before VM execution; reload via process restart; confirmed profile copy',
                  'hardware-only system volume; legacy game volume/mute ignored; aspect/fit/original scaling and Flash quality',
                  'compact current-only Flash FPS and text-sized toast/drag overlays',
                  'Apps data manager with confirmed scoped clears and recoverable local archives',
                  'ten independent per-game control schemes, saved selection and toast switching',
                  'only schemes 1 and 2 have default bindings; schemes 3-10 start empty; Menu+A/B skip empty schemes',
                  'Menu+A previous / Menu+B next schemes, Menu+X direction swap, and Menu+hat down left mouse hold via read-only physical state',
                  'main-menu left/right scheme switching; mouse speed within cursor settings; separate About and contextual shortcut guide',
                  'friendly About introduction, full GitHub repository URL, community credits and bundled license notices',
                  'one global display configuration for scaling, Flash quality, FPS and GLES/Vulkan; confirmed progress flush/restart, RAV1/RAV2 preferences and GLES startup fallback',
                  'pinned official Ruffle wgpu Vulkan renderer with shared GBM DMA-BUF/EGL presentation and existing GLES menus; no per-frame CPU pixel readback for presentation',
                  'unified scheme editor with L/R switching, separate Save/Back actions and unsaved-change confirmation',
                  'Leaf pause theme shared across submenus, five-column action grid and nine visible category tabs',
                  'independent stick directions and click, mixed keyboard/pointer mappings and hysteresis',
                  'requested WASD/JKLI and arrow/1234 defaults; temporary-apply and editor reset actions removed',
                  'data-only v1-v4 mapping compatibility with v5 per-button turbo writes and atomic persistence',
                  'template workflows removed; original two default control schemes retained',
                  'movement-only cursor auto-hide, hollow ring with one-pixel center, single global appearance file and mapping speed',
                  'per-button 5/10/15 Hz hold turbo, 8ms release target with 2ms headroom below 10ms, deadline-aware loop sleep and shared-key ownership',
                  'queued physical Menu taps independent of virtual Guide release state',
                  'asynchronous real Leaf system-volume feedback without double-applying hardware keys',
                  'Simplified Chinese and English UI with persisted LeafOS first-launch detection, immediate language switching and CJK font fallback',
                  'Catastrophe-derived gamepad keyboard on editable Flash focus, transactional Unicode edits, masked passwords, native text restrictions/events and neutral input resume',
                  'pause lifecycle, input ownership, neutral gate and checked SharedObject flush'],
        'pending': ['per-profile human gameplay acceptance and temporary mouse layer',
                    'broad experimental Vulkan visual/performance/power/stability matrix and further synchronization optimization',
                    'Chinese composition/IME and broad custom game text-input compatibility', 'full VM instant savestates',
                    'legacy loose .sol migration', 'power-loss/reboot storage matrix', 'suspend recovery'],
        'device_validation': 'See docs/validation/soft-keyboard-20261028.md for keyboard/runtime checks and installation scope; automated packaging alone does not verify hardware',
    }
    (pak / 'build-manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    archive = output / f'ruffle-leafos-{version}.zip'
    zip_tree(stage, archive)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert all((item.external_attr >> 16) & 0o111 for item in z.infolist() if item.filename.endswith('.sh'))
    if args.include_source:
        source_stage = Path(tempfile.mkdtemp(prefix='preview-source-', dir=ROOT / 'build'))
        for folder in ['player', 'adapter', 'pak', 'scripts', 'docs', 'games', 'target-libs', 'runtime/leaf-12', 'vendor/leaf-contracts', 'vendor/catastrophe']:
            shutil.copytree(ROOT / folder, source_stage / folder,
                ignore=source_ignore)
        for filename in ['README.md', '.gitignore', 'leaf-source.lock.json', 'arm-gnu.lock.json']:
            shutil.copyfile(ROOT / filename, source_stage / filename)
        # Keep the exact menu reference and license available in the source
        # archive; packaging must not depend on an omitted vendor checkout.
        for filename in ['README.md', 'jawaka.lock.json', 'ruffle-wgpu.lock.json', 'catastrophe.lock.json']:
            shutil.copyfile(ROOT / 'vendor' / filename, source_stage / 'vendor' / filename)
        for relative in reference_lock['files_sha256']:
            destination = source_stage / 'vendor/jawaka' / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'vendor/jawaka' / relative, destination)
        zip_tree(source_stage, output / f'ruffle-leafos-{version}-source.zip')
    (output / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    (ROOT / 'build/package-stage-path.txt').write_text(str(stage), encoding='utf8')
    sums = ''.join(f'{sha(p)}  {p.name}\n' for p in sorted(output.glob('*.zip')))
    (output / 'SHA256SUMS.txt').write_text(sums, encoding='ascii')
    print(json.dumps({'stage': str(stage), 'archive': str(archive), 'runtime_sha256': sha(binary)}, indent=2))


if __name__ == '__main__':
    main()
