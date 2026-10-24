"""Rebuild and test the small SDL diagnostics shim with the pinned LLVM tools."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/diagnostics'
BUILD.mkdir(parents=True, exist_ok=True)
ZIG = Path(json.loads((ROOT / 'build/llvm-tools.json').read_text())['zig'])
ADAPTER = ROOT / 'adapter'
sys.path.insert(0, str(ADAPTER / 'scripts'))
from elf_info import inspect


def run(args):
    subprocess.run(list(map(str, args)), check=True, cwd=ADAPTER)


common = ['-emit-obj', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
          '-I', ADAPTER / 'src', '-isystem', ZIG.parent / 'lib/include']
objects = []
for source, name, defines in [('src/exit_probe.c', 'shim', ['-DPROBE_WASM_TEST=1']),
                              ('src/probe_input.c', 'input', []),
                              ('src/frame_metrics.c', 'metrics', []),
                              ('tests/test_adapter.c', 'fake-sdl', [])]:
    obj = BUILD / (name + '-wasm.o')
    run([ZIG, '-cc1', '-triple', 'wasm32-unknown-unknown', '-ffreestanding',
         *common, '-I', ADAPTER / 'tests', *defines, ADAPTER / source, '-o', obj])
    objects.append(obj)
run([ZIG, 'wasm-ld', '--no-entry', '--export=adapter_tests', '--export=metric_tests',
     *objects, '-o', BUILD / 'adapter-tests.wasm'])
node = shutil.which('node')
if not node:
    raise SystemExit('Node.js is required to run the compiled shim tests')
run([node, ADAPTER / 'tests/run_adapter.cjs', BUILD / 'adapter-tests.wasm'])
includes = []
for name in ['aarch64-linux-gnu', 'generic-glibc', 'aarch64-linux-any', 'any-linux-any']:
    includes += ['-isystem', ZIG.parent / 'lib/libc/include' / name]
objects = []
for source, name in [('exit_probe.c', 'shim'), ('probe_input.c', 'input'), ('frame_metrics.c', 'metrics')]:
    obj = BUILD / (name + '-arm.o')
    run([ZIG, '-cc1', '-triple', 'aarch64-unknown-linux-gnu', '-fgnuc-version=4.2.1',
         '-mrelocation-model', 'pic', '-pic-level', '2', *common, *includes,
         ADAPTER / 'src' / source, '-o', obj])
    objects.append(obj)
library = ROOT / 'build/libruffle_probe.so'
run([ZIG, 'ld.lld', '-shared', '--hash-style=both', '-soname', 'libruffle_probe.so',
     '-z', 'relro', '-z', 'now', '-z', 'noexecstack', *objects, '-o', library])
info = inspect(library)
assert info['machine'] == 183 and info['type'] == 3 and not info['needed']
assert {'SDL_PollEvent', 'SDL_GL_SwapWindow', 'SDL_GL_CreateContext'} <= set(info['exports'])
(BUILD / 'report.json').write_text(json.dumps({'tests': 'passed', 'elf': info}, indent=2) + '\n', encoding='utf8')
print('PASS: compiled SDL interception and metric tests; AArch64 diagnostics library')
