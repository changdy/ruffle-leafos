"""Compile and run the dependency-free frontend transition tests on Windows."""
from pathlib import Path
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
settings_path = ROOT / 'workspace.local.json'
settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
rust = Path(settings.get('rust', cache / 'rust-1.94.0'))
mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
build = ROOT / 'build'
linker = build / 'host-link.cmd'
if not linker.exists():
    raise SystemExit('Run build_player.py check first to prepare local compiler wrappers')
env = os.environ.copy()
env['PATH'] = str(mingw / 'bin') + os.pathsep + env['PATH']
env['TEMP'] = env['TMP'] = str(build / 'tmp')
driver = build / 'frontend-tests.rs'
driver.write_text('#[path = ' + json.dumps(str(ROOT / 'player/sdl2test-rocknix/src/session.rs')) +
                  ']\nmod session;\n', encoding='utf8')
subprocess.run([str(rust / 'bin/rustc.exe'), '--edition', '2021', '--test',
    str(driver), '-C', 'linker=' + str(linker),
    '-o', str(build / 'session-tests.exe')], env=env, check=True)
result = subprocess.run([str(build / 'session-tests.exe')], env=env, text=True, capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='')
(build / 'session-test-report.json').write_text(json.dumps({
    'exit_code': result.returncode, 'output': result.stdout, 'stderr': result.stderr,
}, indent=2) + '\n', encoding='utf8')
raise SystemExit(result.returncode)
