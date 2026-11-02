"""Build the original native-progress retry probe with the pinned ASC."""
from pathlib import Path
import argparse
import json
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


def build(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'ProgressRetryProbe.as'
    shutil.copyfile(ROOT / 'scripts/resources' / source.name, source)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    abc = source.with_suffix('.abc')
    abc.unlink(missing_ok=True)
    local = ROOT / 'workspace.local.json'
    settings = json.loads(local.read_text(encoding='utf8')) if local.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    result = subprocess.run([str(java), '-classpath', str(ROOT / 'player/tools/asc/asc.jar'),
                             'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
                             '-outdir', str(output), str(source)], check=True, capture_output=True, text=True)
    if not abc.is_file() or '[Compiler] Error' in result.stdout + result.stderr:
        raise RuntimeError(result.stdout + result.stderr)
    content = tag(82, struct.pack('<I', 0) + b'progress-retry\0' + abc.read_bytes())
    content += tag(76, struct.pack('<HH', 1, 0) + b'ProgressRetryProbe\0')
    write_movie(output / 'ProgressRetryProbe.swf', content, avm2=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output.resolve())
