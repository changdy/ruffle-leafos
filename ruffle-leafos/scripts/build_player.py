"""Build the pinned Ruffle player without changing global compiler settings."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['check', 'build', 'test'])
parser.add_argument('--offline', action='store_true')
parser.add_argument('--isolated-config', action='store_true',
                    help='Use only the project Cargo config and pinned CARGO_HOME, ignoring ancestor mirror settings')
parser.add_argument('--jobs', type=int, default=6)
parser.add_argument('--debug', action='store_true')
parser.add_argument('--tuning', choices=['generic', 'pocket1'], default='generic',
                    help='generic (default): existing opt-level 2/no cross-crate LTO; pocket1: experimental Cortex-A55, release opt-level 3 and ThinLTO')
parser.add_argument('--package', choices=['sdl2test-rocknix', 'ruffle_core'], default='sdl2test-rocknix',
                    help='Use ruffle_core with test to compile ARM64 core unit tests')
args = parser.parse_args()
if args.package == 'ruffle_core' and args.action != 'test':
    parser.error('--package ruffle_core requires the test action')
local = ROOT / 'workspace.local.json'
settings = json.loads(local.read_text(encoding='utf8')) if local.exists() else {}
cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work' / 'toolchains'))
rust = Path(settings.get('rust', cache / 'rust-1.94.0'))
mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
lock = json.loads((ROOT / 'arm-gnu.lock.json').read_text(encoding='utf8'))
cross = ROOT / lock['root']
target_libs = ROOT / 'target-libs'
for pc in (target_libs / 'pkgconfig').glob('*.pc'):
    original = pc.read_text(encoding='utf8')
    relocated = re.sub(r'^libdir=.*$', 'libdir=' + target_libs.as_posix(), original, flags=re.MULTILINE)
    if relocated != original:
        pc.write_text(relocated, encoding='utf8')
build = ROOT / 'build'
temporary = build / 'tmp'
temporary.mkdir(parents=True, exist_ok=True)
zig = Path(settings.get('zig', 'C:/Users/changdy/Documents/Codex/work/ruffle-probe-tools/zig/zig.exe'))
if not zig.is_file():
    raise SystemExit('Set workspace.local.json zig to the Zig 0.14.1 executable')
(build / 'llvm-tools.json').write_text(json.dumps({'zig': str(zig)}, indent=2), encoding='utf8')
for action in ('cc', 'ar', 'link'):
    (build / ('target-' + action + '.cmd')).write_text(
        '@echo off\n"' + sys.executable + '" "' + str(ROOT / 'scripts/clang_target.py') + '" ' + action + ' %*\n', encoding='utf8')
host_compat = build / 'host-compat'
host_compat.mkdir(exist_ok=True)
(host_compat / 'libgcc_eh.a').write_text(
    '/* w64devkit stores the SEH unwinder in libgcc.a. */\nINPUT ( -lgcc )\n', encoding='ascii')
host_linker = build / 'host-link.cmd'
host_linker.write_text('@echo off\n"' + str(mingw / 'bin/gcc.exe') +
    '" -L"' + host_compat.as_posix() + '" %*\n', encoding='utf8')
env = os.environ.copy()
# Cargo gives these global variables precedence over the target-specific flags.
# Clear them only in this child environment so the recorded CPU tuning is used.
env.pop('RUSTFLAGS', None)
env.pop('CARGO_ENCODED_RUSTFLAGS', None)
target_cpu = 'cortex-a55' if args.tuning == 'pocket1' else 'generic'
release_opt_level = '3' if args.tuning == 'pocket1' else '2'
release_lto = 'thin' if args.tuning == 'pocket1' else 'false'
env.update(CARGO_HOME=str(cache / 'cargo-home'), CARGO_TARGET_DIR=str(build / 'cargo'),
           TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary),
           CARGO_NET_GIT_FETCH_WITH_CLI='true', PKG_CONFIG_ALLOW_CROSS='1',
           PKG_CONFIG_LIBDIR=str(target_libs / 'pkgconfig'),
           PKG_CONFIG=str(mingw / 'bin' / 'pkg-config.exe'),
           CARGO_PROFILE_DEV_DEBUG='0', CARGO_PROFILE_RELEASE_DEBUG='0',
           CARGO_PROFILE_RELEASE_OPT_LEVEL=release_opt_level,
           CARGO_PROFILE_RELEASE_LTO=release_lto, CARGO_PROFILE_RELEASE_CODEGEN_UNITS='16')
env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(cross / 'bin'), str(mingw / 'bin'), env['PATH']])
# Rust's GNU host library requests gcc_eh; w64devkit uses the Rust-provided
# unwind implementation and needs the already audited compatibility archive.
env['CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER'] = str(host_linker)
jdk = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1'))
if jdk.is_dir():
    env['JAVA_HOME'] = str(jdk)
    env['PATH'] = str(jdk / 'bin') + os.pathsep + env['PATH']
for triple, compiler, archiver in [
    ('x86_64_pc_windows_gnu', mingw / 'bin/gcc.exe', mingw / 'bin/ar.exe'),
    ('aarch64_unknown_linux_gnu', build / 'target-cc.cmd', build / 'target-ar.cmd')]:
    env['CC_' + triple] = str(compiler)
    env['AR_' + triple] = str(archiver)
    env['CARGO_TARGET_' + triple.upper() + '_LINKER'] = str(compiler)
env['CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER'] = str(host_linker)
env['CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER'] = str(build / 'target-link.cmd')
env['CXX_aarch64_unknown_linux_gnu'] = str(build / 'target-cc.cmd')
env['CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS'] = (
    ('-Ctarget-cpu=cortex-a55 ' if args.tuning == 'pocket1' else '')
    + '-Lnative=' + str(target_libs) + ' -Clink-arg=-Wl,--allow-shlib-undefined')
cargo = rust / 'bin' / 'cargo.exe'
command = [str(cargo), args.action, '--locked', '--target', 'aarch64-unknown-linux-gnu',
           '-p', args.package, '-j', str(args.jobs)]
if args.package == 'ruffle_core':
    command += ['--lib', '--release', '--features',
                'audio,symphonia,mp3,nellymoser,lzma,default_compatibility_rules,default_font']
if args.offline:
    command.append('--offline')
if args.action == 'build' and not args.debug:
    command.append('--release')
if args.action == 'test':
    command.append('--no-run')  # Target tests require the actual ARM64 device.
working_directory = ROOT / 'player'
if args.isolated_config:
    # A global ~/.cargo/config.toml is also an ancestor config when the
    # checkout lives below the user profile. Select the project explicitly
    # from the drive root so an unrelated registry mirror cannot replace the
    # already populated offline crates.io cache. Never edit the user's config.
    command += ['--manifest-path', str(ROOT / 'player/Cargo.toml'),
                '--config', str(ROOT / 'player/.cargo/config.toml'),
                '--config', 'target.aarch64-unknown-linux-gnu.linker=' + json.dumps((build / 'target-link.cmd').as_posix()),
                '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps(host_linker.as_posix())]
    working_directory = Path(ROOT.anchor)
print(f'Player tuning={args.tuning} target_cpu={target_cpu} release_opt_level={release_opt_level} release_lto={release_lto} release_codegen_units=16', flush=True)
print(' '.join(command), flush=True)
result = subprocess.run(command, cwd=working_directory, env=env)
if result.returncode:
    raise SystemExit(result.returncode)
if args.action == 'build':
    profile = 'debug' if args.debug else 'release'
    binary = build / 'cargo' / 'aarch64-unknown-linux-gnu' / profile / 'sdl2test-rocknix'
    shutil.copyfile(binary, build / 'ruffle-leaf')
    sys.path.insert(0, str(ROOT / 'adapter' / 'scripts'))
    from elf_info import inspect
    info = inspect(build / 'ruffle-leaf')
    (build / 'player-elf.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf8')
    build_info = {
        'binary_sha256': hashlib.sha256((build / 'ruffle-leaf').read_bytes()).hexdigest(),
        'target': 'aarch64-unknown-linux-gnu', 'profile': profile,
        'tuning': args.tuning, 'target_cpu': target_cpu,
        'release_opt_level': int(release_opt_level), 'release_lto': release_lto,
        'release_codegen_units': 16,
        'rustc': subprocess.check_output([str(rust / 'bin/rustc.exe'), '--version'], env=env, text=True).strip(),
    }
    (build / 'player-build.json').write_text(json.dumps(build_info, indent=2) + '\n', encoding='utf8')
    print(json.dumps({'binary': str(build / 'ruffle-leaf'), 'elf': info}, indent=2), flush=True)
