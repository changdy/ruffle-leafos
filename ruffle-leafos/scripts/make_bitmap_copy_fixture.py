"""Build an original AVM2 probe for BitmapData.copyPixels after GPU drawing."""
from pathlib import Path
import argparse
import json
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


def build(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'BitmapCopyProbe.as'
    shutil.copyfile(Path(__file__).parent / 'resources/BitmapCopyProbe.as', source)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    subprocess.run([str(java), '-classpath', str(ROOT / 'player/core/build_playerglobal/asc.jar'),
                    'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
                    '-outdir', str(output), str(source)], check=True)
    content = tag(82, struct.pack('<I', 0) + b'bitmap-copy-probe\0' + source.with_suffix('.abc').read_bytes())
    content += tag(76, struct.pack('<HH', 1, 0) + b'BitmapCopyProbe\0')
    write_movie(output / 'BitmapCopyProbe.swf', content, avm2=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output.resolve())
