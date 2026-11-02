"""Rebuild checked-in root LoaderInfo regression fixtures using the pinned ASC."""
from pathlib import Path
import argparse
import json
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


def build(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    local = ROOT / 'workspace.local.json'
    settings = json.loads(local.read_text(encoding='utf8')) if local.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    for name, filename in [('RootProgressProbe', 'root-progress.swf'), ('RootProgressGotoProbe', 'root-progress-goto.swf')]:
        source = output / (name + '.as')
        shutil.copyfile(ROOT / 'scripts/resources' / source.name, source)
        subprocess.run([str(java), '-classpath', str(ROOT / 'player/tools/asc/asc.jar'),
                        'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
                        '-outdir', str(output), str(source)], check=True)
        content = tag(82, struct.pack('<I', 0) + name.encode() + b'\0' + source.with_suffix('.abc').read_bytes())
        content += tag(76, struct.pack('<HH', 1, 0) + name.encode() + b'\0')
        write_movie(output / filename, content, avm2=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output.resolve())
