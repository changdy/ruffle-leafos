"""Execute real launch/diagnostic scripts against an inert player fixture."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--shell', required=True, type=Path)
args = parser.parse_args()
prefix = [str(args.shell.resolve())] + (['sh'] if 'busybox' in args.shell.name.lower() else [])
count = 0
skipped = []

def check(condition, detail):
    global count
    assert condition, detail
    count += 1

def argument(path):
    value = path.resolve().as_posix()
    return value[2:] if os.name == 'nt' else value

with tempfile.TemporaryDirectory(prefix='session-', dir=ROOT/'build') as temporary:
    base = Path(temporary)
    card = base/'primary card'
    pak = card/'Apps'/'mlp1'/'RuffleProbe.pak'
    shutil.copytree(ROOT/'pak', pak)
    front = pak/'emulators'/'ruffle-probe'
    for script in pak.rglob('*.sh'):
        script.chmod(0o755)
        subprocess.run(prefix+['-n', str(script)], check=True)
    for name in ['lib/libruffle_probe.so', 'lib/libSDL2-2.0.so.0', 'lib/libasound.so.2', 'res/menu-font.ttf', 'res/Leaf-GPU-Probe.swf']:
        file = front/name
        file.parent.mkdir(exist_ok=True, parents=True)
        file.write_bytes(b'fixture')
    stub = front/'bin'/'ruffle'
    stub.parent.mkdir(parents=True)
    stub.write_text('#!/bin/sh\n'
                    'printf "mode=%s\\nmouse=%s\\nhome=%s\\nconfig=%s\\nworkdir=%s\\n" '
                    '"$RUFFLE_PROBE_ANY_KEY_EXIT" "$RUFFLE_START_IN_MOUSE_MODE" "$HOME" "$XDG_CONFIG_HOME" "$PWD"\n'
                    'printf "arg=<%s>\\n" "$@"\n', encoding='utf8')
    stub.chmod(0o755)
    roms = base/'secondary card'/'Roms'
    games = roms/'flash'
    games.mkdir(parents=True)
    game = games/"测试 ' $(touch PWNED) & game.swf"
    game.write_bytes(b'FWS')
    userdata = base/'persistent data'
    logs = base/'logs'
    saves = base/'saves'
    saves.mkdir()
    (saves/'existing.sol').write_bytes(b'untouched-progress')
    env = os.environ.copy()
    for name in list(env):
        if name.startswith(('RUFFLE_', 'LD_')) or name in ('UMRK_ENV_FILE', 'SDCARD_PATH'):
            env.pop(name, None)
    env.update(ROMS_PATH=argument(roms), USERDATA_PATH=argument(userdata), LOGS_PATH=argument(logs),
               SAVES_PATH=argument(saves), PLATFORM='mlp1', BB_SH_STANDALONE='1')

    def invoke(argv=(), changes=None, entry=None, dry=True):
        current = env.copy()
        if dry:
            current['RUFFLE_PROBE_DRY_RUN'] = '1'
        if changes:
            current.update(changes)
        return subprocess.run(prefix+[str(entry or front/'launch-game.sh'), *argv], env=current, cwd=base,
                              capture_output=True, text=True, encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace')

    result = invoke([argument(game)])
    check(result.returncode == 0, result.stderr)
    check('session_mode=native\nany_key_exit=0\n' in result.stdout, result.stdout)
    check(f'saves={argument(saves)}\n' in result.stdout, result.stdout)
    check(not logs.exists() and not userdata.exists(), 'dry run caused writes')
    result = invoke(entry=pak/'launch.sh')
    check(result.returncode == 0 and 'session_mode=probe\nany_key_exit=1\n' in result.stdout, result)
    result = invoke([argument(game)], entry=pak/'launch.sh')
    check(result.returncode == 0 and 'session_mode=native' in result.stdout, result)
    result = invoke([argument(game)], {'RUFFLE_SESSION_MODE': 'probe'})
    check(result.returncode == 0 and 'any_key_exit=1' in result.stdout, result)
    for key, value in [('RUFFLE_SESSION_MODE', 'bad'), ('RUFFLE_START_IN_MOUSE_MODE', 'bad'),
                       ('RUFFLE_PROBE_TIMING', 'bad'), ('USERDATA_PATH', 'relative'), ('PLATFORM', 'wrong')]:
        check(invoke([argument(game)], {key: value}).returncode != 0, f'accepted {key}={value}')
    outside = roms/'outside.swf'
    outside.write_bytes(b'FWS')
    check(invoke([argument(games)+'/../outside.swf']).returncode != 0, 'accepted traversal')
    link = games/'escape.swf'
    try:
        link.symlink_to(outside)
    except OSError:
        skipped.append('file symlink escape: Windows symlink privilege unavailable')
    else:
        check(invoke([argument(games)+'/escape.swf']).returncode != 0, 'accepted symlink escape')
        link.unlink()
    # If only LOGS_PATH is missing, env.sh must not replace selected-card paths.
    fallback = base/'env.sh'
    fallback.write_text("ROMS_PATH='/wrong-card'\nUSERDATA_PATH='/wrong-data'\nSAVES_PATH='/wrong-saves'\n"
                        f"LOGS_PATH='{argument(logs)}'\n", encoding='utf8')
    result = invoke([argument(game)], {'LOGS_PATH': '', 'UMRK_ENV_FILE': argument(fallback)})
    check(result.returncode == 0 and f'state={argument(userdata)}/ruffle-probe\n' in result.stdout, result)
    check(f'saves={argument(saves)}\n' in result.stdout, 'SAVES_PATH overwritten by env.sh')

    # Full execution reaches the inert fixture via exec with isolated HOME/CWD.
    result = invoke([argument(game)], dry=False)
    check(result.returncode == 0, result.stderr)
    log = logs/'ruffle-probe.log'
    text = log.read_text(encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace')
    check('mode=0\nmouse=1\n' in text, text)
    check(f'home={argument(userdata)}/ruffle-probe/home\n' in text, text)
    check(f'workdir={userdata.as_posix()}/ruffle-probe\n' in text, text)
    check(f'arg=<{game.as_posix()}>\narg=<--fullscreen>\n' in text, text)
    check(not (base/'PWNED').exists(), 'path executed as shell code')
    report = logs/'ruffle-environment.txt'
    check(report.is_file(), text)
    report_text = report.read_text(encoding='utf8')
    check('leaf_version_export=unknown' in report_text, report_text)
    check('NOT detected device version' in report_text, 'min version confused with observed version')
    check('custom player source unresolved' in report_text, 'unproven source claim')
    check('cloud' not in report_text.lower(), 'unexpected cloud dependency')
    font_cache = userdata/'ruffle-probe'/'home'/'.local'/'share'/'fonts'/'ruffle-leaf'/'MiSans-Normal.ttf'
    check(font_cache.read_bytes() == b'fixture', 'bundled CJK font not discoverable in HOME font directory')
    # Reproduce an old or interrupted font cache from a previous installation.
    # Starting again must repair it without touching progress or configuration.
    font_cache.write_bytes(b'old-incomplete-font')
    sentinel = userdata/'ruffle-probe'/'config'/'keep.cfg'
    sentinel.write_bytes(b'keep-configuration')
    for name in ['VERSION', 'release.json']:
        target = card/'.system'/'leaf'/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('fixture-device-version', encoding='utf8')
    release_record = card/'.umrk'/'mlp1'/'release.json'
    release_record.parent.mkdir(parents=True, exist_ok=True)
    release_record.write_text('{"product":"leaf","version":"0.13.0","source":"managed-install"}', encoding='utf8')
    result = invoke(entry=pak/'collect-diagnostics.sh', dry=False)
    check(result.returncode == 0, result.stderr)
    check(report.with_name(report.name+'.previous').read_text(encoding='utf8') == report_text, 'report rotation lost prior version')
    check('fixture-device-version' in report.read_text(encoding='utf8'), 'version evidence not collected')
    check('managed-install' in report.read_text(encoding='utf8'), 'Leaf installed release record not collected')
    result = invoke([argument(game)], {'RUFFLE_SESSION_MODE': 'probe'}, dry=False)
    check(result.returncode == 0, result.stderr)
    check('mode=1\nmouse=0\n' in log.read_text(encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace'), 'probe environment changed')
    check(font_cache.read_bytes() == b'fixture', 'stale or incomplete font cache was not repaired')
    check((logs/'ruffle-probe.log.previous').read_text(encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace') == text, 'log rotation failed')
    # Reinstallation replaces pak files only; configuration and progress survive.
    shutil.copytree(ROOT/'pak', pak, dirs_exist_ok=True)
    check(sentinel.read_bytes() == b'keep-configuration', 'upgrade removed configuration')
    check((saves/'existing.sol').read_bytes() == b'untouched-progress', 'upgrade touched progress')
    result = invoke([argument(game)], {'USERDATA_PATH': argument(pak/'forbidden-data')}, dry=False)
    check(result.returncode != 0, 'accepted writes into pak')
    check(not (pak/'forbidden-data').exists(), 'preflight created a pak directory before rejecting it')
    check(invoke([argument(game)], entry=pak/'collect-diagnostics.sh', dry=False).returncode != 0, 'collector accepted game argument')
    blocker = base/'not-a-directory'
    blocker.write_bytes(b'x')
    check(invoke([argument(game)], {'USERDATA_PATH': argument(blocker)}, dry=False).returncode != 0, 'unwritable state did not fail')
    for name in ['lib/libruffle_probe.so', 'lib/libasound.so.2', 'res/menu-font.ttf']:
        missing = front/name
        content = missing.read_bytes()
        missing.unlink()
        check(invoke([argument(game)]).returncode != 0, f'missing dependency accepted: {name}')
        missing.write_bytes(content)

summary = {'checks': count, 'skipped': skipped, 'arm_execution': False}
(ROOT/'build'/'session-tests.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf8')
print(f'PASS: {count} session/diagnostics checks; skips={skipped}')
