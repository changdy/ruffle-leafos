"""Build an experimental standalone BVN 3.3 SWF from the bundled directory.

Restores ZipAssetLoader, embeds the current 398 optimized assets, and spreads
startup effect caching across timers. Requires the configured JDK and a completed
player build for playerglobal. Does not change the manifest or release input.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import struct
import zipfile
import zlib

from bvn_single_boot import LOAD_BACK, compile_boot, patch_startup
from optimize_swf_bitmaps import load, pack


ROOT = Path(__file__).resolve().parents[1]
GAME = '死神VS火影3.3-定制版'
MAIN_SHA256 = '294e5e3c60e24fe01475c1fbb4dfc1f9aad1e23e4fbd62dd9849308f2fb020c0'
# Original bleachvsnaruto.initlize instructions, removed by the offline split.
INSTALL_LOADER = bytes.fromhex('5d0d4a0d005e0c2b610c60296622600c4f2a01')
BEFORE = bytes.fromhex('602724016128')
AFTER = bytes.fromhex('5d124f120047')
DISABLED_LOADER = BEFORE + b'\x02' * len(INSTALL_LOADER) + AFTER


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_assets(games: Path) -> tuple[bytes, dict[str, bytes]]:
    manifest = json.loads((games / 'manifest.json').read_text(encoding='utf8'))
    entry = next(g for g in manifest['games'] if g['name'] == GAME)
    main = (games / 'flash' / entry['entrypoint']).read_bytes()
    if entry['main_sha256'] != MAIN_SHA256 or sha(main) != MAIN_SHA256:
        raise ValueError('Unsupported BVN main SWF; audit loader instructions before updating the pin')
    prefix = GAME + '/assets/'
    expected = {name[len(GAME) + 1:]: digest for name, digest in manifest['files_sha256'].items()
                if name.startswith(prefix)}
    folder = games / 'flash' / GAME
    actual = {p.relative_to(folder).as_posix() for p in (folder / 'assets').rglob('*') if p.is_file()}
    if len(expected) != 398 or actual != expected.keys():
        raise ValueError('Expected the complete set of 398 bundled BVN assets')
    assets = {}
    for name, digest in sorted(expected.items()):
        rel = PurePosixPath(name)
        if rel.is_absolute() or '..' in rel.parts or '\\' in name:
            raise ValueError('Unsafe asset path: ' + name)
        data = folder.joinpath(*rel.parts).read_bytes()
        if sha(data) != digest:
            raise ValueError('Asset checksum mismatch: ' + name)
        assets[name] = data
    return main, assets


def asset_zip(assets: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(assets.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 0
            # FZip's Flash Player fallback expects this optional Adler-32 field.
            entry.extra = struct.pack('<HHI', 0xDADA, 4, zlib.adler32(data))
            archive.writestr(entry, data, compresslevel=9)
    return buffer.getvalue()


def embed(main: bytes, archive: bytes, bootstrap: bytes | None = None) -> tuple[bytes, list[int]]:
    header, items = load(main)
    parts, changed = [], []
    loaders = binaries = startups = 0
    boot_payload = b'\0' * 4 + b'ruffle-single-boot\0' + (bootstrap or b'')
    for index, tag in enumerate(items):
        payload = tag.data
        if tag.code == 82 and DISABLED_LOADER in payload:
            if payload.count(DISABLED_LOADER) != 1:
                raise ValueError('Ambiguous disabled ZipAssetLoader instructions')
            payload = payload.replace(DISABLED_LOADER, BEFORE + INSTALL_LOADER + AFTER, 1)
            loaders += 1
        if bootstrap is not None and tag.code == 82 and LOAD_BACK in payload:
            parts.append(pack(82, boot_payload))
            payload = patch_startup(payload)
            startups += 1
        if tag.code == 87 and payload[:2] == struct.pack('<H', 222):
            if payload[2:6] != b'\0' * 4:
                raise ValueError('Unexpected DefineBinaryData reserved bytes')
            with zipfile.ZipFile(io.BytesIO(payload[6:])) as empty:
                if empty.infolist():
                    raise ValueError('Input already has embedded assets')
            payload = payload[:6] + archive
            binaries += 1
        if payload == tag.data:
            parts.append(tag.raw)
        else:
            parts.append(pack(tag.code, payload))
            changed.append(index)
    if loaders != 1 or binaries != 1 or startups != int(bootstrap is not None):
        raise ValueError('Expected the audited loader, startup methods and empty ZIP binary')
    body = header + b''.join(parts)
    # The embedded ZIP is already compressed; keep the outer SWF uncompressed.
    candidate = b'FWS' + main[3:4] + struct.pack('<I', len(body) + 8) + body
    check_header, check_items = load(candidate)
    if bootstrap is not None:
        if sum(t.code == 82 and t.data == boot_payload for t in check_items) != 1:
            raise ValueError('Missing or duplicate startup helper')
        check_items = [t for t in check_items if not (t.code == 82 and t.data == boot_payload)]
    if check_header != header or len(check_items) != len(items):
        raise ValueError('SWF structure changed unexpectedly')
    actual_changes = [i for i, (old, new) in enumerate(zip(items, check_items)) if old.raw != new.raw]
    if actual_changes != changed or len(changed) != 2:
        raise ValueError('Changes exceed the audited main ABC and embedded ZIP tags')
    return candidate, changed


def build(games: Path, output: Path, report: Path) -> dict:
    output, report = output.resolve(), report.resolve()
    if output == report or output.exists() or report.exists():
        raise ValueError('Output and report must be distinct, new files')
    # Generated experiments belong outside the versioned game input directory.
    if output.is_relative_to(games.resolve()) or report.is_relative_to(games.resolve()):
        raise ValueError('Write generated files outside games/')
    main, assets = read_assets(games)
    bootstrap = compile_boot()
    archive = asset_zip(assets)
    candidate, changed = embed(main, archive, bootstrap)
    # Verify the ZIP extracted from the completed SWF, not just the ZIP input.
    _, items = load(candidate)
    embedded = next(t.data[6:] for t in items if t.code == 87 and t.data[:2] == struct.pack('<H', 222))
    with zipfile.ZipFile(io.BytesIO(embedded)) as check:
        if check.testzip() is not None or check.namelist() != sorted(assets):
            raise ValueError('Embedded ZIP integrity or paths differ')
        for name, data in assets.items():
            if check.read(name) != data:
                raise ValueError('Embedded asset differs: ' + name)
    result = {
        'source_main_sha256': sha(main), 'output_sha256': sha(candidate),
        'output_bytes': len(candidate), 'embedded_zip_bytes': len(embedded),
        'asset_count': len(assets), 'asset_bytes': sum(map(len, assets.values())),
        'asset_sha256': {name: sha(data) for name, data in assets.items()},
        'changed_root_tag_indices': changed,
        'unchanged_other_tags_and_frame_header': True,
        'restored_loader_instruction_bytes': len(INSTALL_LOADER),
        'startup_helper_abc_sha256': sha(bootstrap),
        'changed_original_method_count': 3,
        'effect_caching': 'Original effect order and all cacheBitmapData calls; one effect per timer',
        'swf_compression': 'FWS; embedded ZIP uses DEFLATE',
        'device_validation': 'Not performed by this build script',
    }
    for path in (output, report):
        path.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as target:
        target.write(candidate)
    with report.open('x', encoding='utf8', newline='\n') as target:
        target.write(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = build(ROOT / 'games', args.output, args.report)
    print(json.dumps({k: v for k, v in result.items() if k != 'asset_sha256'}, indent=2))
