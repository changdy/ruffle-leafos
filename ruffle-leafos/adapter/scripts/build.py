"""Assemble a private Pocket 1 display probe using the user-supplied runtime."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
import tempfile

from elf_info import inspect
from make_demo import make_icon, make_movie

ROOT = Path(__file__).resolve().parents[1]
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(args, env):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT, env=env)
def zip_tree(source, target, excluded=()):
    with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(source.rglob('*')):
            rel = path.relative_to(source)
            if any(part in excluded for part in rel.parts): continue
            if not path.is_dir() and not path.is_file(): continue
            item = zipfile.ZipInfo(rel.as_posix()+('/' if path.is_dir() else ''), date_time=(2026, 10, 23, 0, 0, 0))
            item.create_system = 3
            executable = path.suffix == '.sh' or path.name == 'ruffle'
            item.external_attr = (0o40755 if path.is_dir() else (0o100755 if executable else 0o100644)) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, b'' if path.is_dir() else path.read_bytes())

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime-pak', required=True, type=Path)
    parser.add_argument('--zig', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--shell', type=Path, required=True, help='BusyBox executable or /bin/sh, for wrapper tests')
    parser.add_argument('--node', default=shutil.which('node'), help='Node executable to run compiled C tests')
    parser.add_argument('--player-source', type=Path, help='Audit the pinned source candidate; does not replace the supplied binary')
    args = parser.parse_args()
    runtime = args.runtime_pak.resolve(); zig = args.zig.resolve(); output = args.output.resolve()
    lock = json.loads((ROOT/'runtime.lock.json').read_text(encoding='utf8'))
    manifest = json.loads((ROOT/'pak'/'pak.json').read_text(encoding='utf8'))
    version = manifest['pak_version']
    for rel, expected in lock['files'].items():
        if sha(runtime/rel) != expected: raise SystemExit(f'runtime hash mismatch: {rel}')
    build = ROOT/'build'; build.mkdir(exist_ok=True)
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(build/'zig-global-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = str(build/'zig-local-cache')
    temporary = build/'tmp'; temporary.mkdir(exist_ok=True)
    for name in ('TMP', 'TEMP', 'TMPDIR'): env[name] = str(temporary)
    assert subprocess.check_output([str(zig), 'version'], text=True).strip() == '0.14.1', 'wrong Zig version'
    refs = json.loads((ROOT/'references.lock.json').read_text(encoding='utf8'))
    contract_commit = subprocess.check_output(['git', '-C', str(args.contract.resolve()), 'rev-parse', 'HEAD'], text=True).strip()
    assert contract_commit == refs['leaf-contracts']['commit'], 'contract checkout differs from lock'
    subprocess.run(['git', '-C', str(args.contract.resolve()), 'diff', '--exit-code', 'HEAD', '--'], check=True)
    stage = Path(tempfile.mkdtemp(prefix=f'card-{version}-', dir=build)); pak = stage/'Apps'/'mlp1'/'RuffleProbe.pak'
    # Fresh staging prevents removed or untracked payloads leaking into a rebuild.
    shutil.copytree(ROOT/'pak', pak, dirs_exist_ok=True)
    frontend = pak/'emulators'/'ruffle-probe'
    for rel in ['bin/ruffle', 'lib/libSDL2-2.0.so.0', 'lib/libasound.so.2', 'res/menu-font.ttf']:
        dst = frontend/rel; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(runtime/rel, dst)
    provenance = pak/'provenance'; provenance.mkdir(exist_ok=True)
    for rel in ['pak.json', 'build-manifest.json', 'launch.sh']:
        shutil.copyfile(runtime/rel, provenance/('leaf-12-'+rel))
    shutil.copyfile(ROOT/'runtime.lock.json', provenance/'runtime.lock.json')
    for rel in ['README.md', 'THIRD_PARTY.md', 'LICENSE']:
        shutil.copyfile(ROOT/rel, pak/rel)
    for rel in ['player-source.lock.json', 'rust-toolchain.lock.json']:
        shutil.copyfile(ROOT/rel, provenance/rel)
    make_icon(pak/'art'/'FLASH.png')
    movie_rel = Path(manifest['provides']['systems'][0]['rom_root'])/'Leaf-GPU-Probe.swf'
    make_movie(stage/movie_rel)
    (stage/'Images'/'flash').mkdir(parents=True, exist_ok=True)
    # The Apps probe is independent of library indexing and ROM folder setup.
    make_movie(frontend/'res'/'Leaf-GPU-Probe.swf')
    if not args.node: raise SystemExit('Node.js is required for the compiled C input tests')
    include = zig.parent/'lib'/'include'
    common = ['-emit-obj', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-I', ROOT/'src', '-isystem', include]
    # Invoke pinned Clang/LLD directly; Zig's Windows cc cache driver can fail
    # in managed sandboxes. No host CRT is needed for the C state tests.
    for source, object_name in [('src/probe_input.c', 'input-wasm.o'), ('tests/test_input.c', 'test-wasm.o')]:
        run([zig, '-cc1', '-triple', 'wasm32-unknown-unknown', '-ffreestanding', *common, source, '-o', build/object_name], env)
    test = build/'test-input.wasm'
    run([zig, 'wasm-ld', '--no-entry', '--export=probe_tests', build/'input-wasm.o', build/'test-wasm.o', '-o', test], env)
    run([args.node, ROOT/'tests'/'run_input.cjs', test], env)
    adapter_objects = []
    for source, object_name, defines in [('src/exit_probe.c', 'adapter-wasm.o', ['-DPROBE_WASM_TEST=1']),
                                         ('src/probe_input.c', 'adapter-input-wasm.o', []),
                                         ('src/frame_metrics.c', 'metrics-wasm.o', []),
                                         ('tests/test_adapter.c', 'fake-sdl-wasm.o', [])]:
        run([zig, '-cc1', '-triple', 'wasm32-unknown-unknown', '-ffreestanding', *common,
             '-I', ROOT/'tests', *defines, source, '-o', build/object_name], env)
        adapter_objects.append(build/object_name)
    adapter_test = build/'test-adapter.wasm'
    run([zig, 'wasm-ld', '--no-entry', '--export=adapter_tests', '--export=metric_tests',
         *adapter_objects, '-o', adapter_test], env)
    run([args.node, ROOT/'tests'/'run_adapter.cjs', adapter_test], env)
    run([sys.executable, ROOT/'tests'/'test_wrapper.py', '--shell', args.shell.resolve()], env)
    run([sys.executable, ROOT/'tests'/'test_session.py', '--shell', args.shell.resolve()], env)
    shim = frontend/'lib'/'libruffle_probe.so'
    target_includes = []
    for name in ['aarch64-linux-gnu', 'generic-glibc', 'aarch64-linux-any', 'any-linux-any']:
        target_includes += ['-isystem', zig.parent/'lib'/'libc'/'include'/name]
    for source, object_name in [('src/probe_input.c', 'input-arm.o'), ('src/frame_metrics.c', 'metrics-arm.o'), ('src/exit_probe.c', 'exit-arm.o')]:
        run([zig, '-cc1', '-triple', 'aarch64-unknown-linux-gnu', '-fgnuc-version=4.2.1',
             '-mrelocation-model', 'pic', '-pic-level', '2', *common, *target_includes,
             source, '-o', build/object_name], env)
    run([zig, 'ld.lld', '-shared', '--hash-style=both', '-soname', 'libruffle_probe.so', '-z', 'relro', '-z', 'now',
         build/'input-arm.o', build/'metrics-arm.o', build/'exit-arm.o', '-z', 'noexecstack', '-o', shim], env)
    info = inspect(shim)
    assert info['machine'] == 183 and info['type'] == 3, info
    assert {'SDL_PollEvent', 'SDL_GL_SwapWindow', 'SDL_GL_CreateContext'} <= set(info['exports'])
    # Preload shim resolves its C/POSIX imports from the already loaded libc.
    # It has no bundled libc or additional DT_NEEDED libraries.
    assert not info['needed'], info['needed']
    assert set(info['imports']) <= {'getenv', 'strcmp', 'strstr', 'dlsym', '_exit', 'clock_gettime', 'dprintf', 'memset', '__stack_chk_fail'}, info['imports']
    assert {'libSDL2-2.0.so.0'} <= set(inspect(frontend/'bin'/'ruffle')['needed'])
    report = {
        'version': version, 'runtime': 'user-supplied leaf-12; not rebuilt from source',
        'runtime_sha256': sha(frontend/'bin'/'ruffle'), 'exit_layer_sha256': sha(shim),
        'exit_layer_elf': info, 'host_input_tests': 'passed: C compiled to WebAssembly, executed with Node',
        'adapter_tests': 'passed: actual SDL interception implementation linked to fake SDL and executed as WASM',
        'wrapper_tests': 'passed',
        'session_tests': json.loads((build/'session-tests.json').read_text(encoding='utf8')),
        'contract_commit': contract_commit,
        'player_rebuilt_from_source': False,
        'runtime_source_status': 'public base candidate pinned; Leaf-specific modifications not reproduced',
        'implemented_scope': ['native input passthrough', 'Apps display probe retained', 'bounded local environment report', 'GL context evidence and present timing', 'data-write preflight and path containment', 'discoverable bundled CJK fallback font', 'Leaf PulseAudio default routing with custom configuration preserved', 'installed Leaf release record and libc diagnostics'],
        'not_implemented': ['new 12-profile input system', 'internal pause menu', 'reliable SharedObject storage and backup', 'suspend recovery'],
        'on_device_tests': 'not run; user hardware required',
        'references': json.loads((ROOT/'references.lock.json').read_text(encoding='utf8')),
    }
    report['player_source_lock'] = json.loads((ROOT/'player-source.lock.json').read_text(encoding='utf8'))
    if args.player_source:
        from verify_player_source import verify
        report['player_source_audit'] = verify(args.player_source)
    if args.contract:
        run([sys.executable, ROOT/'scripts'/'validate.py', '--pak', pak, '--contract', args.contract.resolve()], env)
        report['leaf_contract_validation'] = 'passed'
    (pak/'build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf8')
    output.mkdir(parents=True, exist_ok=True)
    binary_zip = output/f'ruffle-leaf-probe-{version}.zip'
    zip_tree(stage, binary_zip)
    with zipfile.ZipFile(binary_zip) as archive:
        assert archive.testzip() is None
        assert movie_rel.as_posix() in archive.namelist(), 'ROM folder casing changed in archive'
        assert not any(name.startswith('Roms/FLASH/') for name in archive.namelist()), 'legacy uppercase ROM folder leaked into package'
        assert 'Images/flash/' in archive.namelist()
        for rel in ['launch.sh', 'collect-diagnostics.sh', 'emulators/ruffle-probe/launch-game.sh', 'emulators/ruffle-probe/paths.sh', 'emulators/ruffle-probe/diagnostics.sh']:
            entry = archive.getinfo('Apps/mlp1/RuffleProbe.pak/'+rel)
            assert (entry.external_attr >> 16) & 0o111, 'missing executable mode: '+rel
    source_zip = output/f'ruffle-leaf-probe-source-{version}.zip'
    zip_tree(ROOT, source_zip, excluded=('build', '__pycache__', '.git'))
    shutil.copyfile(ROOT/'README.md', output/'ruffle-leaf-probe-README.md')
    report['archives'] = {p.name: sha(p) for p in [binary_zip, source_zip]}
    (output/'ruffle-leaf-probe-build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf8')
    print(json.dumps({'install_zip': str(binary_zip), 'source_zip': str(source_zip), 'sha256': report['archives']}, indent=2))

if __name__ == '__main__': main()
