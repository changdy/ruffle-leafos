"""Run SWF parser regressions on the Windows GNU host with a 512 MiB job limit."""
from pathlib import Path
import ctypes
from ctypes import wintypes
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def limited_test(binary, env):
    # The gate waits for job assignment before launching any test code. All
    # descendants inherit the job's aggregate memory limit and kill-on-close.
    class BasicLimits(ctypes.Structure):
        _fields_ = [('process_time', ctypes.c_longlong), ('job_time', ctypes.c_longlong),
                    ('flags', wintypes.DWORD), ('min_working_set', ctypes.c_size_t),
                    ('max_working_set', ctypes.c_size_t), ('active_processes', wintypes.DWORD),
                    ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD),
                    ('scheduling', wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [('basic', BasicLimits), ('io', IoCounters),
                    ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                    ('peak_process_memory', ctypes.c_size_t), ('peak_job_memory', ctypes.c_size_t)]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetInformationJobObject.restype = wintypes.BOOL
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    process = None
    try:
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000 | 0x200  # KILL_ON_JOB_CLOSE | JOB_MEMORY
        limits.job_memory = 512 * 1024 * 1024
        if not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        gate = 'import subprocess,sys; sys.stdin.readline(); sys.exit(subprocess.call(sys.argv[1:]))'
        process = subprocess.Popen(
            [sys.executable, '-c', gate, str(binary), '--test-threads=1'],
            cwd=ROOT / 'player/swf', env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if not kernel.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())
        output, _ = process.communicate('\n', timeout=180)
        return {'binary': str(binary), 'exit_code': process.returncode, 'output': output}
    finally:
        kernel.CloseHandle(job)
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate()


def main():
    if os.name != 'nt':
        raise SystemExit('This runner uses Windows Job Objects; run it on the pinned Windows host.')
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
    env.pop('RUSTFLAGS', None)
    env.pop('CARGO_ENCODED_RUSTFLAGS', None)
    env.update(CARGO_HOME=str(cache / 'cargo-home'), CARGO_TARGET_DIR=str(build / 'cargo'),
               TEMP=str(build / 'tmp'), TMP=str(build / 'tmp'),
               CARGO_PROFILE_DEV_DEBUG='0', CARGO_PROFILE_TEST_DEBUG='0',
               CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=str(linker))
    env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
    command = [str(rust / 'bin/cargo.exe'), 'test', '--locked', '--offline',
               '--target', 'x86_64-pc-windows-gnu', '-p', 'swf', '--lib', '--tests',
               '--no-run', '--message-format=json', '-j', '4',
               '--manifest-path', str(ROOT / 'player/Cargo.toml'),
               '--config', str(ROOT / 'player/.cargo/config.toml'),
               '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps(linker.as_posix())]
    result = subprocess.run(command, cwd=Path(ROOT.anchor), env=env, text=True, capture_output=True)
    print(result.stderr, end='', flush=True)
    binaries = []
    for line in result.stdout.splitlines():
        message = json.loads(line)
        if message.get('reason') == 'compiler-message':
            print(message['message']['rendered'], end='')
        if message.get('reason') == 'compiler-artifact' and message.get('executable'):
            binaries.append(Path(message['executable']))
    if result.returncode:
        raise SystemExit(result.returncode)
    if not binaries:
        raise SystemExit('Cargo returned no test executables')
    results = []
    for binary in binaries:
        record = limited_test(binary, env)
        results.append(record)
        print(record['output'], end='', flush=True)
    (build / 'swf-test-report.json').write_text(json.dumps({
        'memory_limit_mib': 512, 'timeout_seconds_per_binary': 180,
        'scope': 'Windows GNU parser tests; no device or game runtime acceptance',
        'results': results,
    }, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    raise SystemExit(0 if all(r['exit_code'] == 0 for r in results) else 1)


if __name__ == '__main__':
    main()
