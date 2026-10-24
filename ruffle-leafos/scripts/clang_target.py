"""Small driver for Zig's Clang/LLD entry points and the pinned ARM sysroot.

Uses the LLVM tools directly: no global SDK installation or Zig cache is needed.
Only flags emitted by the pinned cc-rs/Rust build are supported; unknown flags
are passed to LLVM so unsupported changes fail visibly.
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / 'build' / 'llvm-tools.json').read_text(encoding='utf8'))
ZIG = Path(CONFIG['zig'])
SDK = ROOT / json.loads((ROOT / 'arm-gnu.lock.json').read_text())['root']
SYSROOT = SDK / 'aarch64-none-linux-gnu' / 'libc'
GCCLIB = SDK / 'lib/gcc/aarch64-none-linux-gnu/14.2.1'
LIBC = SYSROOT / 'usr/lib64'
TRIPLE = 'aarch64-unknown-linux-gnu'


def run(args):
    return subprocess.call([str(ZIG), *map(str, args)])


def compile_c(args):
    if '--version' in args:
        print('clang version 19.1.7 (Zig 0.14.1 raw frontend)')
        return 0
    translated = []
    source = None
    output = None
    preprocess = '-E' in args
    i = 0
    while i < len(args):
        arg = args[i]
        i += 1
        if arg in ('-c', '-E', '-fno-plt', '-fno-omit-frame-pointer') or arg.startswith(('-g', '--target=')):
            continue
        if arg in ('-fPIC', '-fpic'):
            translated += ['-mrelocation-model', 'pic', '-pic-level', '2']
        elif arg == '-o':
            output = args[i]
            i += 1
        elif arg in ('-I', '-isystem', '-D', '-U', '-include'):
            translated += [arg, args[i]]
            i += 1
        elif not arg.startswith('-'):
            source = arg
        else:
            translated.append(arg)
    if source is None:
        raise SystemExit('clang_target: a C or assembly source is required')
    assembly = Path(source).suffix in ('.S', '.s')
    base = ['-cc1', '-triple', TRIPLE, '-fgnuc-version=4.2.1',
            '-internal-isystem', str(ZIG.parent / 'lib/include'),
            '-internal-isystem', str(SYSROOT / 'usr/include'),
            '-target-feature', '+neon']
    if assembly and not preprocess:
        temporary = Path(output).with_suffix('.preprocessed.s')
        status = run([*base, '-E', '-x', 'assembler-with-cpp', *translated,
                      '-o', temporary, source])
        if status:
            return status
        try:
            return run(['-cc1as', '-triple', TRIPLE, '-filetype', 'obj',
                        '-target-feature', '+neon', '-o', output, temporary])
        finally:
            temporary.unlink(missing_ok=True)
    command = [*base, '-E' if preprocess else '-emit-obj', *translated]
    if output:
        command += ['-o', output]
    return run([*command, source])


def link(args):
    expanded = []
    for arg in args:
        if arg.startswith('@'):
            expanded.extend(shlex.split(Path(arg[1:]).read_text(encoding='utf8'), posix=True))
        else:
            expanded.append(arg)
    args = expanded
    command = ['ld.lld', '-m', 'aarch64elf', '--sysroot=' + str(SYSROOT),
               '--dynamic-linker', '/lib/ld-linux-aarch64.so.1']
    for folder in [GCCLIB, LIBC, SYSROOT / 'lib64', SYSROOT / 'lib',
                   SDK / 'aarch64-none-linux-gnu/lib64']:
        command += ['-L', str(folder)]
    startup = '-nostartfiles' not in args and '-nostdlib' not in args
    shared = '-shared' in args
    if startup:
        if not shared:
            command += [str(LIBC / ('Scrt1.o' if '-pie' in args else 'crt1.o'))]
        command += [str(LIBC / 'crti.o'), str(GCCLIB / 'crtbeginS.o')]
    for arg in args:
        if arg.startswith('-Wl,'):
            command.extend(arg[4:].split(','))
        elif arg == '-pthread':
            command.append('-lpthread')
        elif arg == '-rdynamic':
            command.append('--export-dynamic')
        elif arg in ('-nodefaultlibs', '-nostartfiles', '-nostdlib', '-fPIC',
                     '-fno-use-linker-plugin', '-fuse-ld=lld', '-m64', '-static-libgcc'):
            continue
        else:
            command.append(arg)
    if startup:
        command += [str(GCCLIB / 'crtendS.o'), str(LIBC / 'crtn.o')]
    # The Ruffle link has hundreds of inputs, exceeding Windows' command-line
    # limit after expanding Rust's GCC response file. LLD gets its own file.
    with tempfile.NamedTemporaryFile(mode='w', suffix='.lld.rsp', encoding='utf8',
                                     dir=ROOT / 'build/tmp', delete=False) as response:
        for arg in command[1:]:
            response.write('"' + str(arg).replace('\\', '/').replace('"', '\\"') + '"\n')
        response_path = Path(response.name)
    status = run(['ld.lld', '@' + str(response_path)])
    if not status:
        response_path.unlink()
    return status


if __name__ == '__main__':
    action, *arguments = sys.argv[1:]
    if action == 'cc':
        status = compile_c(arguments)
    elif action == 'ar':
        status = run(['ar', *arguments])
    elif action == 'link':
        status = link(arguments)
    else:
        raise SystemExit('unknown LLVM driver action: ' + action)
    raise SystemExit(status)
