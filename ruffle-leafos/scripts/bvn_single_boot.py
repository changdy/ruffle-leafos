"""Audited startup edits for the pinned bundled BVN 3.3 main ABC."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile

from make_keyboard_fixtures import ROOT, playerglobal_abc


LOAD_BACK = bytes.fromhex('b001020100001860f50466224f360060f604201307000060f60464410029470000')
CACHE_EFFECTS = bytes.fromhex('c1050204000122d0302400d760960dd6100d000009d2d32380900cd5d14fa20d0032020311ecffff470000')


def u30(value: int) -> bytes:
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


class Reader:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 4

    def u30(self):
        result = 0
        for shift in range(0, 35, 7):
            value = self.data[self.pos]
            self.pos += 1
            result |= (value & 127) << shift
            if not value & 128:
                return result
        raise ValueError('Invalid ABC integer')

    def byte(self):
        value = self.data[self.pos]
        self.pos += 1
        return value


def append_boot_class(abc: bytes) -> tuple[bytes, int]:
    r = Reader(abc)
    for _ in range(2):  # int and uint pools
        for _ in range(max(0, r.u30() - 1)):
            r.u30()
    doubles = r.u30()
    r.pos += max(0, doubles - 1) * 8
    string_at = r.pos
    strings = r.u30()
    string_records = r.pos
    for _ in range(max(0, strings - 1)):
        size = r.u30()
        r.pos += size
    string_end = r.pos
    namespaces = []
    for _ in range(max(0, r.u30() - 1)):
        namespaces.append((r.byte(), r.u30()))
    # PackageNamespace("") is index 1 in this pinned ABC.
    if not namespaces or namespaces[0] != (0x16, 2):
        raise ValueError('Unexpected BVN public namespace')
    for _ in range(max(0, r.u30() - 1)):
        for _ in range(r.u30()):
            r.u30()
    name_at = r.pos
    names = r.u30()
    name_records = r.pos
    for _ in range(max(0, names - 1)):
        kind = r.byte()
        if kind in (7, 13, 9, 14):
            r.u30(); r.u30()
        elif kind in (15, 16, 27, 28):
            r.u30()
        elif kind in (17, 18):
            pass
        elif kind == 29:
            r.u30()
            for _ in range(r.u30()):
                r.u30()
        else:
            raise ValueError('Unknown ABC multiname')
    if strings != 4851 or names != 4093:
        raise ValueError('Unsupported BVN ABC pool sizes')
    name = b'RuffleSingleSwfBoot'
    # Append entries, preserving all existing string and multiname indices.
    result = (abc[:string_at] + u30(strings + 1) + abc[string_records:string_end]
              + u30(len(name)) + name + abc[string_end:name_at] + u30(names + 1)
              + abc[name_records:r.pos] + b'\x07\x01' + u30(strings) + abc[r.pos:])
    return result, names


def patch_startup(payload: bytes) -> bytes:
    if payload.count(LOAD_BACK) != 1 or payload.count(CACHE_EFFECTS) != 1:
        raise ValueError('Expected the two audited BVN startup method bodies')
    start = payload.index(b'\0', 4) + 1
    abc, helper = append_boot_class(payload[start:])
    # MainGame.loadGameBack: register the original callback before initlize.
    # QName 141 = public back; 629 = EffectModel; 630 = captured initBack;
    # 34 = public I; 54 = public initlize. No method signature changes.
    callback = (b'\x60' + u30(helper) + b'\x60' + u30(630) + b'\x61' + u30(141)
                + b'\x60' + u30(629) + b'\x66' + u30(34) + b'\x4f' + u30(54) + b'\x00\x47')
    new_back = u30(176) + b'\x02\x01\x00\x00' + u30(len(callback)) + callback + b'\x00\x00'
    # EffectModel.cacheEffects: pass the unchanged effect map to the helper.
    # QName 1686 = private _effect; 707 = public load.
    cache = b'\xd0\x30\x60' + u30(helper) + b'\x60' + u30(1686) + b'\x4f' + u30(707) + b'\x01\x47'
    new_cache = u30(705) + b'\x02\x04\x00\x01' + u30(len(cache)) + cache + b'\x00\x00'
    abc = abc.replace(LOAD_BACK, new_back, 1).replace(CACHE_EFFECTS, new_cache, 1)
    return payload[:start] + abc


def compile_boot() -> bytes:
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
    java = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1')) / 'bin/java.exe'
    with tempfile.TemporaryDirectory(prefix='bvn-single-boot-') as temporary:
        folder = Path(temporary)
        source = folder / 'RuffleSingleSwfBoot.as'
        shutil.copyfile(Path(__file__).parent / 'resources/RuffleSingleSwfBoot.as', source)
        (folder / 'playerglobal.abc').write_bytes(playerglobal_abc())
        result = subprocess.run([
            str(java), '-classpath', str(ROOT / 'player/core/build_playerglobal/asc.jar'),
            'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(folder / 'playerglobal.abc'),
            '-outdir', str(folder), str(source),
        ], capture_output=True, timeout=60)
        compiled = source.with_suffix('.abc')
        if result.returncode or not compiled.exists():
            raise ValueError('Bootstrap compilation failed: ' + (result.stdout + result.stderr).decode('utf8', 'replace'))
        return compiled.read_bytes()
