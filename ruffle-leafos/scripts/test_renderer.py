"""Run wgpu bitmap ownership regressions without a GPU or Vulkan driver."""
from pathlib import Path
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
local = ROOT / 'workspace.local.json'
settings = json.loads(local.read_text(encoding='utf8')) if local.exists() else {}
cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
rust = Path(settings.get('rust', cache / 'rust-1.94.0'))
mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
build = ROOT / 'build'
linker = build / 'host-link.cmd'
if not linker.exists():
    raise SystemExit('Run build_player.py check first to prepare local compiler wrappers')
(build / 'tmp').mkdir(exist_ok=True)
env = os.environ.copy()
env.update(CARGO_HOME=str(cache / 'cargo-home'), CARGO_TARGET_DIR=str(build / 'cargo'),
           TEMP=str(build / 'tmp'), TMP=str(build / 'tmp'),
           CARGO_PROFILE_DEV_DEBUG='0', CARGO_PROFILE_TEST_DEBUG='0',
           CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=str(linker))
env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
command = [str(rust / 'bin/cargo.exe'), 'test', '--locked', '--offline',
           '--target', 'x86_64-pc-windows-gnu', '-p', 'ruffle_render_wgpu',
           '--test', 'bitmap_lifetime', '-j', '4',
           '--manifest-path', str(ROOT / 'player/Cargo.toml'),
           '--config', str(ROOT / 'player/.cargo/config.toml'),
           '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps(linker.as_posix())]
# Isolate ancestor Cargo mirror settings in the same way as build_player.py.
result = subprocess.run(command, cwd=Path(ROOT.anchor), env=env, text=True, capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='')
(build / 'renderer-test-report.json').write_text(json.dumps({
    'exit_code': result.returncode, 'output': result.stdout, 'stderr': result.stderr,
    'backend': 'wgpu noop; resource ownership only, no pixel or GPU acceptance',
}, indent=2) + '\n', encoding='utf8')
raise SystemExit(result.returncode)
