"""Rebuild the small, original VM mouse-event regression SWF.

Requires the configured JDK and an existing playerglobal build. Intermediate
compiler files stay in the ignored build directory. Only the test SWF is copied
to the core test-data directory; this is not a packaged game asset.
"""
from pathlib import Path
import json
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


def main():
    fixtures = ROOT / 'player/core/src/player/tests'
    output = ROOT / 'build/diagnostics/mouse-events'
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'MouseEventsProbe.as'
    shutil.copyfile(fixtures / source.name, source)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    settings_file = ROOT / 'workspace.local.json'
    settings = json.loads(settings_file.read_text(encoding='utf8')) if settings_file.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    subprocess.run([str(java), '-classpath', str(ROOT / 'player/core/build_playerglobal/asc.jar'),
                    'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
                    '-outdir', str(output), str(source)], check=True)
    tags = tag(82, struct.pack('<I', 0) + b'mouse-events\0' + source.with_suffix('.abc').read_bytes())
    tags += tag(76, struct.pack('<HH', 1, 0) + b'MouseEventsProbe\0')
    write_movie(fixtures / 'mouse-events.swf', tags, avm2=True)


if __name__ == '__main__':
    main()
