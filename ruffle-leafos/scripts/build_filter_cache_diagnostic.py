"""Build an original AVM1 filter-resize diagnostic that stops on its last frame."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'adapter/scripts'))
from make_demo import rect, shape, tag
import optimize_swf_bitmaps as inspect_swf

STAGE, BASE, FIRST, LAST, FPS, FRAMES = 640, 128, 128, 500, 30, 48
BACKGROUND = (16, 24, 32)
BLOCK = (48, 160, 224)
GLOW = (255, 220, 64, 255)
BLUR = 16
SHAPE_ID, DEPTH = 1, 1


def frame_matrix(size):
    scale = size * 65536 // BASE
    at = (STAGE - size) * 10  # Center in twips, including half-pixel origins.
    return scale, 0, 0, scale, at, at


def make_movie():
    sizes = [FIRST + round((LAST - FIRST) * frame / (FRAMES - 1)) for frame in range(FRAMES)]
    body = rect(STAGE * 20, STAGE * 20) + struct.pack('<HH', FPS * 256, FRAMES)
    body += tag(69, struct.pack('<I', 0))  # AVM1, no networking flags.
    body += tag(9, bytes(BACKGROUND)) + shape(SHAPE_ID, BASE, BASE, BLOCK)
    expected = []
    for frame, size in enumerate(sizes):
        transform = inspect_swf.encode_matrix(frame_matrix(size))
        if frame == 0:
            # Place exactly one Graphic; attaching a filter implicitly caches it.
            flags = 0x0106  # HasFilterList | HasMatrix | HasCharacter.
            placement = struct.pack('<HHH', flags, DEPTH, SHAPE_ID) + transform
            placement += bytes((1, 2, *GLOW))  # One filter, type 2 = Glow.
            placement += struct.pack('<iiHB', BLUR << 16, BLUR << 16, 256, 0x21)
        else:
            # Modify the existing instance; keep its original filter and depth.
            placement = struct.pack('<HH', 0x0005, DEPTH) + transform
        body += tag(70, placement)
        if frame == FRAMES - 1:
            body += tag(12, b'\x07\x00')  # ActionStop, ActionEnd.
        body += tag(1)
        expected.append({
            'frame': frame + 1, 'source_pixels': [size, size],
            'source_rect_pixels': [(STAGE - size) / 2, (STAGE - size) / 2, size, size],
            'expected_cache_pixels_at_stage_scale_1': [size + 2 * BLUR, size + 2 * BLUR],
        })
    body += tag(0)
    movie = b'FWS\x08' + struct.pack('<I', len(body) + 8) + body
    return movie, expected


def validate(movie, expected):
    # Separate bit reader/tag parser validates the emitted structure and all
    # placement matrices; no renderer or GPU is needed for this readback.
    header, entries = inspect_swf.load(movie)
    bits = inspect_swf.Bits(header)
    assert inspect_swf.rectangle(bits) == [0, STAGE * 20, 0, STAGE * 20]
    assert struct.unpack('<HH', header[-4:]) == (FPS * 256, FRAMES)
    assert Counter(entry.code for entry in entries) == Counter({
        69: 1, 9: 1, 2: 1, 70: FRAMES, 1: FRAMES, 12: 1, 0: 1,
    })
    artwork = next(entry for entry in entries if entry.code == 2).data
    bits = inspect_swf.Bits(artwork, 2)
    assert inspect_swf.rectangle(bits) == [0, BASE * 20, 0, BASE * 20]
    assert artwork[bits.byte():bits.byte() + 6] == bytes((1, 0, *BLOCK, 0))
    assert inspect_swf.shape_fills(artwork, 1) == []  # Solid fill, no bitmaps.
    placements = [entry for entry in entries if entry.code == 70]
    for frame, (entry, target) in enumerate(zip(placements, expected)):
        flags, depth = struct.unpack_from('<HH', entry.data)
        assert depth == DEPTH
        assert flags == (0x0106 if frame == 0 else 0x0005)
        offset = 4
        if frame == 0:
            assert struct.unpack_from('<H', entry.data, offset)[0] == SHAPE_ID
            offset += 2
        bits = inspect_swf.Bits(entry.data, offset)
        matrix = inspect_swf.matrix(bits)
        size = target['source_pixels'][0]
        assert matrix == (size * 512, 0, 0, size * 512, (STAGE - size) * 10, (STAGE - size) * 10)
        offset = bits.byte()
        if frame == 0:
            assert entry.data[offset:offset + 2] == b'\x01\x02'
            assert struct.unpack_from('<4BiiHB', entry.data, offset + 2) == (*GLOW, BLUR << 16, BLUR << 16, 256, 0x21)
            offset += 17
        assert offset == len(entry.data)
    timeline = [entry.code for entry in entries[3:]]
    assert timeline == [70, 1] * (FRAMES - 1) + [70, 12, 1, 0]
    assert entries[-3].data == b'\x07\x00'
    assert len({tuple(frame['source_pixels']) for frame in expected}) == FRAMES
    assert expected[-1]['source_pixels'] == [LAST, LAST]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/diagnostics',
                        help='Destination for the SWF and expected-size JSON report')
    output = parser.parse_args().output_dir
    output.mkdir(parents=True, exist_ok=True)
    movie, expected = make_movie()
    validate(movie, expected)
    filename = 'filter-cache-resize-stop.swf'
    (output / filename).write_bytes(movie)
    report = {
        'fixture': filename, 'sha256': hashlib.sha256(movie).hexdigest(), 'bytes': len(movie),
        'swf_version': 8, 'avm': 1, 'fps': FPS, 'frames': FRAMES,
        'authored_duration_seconds': FRAMES / FPS, 'stage_pixels': [STAGE, STAGE],
        'display_object_instances': 1, 'shape_id': SHAPE_ID, 'depth': DEPTH,
        'background_rgb': BACKGROUND, 'block_rgb': BLOCK,
        'glow': {'rgba': GLOW, 'blur_x': BLUR, 'blur_y': BLUR, 'strength': 1, 'passes': 1,
                 'composite_source': True, 'inner': False, 'knockout': False},
        'bitmap_data_operations': 0, 'bitmap_definitions': 0, 'external_resources': False,
        'actions': 'Exactly one final-frame ActionStop followed by ActionEnd; no other script.',
        'timeline': 'Place one Graphic on frame 1; subsequent PlaceObject3 tags only move/modify its matrix. Never remove or replace it. Keep the first Glow filter. Stop after the 500px frame.',
        'expected_frames': expected,
        'final_frame': expected[-1],
        'expected_filter_rect_at_stage_scale_1': [54, 54, 532, 532],
        'size_scope': 'Source/cache dimensions are authored stage pixels at 1:1 stage scale. A scaled viewport also scales the Glow according to Ruffle stage-view rules; do not treat these as physical framebuffer measurements.',
        'validation': 'Independent SWF tag/bit parser checked header, original solid shape, all move-only matrices, one-instance placement, Glow fields and final Stop action.',
        'reproduce': [
            'Build with python scripts/build_filter_cache_diagnostic.py; --output-dir selects another artifact directory.',
            'Run the same SWF with old and candidate GLES players in isolated data directories and identical scaling/quality; wait at least 3 seconds for the final static frame.',
            'Capture the final blue square and gold Glow, including background margins, and compare old/candidate screenshots. Capture temporary-pool/current-GL payload after resizing and after several idle seconds.',
            'The 48 distinct cache sizes exercise filter scratch reuse/retention without per-frame new display instances, BitmapData allocation or external loading. Host validation does not establish device pixel parity.',
        ],
    }
    (output / 'filter-cache-resize-stop.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    print(json.dumps({key: report[key] for key in ('fixture', 'sha256', 'bytes', 'fps', 'frames', 'display_object_instances', 'final_frame')}, indent=2))


if __name__ == '__main__':
    main()
