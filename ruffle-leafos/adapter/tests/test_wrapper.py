"""Exercise the real POSIX launch wrapper without executing the ARM64 player."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--shell', required=True, type=Path, help='BusyBox executable, or /bin/sh')
args = parser.parse_args()
shell = str(args.shell.resolve())
prefix = [shell, 'sh'] if 'busybox' in args.shell.name.lower() else [shell]
wrapper_source = ROOT/'pak'/'emulators'/'ruffle-probe'/'launch-game.sh'
app_source = ROOT/'pak'/'launch.sh'
subprocess.run(prefix + ['-n', str(wrapper_source)], check=True)
subprocess.run(prefix + ['-n', str(app_source)], check=True)

def unix_argument(path):
    result = path.resolve().as_posix()
    # BusyBox-w32 accepts /Users/... relative to the current Windows drive.
    return result[2:] if os.name == 'nt' else result

count = 0
with tempfile.TemporaryDirectory(prefix='wrapper-', dir=ROOT/'build') as temporary:
    base = Path(temporary)
    install_card = base/'installed card'
    pak = install_card/'Apps'/'mlp1'/'RuffleProbe.pak'
    frontend = pak/'emulators'/'ruffle-probe'
    for rel in ['bin/ruffle', 'lib/libruffle_probe.so', 'lib/libSDL2-2.0.so.0', 'lib/libasound.so.2', 'res/menu-font.ttf']:
        path = frontend/rel; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'not executable: dry-run fixture')
    wrapper = frontend/'launch-game.sh'; shutil.copyfile(wrapper_source, wrapper)
    wrapper.chmod(0o755)
    for extra in ('paths.sh', 'diagnostics.sh'):
        shutil.copyfile(wrapper_source.parent/extra, frontend/extra); (frontend/extra).chmod(0o755)
    app = pak/'launch.sh'; shutil.copyfile(app_source, app); app.chmod(0o755)
    demo = frontend/'res'/'Leaf-GPU-Probe.swf'
    demo.parent.mkdir(parents=True, exist_ok=True); demo.write_bytes(b'FWS')
    for card, folder in [(card, folder) for card in ('card-one', 'card-two') for folder in ('flash', 'FLASH')]:
        card_root = base/(card + ('-lower' if folder == 'flash' else '-legacy'))
        roms = card_root/'Roms'; games = roms/folder/'Chinese names and spaces'
        games.mkdir(parents=True)
        game = games/'小样 $(touch PWNED).SWF'; game.write_bytes(b'FWS')
        env = os.environ.copy()
        env.update(ROMS_PATH=unix_argument(roms), USERDATA_PATH=unix_argument(card_root/'userdata'),
                   LOGS_PATH=unix_argument(card_root/'logs'), PLATFORM='mlp1', RUFFLE_PROBE_DRY_RUN='1',
                   BB_SH_STANDALONE='1')
        def invoke(arguments, changes=None, entry=wrapper):
            current = env.copy()
            if changes: current.update(changes)
            return subprocess.run(prefix+[str(entry), *arguments], env=current, cwd=base,
                                  capture_output=True, text=True, encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace')
        result = invoke([unix_argument(game)])
        assert result.returncode == 0, result.stderr
        assert f'argument={game.resolve().as_posix()}\n' in result.stdout, result.stdout
        assert result.stdout.count('argument=') == 2, result.stdout
        assert 'argument=--fullscreen' in result.stdout
        assert f'flash_root={(roms/folder).resolve().as_posix()}\n' in result.stdout, result.stdout
        assert not (base/'PWNED').exists(), 'path was treated as shell code'
        count += 1
        for argv in ([], ['relative.swf'], [unix_argument(game), '--extra']):
            assert invoke(argv).returncode != 0; count += 1
        assert invoke([unix_argument(game)], {'ROMS_PATH': ''}).returncode != 0; count += 1
        missing = games/'missing.swf'
        assert invoke([unix_argument(missing)]).returncode != 0; count += 1
        invalid = games/'not-a-movie.txt'; invalid.write_bytes(b'x')
        assert invoke([unix_argument(invalid)]).returncode != 0; count += 1
        outside = roms/'other.swf'; outside.write_bytes(b'FWS')
        assert invoke([unix_argument(outside)]).returncode != 0; count += 1
        # A direct argument to the Apps entry still uses the selected game's card.
        result = invoke([unix_argument(game)], entry=app)
        assert result.returncode == 0, result.stderr
        assert f'argument={game.resolve().as_posix()}\n' in result.stdout
        assert f'flash_root={(roms/folder).resolve().as_posix()}\n' in result.stdout
        count += 1
        # No arguments means one fixed internal demo, not the runtime's browser.
        result = invoke([], entry=app)
        assert result.returncode == 0, result.stderr
        assert f'argument={demo.resolve().as_posix()}\n' in result.stdout, result.stdout
        count += 1
    bare_env = os.environ.copy()
    for key in ('ROMS_PATH', 'USERDATA_PATH', 'LOGS_PATH', 'SDCARD_PATH', 'UMRK_ENV_FILE', 'PLATFORM', 'RUFFLE_PROBE_APP_MODE'):
        bare_env.pop(key, None)
    bare_env.update(RUFFLE_PROBE_DRY_RUN='1', BB_SH_STANDALONE='1')
    def app_without_leaf():
        return subprocess.run(prefix+[str(app)], env=bare_env, cwd=base, capture_output=True,
                              text=True, encoding='mbcs' if os.name == 'nt' else 'utf8', errors='replace')
    result = app_without_leaf()
    assert result.returncode == 0, result.stderr
    assert f'argument={demo.resolve().as_posix()}\n' in result.stdout
    assert f'state={(install_card/".userdata"/"mlp1"/"ruffle-probe").resolve().as_posix()}\n' in result.stdout
    count += 1
    # Legacy and platform-specific env.sh are both accepted by the Apps route.
    for rel in ('.system/leaf/launcher/env.sh', '.system/leaf/platforms/mlp1/launcher/env.sh'):
        env_file = install_card/rel; env_file.parent.mkdir(parents=True, exist_ok=True)
        chosen = base/('platform-data' if 'platforms' in rel else 'legacy-data')
        env_file.write_text(f"USERDATA_PATH='{chosen.resolve().as_posix()}'\n", encoding='utf8')
        result = app_without_leaf()
        assert result.returncode == 0, result.stderr
        assert f'state={(chosen/"ruffle-probe").resolve().as_posix()}\n' in result.stdout, result.stdout
        count += 1
    demo.unlink()
    assert app_without_leaf().returncode != 0, 'missing bundled movie must fail, not enter browser'
    count += 1
    assert not list(base.rglob('ruffle-probe.log')), 'dry-run must not execute or create a runtime log'
print(f'PASS: Apps/core shell syntax and {count} checks (selected game, built-in Apps demo, env.sh fallbacks, lowercase flash / legacy FLASH, mount roots, Unicode/quoting, missing arguments, wrong paths, no runtime execution)')
