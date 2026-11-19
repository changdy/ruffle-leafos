"""Package the locally built Pocket AIS player or the RGBox Screen Color app."""
import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from build_inputs import sha256_file, verify_player_build
from package_preview import bundle_samples, dependency_notices

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'adapter/scripts'))
from elf_info import inspect

WHEEL_SHA = 'fe923dbf5c7b27bbc1eb2bf58abfa793f8f13fd7ae8b27b1bc2de49920bcbd41'
FONT = PROJECT / 'runtime/leaf-12/res/menu-font.ttf'
APP = 'applyCenter/.ruffle-flash/'
SCREEN_COLOR = 'applyCenter/.screen-color/'


def member(archive, name, data, mode=0o644):
    info = ZipInfo(name)
    info.create_system = 3
    info.external_attr = (0o100000 | mode) << 16
    archive.writestr(info, data, compress_type=ZIP_DEFLATED)


def shell(archive, name, file):
    member(archive, name, file.read_bytes().replace(b'\r\n', b'\n'), 0o755)


def package_screen_color(binding, out):
    source = PROJECT / 'aisos'
    for name in ('screen_color.py', 'screen_color_ui.py'):
        member(out, SCREEN_COLOR + name, (source / name).read_bytes())
    shell(out, SCREEN_COLOR + 'install.sh', source / 'screen-color-install.sh')
    shell(out, 'applyCenter/Screen Color.app', source / 'Screen Color.app')
    member(out, 'applyCenter/images/Screen Color.png', (source / 'screen-color.png').read_bytes())
    member(out, SCREEN_COLOR + 'panel-font.ttf', FONT.read_bytes())
    member(out, SCREEN_COLOR + 'pysdl2.whl', binding)
    name = 'licenses/Source Han Sans OFL.txt'
    member(out, SCREEN_COLOR + name, (source / name).read_bytes())


def package_ruffle(binding, out, stage):
    binary = PROJECT / 'build/ruffle-leaf'
    build = json.loads((PROJECT / 'build/player-build.json').read_text(encoding='utf8'))
    verify_player_build(PROJECT, binary, build)
    elf = inspect(binary)
    if elf['machine'] != 183 or elf['type'] != 3:
        raise ValueError('Pocket AIS requires an AArch64 PIE player')
    if any(tuple(map(int, version[6:].split('.'))) > (2, 40) for version in elf['glibc_versions']):
        raise ValueError('Player requires a newer glibc than the tested AisOS firmware')
    player = binary.read_bytes()
    if hashlib.sha256(player).hexdigest() != build['binary_sha256']:
        raise ValueError('Player changed during packaging')
    member(out, APP + 'ruffle', player, 0o755)
    member(out, APP + 'MiSans-Normal.ttf', FONT.read_bytes())
    member(out, 'applyCenter/images/Ruffle Flash.png', (PROJECT / 'pak/art/FLASH.png').read_bytes())
    member(out, APP + 'pysdl2.whl', binding)
    source = PROJECT / 'aisos'
    member(out, APP + 'launcher.py', (source / 'launcher.py').read_bytes())
    shell(out, 'applyCenter/Ruffle Flash.app', source / 'Ruffle Flash.app')

    samples = bundle_samples(stage)
    sample_dir = stage / 'Roms/flash'
    for game in samples:
        name = game['install_path']
        member(out, 'flash/' + name, (sample_dir / name).read_bytes())
    probe = BytesIO()
    with ZipFile(probe, 'w') as game:
        member(game, 'Leaf-GPU-Probe.swf', (sample_dir / 'Leaf-GPU-Probe.swf').read_bytes())
    member(out, 'flash/Leaf-GPU-Probe.zip', probe.getvalue())

    notices = BytesIO()
    with ZipFile(notices, 'w') as licenses:
        for name, path in dependency_notices():
            member(licenses, 'licenses/' + name, path.read_bytes())
        for name, path in {
            'LICENSE': 'adapter/LICENSE',
            'THIRD_PARTY.md': 'docs/THIRD_PARTY.md',
            'licenses/RUFFLE-LICENSE.md': 'player/LICENSE.md',
            'licenses/JAWAKA-LICENSE.txt': 'vendor/jawaka/LICENSE',
            'licenses/CATASTROPHE-LICENSE.txt': 'vendor/catastrophe/LICENSE',
            'provenance/sample-games.json': 'games/samples/manifest.json',
        }.items():
            member(licenses, name, (PROJECT / path).read_bytes())
        with ZipFile(BytesIO(binding)) as wheel:
            for name in wheel.namelist():
                if '.dist-info/' in name and Path(name).name.upper().startswith(('LICENSE', 'COPYING')):
                    member(licenses, 'licenses/PySDL2/' + Path(name).name, wheel.read(name))
        member(licenses, 'build-manifest.json', json.dumps({
            'target': 'Pocket AIS / AisOS / AArch64', 'player_build': build,
            'elf': elf, 'sample_games': samples, 'pysdl2_wheel_sha256': WHEEL_SHA,
        }, indent=2, ensure_ascii=False).encode('utf8'))
    member(out, APP + 'notices.zip', notices.getvalue())
    verify_player_build(PROJECT, binary, build)


def package(wheel, output, screen_color=False):
    if output.resolve() == wheel.resolve():
        raise ValueError('Output must be distinct from the PySDL2 input')
    binding = wheel.read_bytes()
    if hashlib.sha256(binding).hexdigest() != WHEEL_SHA:
        raise ValueError(f'Unexpected SHA-256 for {wheel}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        staged = Path(tmp) / 'aisos.zip'
        with ZipFile(staged, 'w') as out:
            if screen_color:
                package_screen_color(binding, out)
            else:
                package_ruffle(binding, out, Path(tmp))
        with ZipFile(staged) as archive:
            if failed := archive.testzip():
                raise ValueError(f'Bad ZIP member: {failed}')
        staged.replace(output)
    print(f'{sha256_file(output)}  {output.name}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pysdl2_wheel', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--screen-color', action='store_true', help='Build only the Screen Color app')
    args = parser.parse_args()
    package(args.pysdl2_wheel, args.output, args.screen_color)
