"""Rebuild the Vector VM regression SWF and check its no-subclass assumption.

Requires the configured JDK and an existing playerglobal build. Compiler output
stays in the ignored build directory; the original AS3 sources and test SWF are
versioned. Run the compiled core unit test with the filter vector_native_calls.
"""
from pathlib import Path
import json
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


def main():
    fixtures = ROOT / 'player/core/src/player/tests'
    output = ROOT / 'build/diagnostics/vector-native'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    settings_file = ROOT / 'workspace.local.json'
    settings = json.loads(settings_file.read_text(encoding='utf8')) if settings_file.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'

    def compile_source(name):
        source = output / name
        shutil.copyfile(fixtures / name, source)
        abc = source.with_suffix('.abc')
        abc.unlink(missing_ok=True)
        result = subprocess.run(
            [str(java), '-classpath', str(ROOT / 'player/core/build_playerglobal/asc.jar'),
             'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import',
             str(output / 'playerglobal.abc'), '-outdir', str(output), str(source)],
            check=True, capture_output=True, text=True,
        )
        # ASC reports some errors with exit code 0, so also check its diagnostic
        # and whether this invocation produced a fresh ABC file.
        return abc, result.stdout + result.stderr

    abc, diagnostics = compile_source('VectorNativeProbe.as')
    if not abc.is_file() or '[Compiler] Error' in diagnostics:
        raise RuntimeError(diagnostics)
    tags = tag(82, struct.pack('<I', 0) + b'vector-native\0' + abc.read_bytes())
    tags += tag(76, struct.pack('<HH', 1, 0) + b'VectorNativeProbe\0')
    write_movie(fixtures / 'vector-native.swf', tags, avm2=True)

    abc, diagnostics = compile_source('VectorCannotExtend.as')
    if abc.exists() or 'Error #1016: Base class is final.' not in diagnostics:
        raise RuntimeError('Vector subclass was not rejected as expected:\n' + diagnostics)
    print('Vector probe rebuilt; parameterized Vector subclass rejected with compiler error 1016.')


if __name__ == '__main__':
    main()
