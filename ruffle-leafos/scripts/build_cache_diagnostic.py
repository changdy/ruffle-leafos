"""Build an original deterministic static-bitmap cache diagnostic SWF."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'adapter/scripts'))
from make_demo import Bits, rect, matrix, signed_bits, tag
import optimize_swf_bitmaps as inspect_swf

WIDTH, HEIGHT, SIZE, FPS = 320, 240, 128, 30
X, Y = 96, 56
BACKGROUND = (16, 24, 32)
COLORS = ((224, 48, 48), (48, 208, 80), (48, 96, 224))


def bitmap_pixels(index):
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            color = COLORS[index]
            if (x // 16 + y // 16) % 2:
                color = tuple(channel * 3 // 4 for channel in color)
            # One, two or three white marks identify the current frame.
            if 8 <= y < 16 and any(8 + n * 16 <= x < 16 + n * 16 for n in range(index + 1)):
                color = (255, 255, 255)
            out.extend((255, *color))  # DefineBitsLossless2 uses ARGB.
    return bytes(out)


def bitmap_shape(identifier, bitmap_id):
    side = SIZE * 20
    records = Bits()
    records.put(1, 4)  # One bit per fill index.
    records.put(0, 4)  # No line styles.
    records.put(0, 1)
    records.put(0b00101, 5)  # MoveTo and FillStyle1.
    records.put(1, 5)
    records.put(0, 1)
    records.put(0, 1)
    records.put(1, 1)
    for dx, dy in ((side, 0), (0, side), (-side, 0), (0, -side)):
        count = max(2, signed_bits(dx, dy))
        records.put(1, 1)
        records.put(1, 1)
        records.put(count - 2, 4)
        records.put(1, 1)
        records.put(dx, count)
        records.put(dy, count)
    records.put(0, 6)
    fill = bytes((1, 0x41)) + struct.pack('<H', bitmap_id)
    fill += inspect_swf.encode_matrix((20 * 65536, 0, 0, 20 * 65536, 0, 0))
    body = struct.pack('<H', identifier) + rect(side, side) + fill
    body += b'\x00' + records.data()  # Empty line-style array.
    return tag(2, body)


def png(path, pixels):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)
        for x in range(WIDTH):
            color = BACKGROUND
            if X <= x < X + SIZE and Y <= y < Y + SIZE:
                at = ((y - Y) * SIZE + (x - X)) * 4
                color = pixels[at + 1:at + 4]
            rows.extend(color)
    data = b'\x89PNG\r\n\x1a\n'
    data += chunk(b'IHDR', struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/diagnostics',
                        help='Destination for the SWF, JSON report and three reference PNGs')
    output = parser.parse_args().output_dir
    output.mkdir(parents=True, exist_ok=True)
    body = rect(WIDTH * 20, HEIGHT * 20) + struct.pack('<HH', FPS * 256, 3)
    body += tag(69, struct.pack('<I', 0)) + tag(9, bytes(BACKGROUND))
    pixels = [bitmap_pixels(index) for index in range(3)]
    for index, data in enumerate(pixels):
        body += tag(36, struct.pack('<HBHH', index + 1, 5, SIZE, SIZE) + zlib.compress(data, 9))
        body += bitmap_shape(index + 101, index + 1)
    for index in range(3):
        body += tag(28, struct.pack('<H', 1))
        body += tag(26, b'\x06' + struct.pack('<HH', 1, index + 101) + matrix(X * 20, Y * 20))
        body += tag(1)
    body += tag(0)
    movie = b'FWS\x08' + struct.pack('<I', len(body) + 8) + body
    path = output / 'static-bitmap-cycle.swf'
    path.write_bytes(movie)

    # Read back through the repository's separate parser before device use.
    header, entries = inspect_swf.load(movie)
    codes = Counter(entry.code for entry in entries)
    assert struct.unpack('<HH', header[-4:]) == (FPS * 256, 3)
    assert codes == Counter({69: 1, 9: 1, 36: 3, 2: 3, 28: 3, 26: 3, 1: 3, 0: 1})
    assert [entry.code for entry in entries[-10:]] == [28, 26, 1] * 3 + [0]
    for index, entry in enumerate(entry for entry in entries if entry.code == 26):
        assert entry.data[:5] == b'\x06' + struct.pack('<HH', 1, index + 101)
        assert inspect_swf.matrix(inspect_swf.Bits(entry.data, 5)) == (65536, 0, 0, 65536, X * 20, Y * 20)
    assert all(entry.data == struct.pack('<H', 1) for entry in entries if entry.code == 28)
    for index, entry in enumerate(entry for entry in entries if entry.code == 36):
        decoded = inspect_swf.bitmap(entry.data)
        assert decoded[0:4] == (index + 1, 5, SIZE, SIZE)
        assert decoded[4] == pixels[index]
    for index, entry in enumerate(entry for entry in entries if entry.code == 2):
        fills = inspect_swf.shape_fills(entry.data, 1)
        assert len(fills) == 1 and fills[0].bitmap == index + 1
        assert fills[0].transform == (20 * 65536, 0, 0, 20 * 65536, 0, 0)
    for index, data in enumerate(pixels):
        png(output / f'expected-frame-{index + 1}.png', data)
    report = {
        'fixture': path.name, 'sha256': hashlib.sha256(movie).hexdigest(),
        'bytes': len(movie), 'fps': FPS, 'frames': 3, 'stage': [WIDTH, HEIGHT],
        'bitmap_count': 3, 'bitmap_size': [SIZE, SIZE],
        'total_rgba_bytes': sum(map(len, pixels)), 'scripts': False, 'external_resources': False,
        'timeline': 'Each frame RemoveObject2 depth 1, PlaceObject2 shape 101/102/103 depth 1, ShowFrame; loops naturally.',
        'visual': 'Red, green, blue checkerboards at [96,56,128,128], with one/two/three white marks at the top.',
        'validation': 'Independent repository SWF parser accepted all tags, bitmap payloads and shape fills/matrices.',
        'expected_steady_state': 'At 30 FPS GPU0 recreates a shape/texture nearly each draw; GPU8 MiB should retain all three after warmup. Enable CPU0 separately to expose demand re-decodes.',
        'render_acceptance': 'Pending device run; PNGs describe authored pixels, not screenshots.',
    }
    (output / 'static-bitmap-cycle.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
