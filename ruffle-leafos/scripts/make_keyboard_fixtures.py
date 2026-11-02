"""Generate original AVM1/AVM2 input fixtures for --self-test-keyboard.

Requires a completed current core build (debug or release) and the configured JDK.
Generated SWF/ABC files belong in ignored build/output directories.
"""
from pathlib import Path
import argparse
import json
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'adapter/scripts'))
from make_demo import rect, matrix, tag


def action(code, data=b''):
    return bytes([code]) + (struct.pack('<H', len(data)) + data if code >= 128 else b'')


def push(value):
    if isinstance(value, str):
        data = b'\0' + value.encode('utf8') + b'\0'
    else:
        data = b'\x07' + struct.pack('<i', value)
    return action(0x96, data)


def write_movie(path, content, avm2=False):
    body = rect(960*20, 720*20) + struct.pack('<HH', 24*256, 1)
    body += tag(69, struct.pack('<I', 8 if avm2 else 0))
    body += tag(9, bytes([35, 45, 58])) + content + tag(1) + tag(0)
    path.write_bytes(b'FWS\x0a' + struct.pack('<I', len(body)+8) + body)


FIELDS = [('normal', 'seed', 110), ('password', 'secret', 210),
          ('digits', '12', 310), ('multi', 'line', 410), ('readonly', 'Read only', 510)]


def avm1(output):
    tags = b''
    formats = b''
    for identifier, (name, text, y) in enumerate(FIELDS, 1):
        # DefineEditText, device font fallback, 32px. Keep all user text original.
        flags = 1 | 4 | 128 | 2048
        if name == 'password': flags |= 16
        if name == 'multi': flags |= 32
        if name == 'readonly': flags |= 8
        if name == 'digits': flags |= 2
        body = struct.pack('<H', identifier) + rect(840*20, 60*20) + struct.pack('<HHH', flags, 0, 32*20)
        body += bytes([255, 255, 255, 255])
        if name == 'digits': body += struct.pack('<H', 4)
        body += b'\0' + text.encode('utf8') + b'\0'
        tags += tag(37, body)
        tags += tag(26, bytes([38]) + struct.pack('<HH', identifier, identifier) + matrix(60*20, y*20) + name.encode() + b'\0')
        for method in ['setTextFormat', 'setNewTextFormat']:
            formats += push(0x1e2832)+push(32)+push('_sans')+push(3)+push('TextFormat')+action(0x40)
            formats += push(1)+push('_root')+action(0x1c)+push(name)+action(0x4e)+push(method)+action(0x52)+action(0x17)
    # Set restrict on the actual AVM1 TextField instance.
    script = formats + push('_root') + action(0x1c) + push('digits') + action(0x4e)
    script += push('restrict') + push('0-9') + action(0x4f)
    script += push('_root.normal')+push(1)+push('Selection')+action(0x1c)+push('setFocus')+action(0x52)+action(0x17)
    script += action(0x07) + b'\0'
    tags += tag(12, script)
    write_movie(output / 'Keyboard-AVM1.swf', tags)


def playerglobal_import_path(build_root=None):
    """Select the newest completed current-format ARM64 core globals build.

    The raw import ABC precedes native-table rewriting in v0.7.1. Never
    extract the old playerglobal.swf: a leftover old release build would
    otherwise silently compile new fixtures against the pre-upgrade API.
    """
    build_root = Path(build_root) if build_root is not None else ROOT / 'build/cargo/aarch64-unknown-linux-gnu'
    candidates = []
    for profile in ('debug', 'release'):
        for abc in (build_root / profile / 'build').glob('ruffle_core-*/out/playerglobal_import.abc'):
            movie = abc.with_name('playerglobal_avm2.swf')
            if (movie.is_file() and abc.stat().st_size > 0
                    and movie.stat().st_mtime_ns >= abc.stat().st_mtime_ns):
                candidates.append(abc)
    if not candidates:
        raise RuntimeError('No completed current playerglobal_import.abc / playerglobal_avm2.swf pair; '
                           'run build_player.py check or build with the current source first')
    return max(candidates, key=lambda path: (path.stat().st_mtime_ns, path.as_posix()))


def playerglobal_abc():
    path = playerglobal_import_path()
    print('Using ASC import ' + str(path), file=sys.stderr)
    return path.read_bytes()


def avm2(output):
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    source = output / 'KeyboardProbe.as'
    source.write_text('''package {
import flash.display.MovieClip;
import flash.text.TextField;
import flash.text.TextFormat;
import flash.events.Event;
import flash.events.TextEvent;
public class KeyboardProbe extends MovieClip {
    public function KeyboardProbe() {
        var normal:TextField=make("normal", "seed", 110);
        normal.addEventListener(Event.CHANGE,function(e:Event):void {trace("KEYBOARD_NORMAL_TEXT:"+normal.text);});
        make("password", "secret", 210).displayAsPassword=true;
        var digits:TextField=make("digits", "12", 310);
        digits.maxChars=4; digits.restrict="0-9";
        make("multi", "line", 410).multiline=true;
        make("readonly", "Read only", 510).type="dynamic";
        var blocked:TextField=make("blocked", "keep", 610);
        blocked.addEventListener(TextEvent.TEXT_INPUT, function(e:TextEvent):void {e.preventDefault();trace("KEYBOARD_TEXT_PREVENTED");});
        if (stage) stage.focus=normal;
        else addEventListener(Event.ADDED_TO_STAGE,function(e:Event):void {stage.focus=normal;});
        stop();
    }
    private function make(label:String, value:String, y:int):TextField {
        var f:TextField=new TextField(); f.name=label; f.type="input";
        f.x=60; f.y=y; f.width=840; f.height=60; f.border=true;
        f.defaultTextFormat=new TextFormat("_sans",32,0xffffff);f.text=value;
        // Event traces contain no user text, including password fields.
        f.addEventListener(Event.CHANGE,function(e:Event):void {trace("KEYBOARD_CHANGE:"+e.target.name);});
        addChild(f);return f;
    }
}}
''', encoding='utf8')
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text()) if settings_path.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    subprocess.run([str(java), '-classpath', str(ROOT / 'player/tools/asc/asc.jar'),
                    'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
                    '-outdir', str(output), str(source)], check=True)
    abc = source.with_suffix('.abc').read_bytes()
    content = tag(82, struct.pack('<I', 0)+b'keyboard\0'+abc)
    content += tag(76, struct.pack('<HH', 1, 0)+b'KeyboardProbe\0')
    write_movie(output / 'Keyboard-AVM2.swf', content, avm2=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    avm1(args.output); avm2(args.output)
    print('Original AVM1 / AVM2 keyboard fixtures generated in', args.output)
