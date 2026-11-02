"""Run actual Ruffle core unit and storage regressions on the Windows GNU host.

These tests use Null backends. They exercise VM, event, storage and cache
semantics, but do not establish Pocket1 graphics, audio or input acceptance.
"""
from pathlib import Path
import json
import os
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main():
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
    cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
    rust_version = json.loads((ROOT / 'adapter/rust-toolchain.lock.json').read_text(encoding='utf8'))['rust_version']
    rust = Path(settings.get('rust', cache / ('rust-' + rust_version)))
    mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
    build = ROOT / 'build'
    linker = build / 'host-link.cmd'
    if not linker.is_file():
        raise SystemExit('Run build_player.py check first to prepare local compiler wrappers')
    (build / 'tmp').mkdir(exist_ok=True)
    env = os.environ.copy()
    env.pop('RUSTFLAGS', None)
    env.pop('CARGO_ENCODED_RUSTFLAGS', None)
    env.update(
        CARGO_HOME=str(cache / 'cargo-home'), CARGO_TARGET_DIR=str(build / 'cargo'),
        TEMP=str(build / 'tmp'), TMP=str(build / 'tmp'),
        CARGO_PROFILE_DEV_DEBUG='0', CARGO_PROFILE_TEST_DEBUG='0',
        CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=str(linker),
        CC_x86_64_pc_windows_gnu=str(mingw / 'bin/gcc.exe'),
        AR_x86_64_pc_windows_gnu=str(mingw / 'bin/ar.exe'),
    )
    env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
    jdk = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1'))
    if jdk.is_dir():
        env['JAVA_HOME'] = str(jdk)
        env['PATH'] = str(jdk / 'bin') + os.pathsep + env['PATH']
    command = [
        str(rust / 'bin/cargo.exe'), 'test', '--locked', '--offline',
        '--target', 'x86_64-pc-windows-gnu', '-p', 'ruffle_core', '--lib', '--tests',
        '--features', 'audio,symphonia,mp3,nellymoser,lzma,default_compatibility_rules,default_font',
        '-j', '4', '--manifest-path', str(ROOT / 'player/Cargo.toml'),
        '--config', str(ROOT / 'player/.cargo/config.toml'),
        '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps(linker.as_posix()),
        '--', '--test-threads=1',
    ]
    result = subprocess.run(command, cwd=Path(ROOT.anchor), env=env,
                            encoding='utf8', capture_output=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    (build / 'core-test-report.json').write_text(json.dumps({
        'exit_code': result.returncode, 'output': result.stdout, 'stderr': result.stderr,
        'backend': 'Null backends; VM and cache semantics, no Pocket1 device acceptance',
    }, indent=2) + '\n', encoding='utf8')
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
