"""Run the real ZIP extraction/cache tests on the Windows GNU host."""
from pathlib import Path
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
settings_path = ROOT / 'workspace.local.json'
settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
version = json.loads((ROOT / 'adapter/rust-toolchain.lock.json').read_text(encoding='utf8'))['rust_version']
rust = Path(settings.get('rust', cache / ('rust-' + version)))
mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
build = ROOT / 'build'
linker = build / 'host-link.cmd'
if not linker.exists():
    raise SystemExit('Run build_player.py check first')
project = build / 'zip-game-test'
(project / 'src').mkdir(parents=True, exist_ok=True)
(project / 'Cargo.toml').write_text('[package]\nname = "zip-game-test"\nversion = "0.1.0"\nedition = "2021"\n'
    '[dependencies]\nzip = { version = "=8.6.0", default-features = false, features = ["deflate"] }\nsha2 = "=0.10.9"\ncrc32fast = "=1.5.0"\n', encoding='utf8')
source = ROOT / 'player/sdl2test-rocknix/src/zip_game.rs'
(project / 'src/lib.rs').write_text('#[path = ' + json.dumps(source.as_posix()) + ']\nmod zip_game;\n', encoding='utf8')
env = os.environ.copy()
env.update(CARGO_HOME=str(cache / 'cargo-home'), CARGO_TARGET_DIR=str(build / 'cargo'),
           TEMP=str(build / 'tmp'), TMP=str(build / 'tmp'), CARGO_PROFILE_TEST_DEBUG='0',
           CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=str(linker))
env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
command = [str(rust / 'bin/cargo.exe'), 'test', '--offline', '--target', 'x86_64-pc-windows-gnu',
           '--manifest-path', str(project / 'Cargo.toml'), '--config', str(ROOT / 'player/.cargo/config.toml'),
           '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps(linker.as_posix())]
result = subprocess.run(command, cwd=Path(ROOT.anchor), env=env, text=True, encoding='utf8', errors='replace', capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='')
raise SystemExit(result.returncode)
