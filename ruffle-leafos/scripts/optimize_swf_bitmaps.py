"""Conservatively crop zero borders and deduplicate private lossless SWF bitmaps.

Uses only the Python standard library. ActionScript, geometry, timing, audio,
exported bitmaps and unmodified tags remain byte-for-byte unchanged. See the
project implementation documentation for the supported subset and limitations.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import zlib


class Bits:
    def __init__(self, data: bytes, byte: int = 0):
        self.data, self.pos = data, byte * 8

    def read(self, count: int, signed: bool = False) -> int:
        if self.pos + count > len(self.data) * 8:
            raise ValueError('Truncated SWF bit field')
        value = 0
        for _ in range(count):
            value = (value << 1) | ((self.data[self.pos // 8] >> (7 - self.pos % 8)) & 1)
            self.pos += 1
        if signed and count and value & (1 << (count - 1)):
            value -= 1 << count
        return value

    def align(self):
        self.pos = (self.pos + 7) // 8 * 8

    def byte(self) -> int:
        if self.pos % 8:
            raise ValueError('Unaligned SWF byte field')
        return self.pos // 8

    def skip(self, count: int):
        self.pos += count * 8
        if self.pos > len(self.data) * 8:
            raise ValueError('Truncated SWF byte field')


def rectangle(bits: Bits):
    count = bits.read(5)
    result = [bits.read(count, True) for _ in range(4)]
    bits.align()
    return result


def matrix(bits: Bits) -> tuple[int, ...]:
    a = d = 65536
    b = c = 0
    if bits.read(1):
        count = bits.read(5)
        a, d = bits.read(count, True), bits.read(count, True)
    if bits.read(1):
        count = bits.read(5)
        b, c = bits.read(count, True), bits.read(count, True)
    count = bits.read(5)
    tx, ty = bits.read(count, True), bits.read(count, True)
    bits.align()
    return a, b, c, d, tx, ty


def encode_matrix(value: tuple[int, ...]) -> bytes:
    output = []

    def write(number: int, count: int):
        valid = -(1 << (count - 1)) <= number < (1 << count) if count else number == 0
        if not valid:
            raise ValueError('SWF matrix field overflow')
        number &= (1 << count) - 1
        output.extend((number >> bit) & 1 for bit in range(count - 1, -1, -1))

    def width(*numbers):
        return max((n.bit_length() + 1 if n >= 0 else (~n).bit_length() + 1) for n in numbers)

    a, b, c, d, tx, ty = value
    scale = (a, d) != (65536, 65536)
    write(int(scale), 1)
    if scale:
        count = width(a, d)
        if count > 31:
            raise ValueError('SWF matrix scale too wide')
        write(count, 5)
        write(a, count)
        write(d, count)
    rotate = (b, c) != (0, 0)
    write(int(rotate), 1)
    if rotate:
        count = width(b, c)
        if count > 31:
            raise ValueError('SWF matrix skew too wide')
        write(count, 5)
        write(b, count)
        write(c, count)
    count = width(tx, ty)
    if count > 31:
        raise ValueError('SWF matrix translation too wide')
    write(count, 5)
    write(tx, count)
    write(ty, count)
    output.extend([0] * (-len(output) % 8))
    return bytes(sum(output[i + bit] << (7 - bit) for bit in range(8))
                 for i in range(0, len(output), 8))


@dataclass
class Fill:
    bitmap: int
    kind: int
    id_at: int
    start: int
    end: int
    transform: tuple[int, ...]


def shape_fills(data: bytes, version: int) -> list[Fill]:
    bits = Bits(data, 2)
    rectangle(bits)
    if version == 4:
        rectangle(bits)
        bits.skip(1)
    fills = []

    def fill():
        kind = bits.read(8)
        if kind == 0:
            bits.skip(4 if version >= 3 else 3)
        elif kind in (0x10, 0x12, 0x13):
            matrix(bits)
            count = bits.read(8) & 15
            bits.skip(count * (5 if version >= 3 else 4))
            if kind == 0x13:
                bits.skip(2)
        elif kind in (0x40, 0x41, 0x42, 0x43):
            id_at = bits.byte()
            ident = struct.unpack_from('<H', data, id_at)[0]
            bits.skip(2)
            start = bits.byte()
            transform = matrix(bits)
            fills.append(Fill(ident, kind, id_at, start, bits.byte(), transform))
        else:
            raise ValueError(f'Unsupported shape fill {kind:#x}')

    def styles():
        count = bits.read(8)
        if count == 255 and version >= 2:
            count = struct.unpack_from('<H', data, bits.byte())[0]
            bits.skip(2)
        for _ in range(count):
            fill()
        count = bits.read(8)
        if count == 255 and version >= 2:
            count = struct.unpack_from('<H', data, bits.byte())[0]
            bits.skip(2)
        for _ in range(count):
            bits.skip(2)
            if version < 4:
                bits.skip(4 if version >= 3 else 3)
            else:
                flags = struct.unpack_from('<H', data, bits.byte())[0]
                bits.skip(2)
                if flags & 0x30 == 0x20:
                    bits.skip(2)
                if flags & 8:
                    fill()
                else:
                    bits.skip(4)
        return bits.read(4), bits.read(4)

    fill_bits, line_bits = styles()
    while True:
        if bits.read(1):
            straight = bits.read(1)
            count = bits.read(4) + 2
            if straight:
                general = bits.read(1)
                if general:
                    bits.read(count * 2)
                else:
                    bits.read(1)
                    bits.read(count)
            else:
                bits.read(count * 4)
        else:
            flags = bits.read(5)
            if not flags:
                break
            if flags & 1:
                bits.read(bits.read(5) * 2)
            if flags & 2:
                bits.read(fill_bits)
            if flags & 4:
                bits.read(fill_bits)
            if flags & 8:
                bits.read(line_bits)
            if flags & 16:
                bits.align()
                fill_bits, line_bits = styles()
    bits.align()
    if bits.byte() != len(data):
        raise ValueError('Unexpected bytes after SWF shape records')
    return fills


@dataclass
class Tag:
    code: int
    data: bytes
    raw: bytes
    children: list[Tag] | None = None


def tags(data: bytes) -> list[Tag]:
    result, offset = [], 0
    while offset < len(data):
        start = offset
        header = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        code, length = header >> 6, header & 63
        if length == 63:
            length = struct.unpack_from('<I', data, offset)[0]
            offset += 4
        body = data[offset:offset + length]
        offset += length
        if len(body) != length:
            raise ValueError('Truncated SWF tag')
        result.append(Tag(code, body, data[start:offset], tags(body[4:]) if code == 39 else None))
        if code == 0:
            if offset != len(data):
                raise ValueError('Trailing data after End tag')
            break
    if not result or result[-1].code != 0:
        raise ValueError('Missing SWF End tag')
    return result


def walk(items):
    for tag in items:
        yield tag
        if tag.children is not None:
            yield from walk(tag.children)


def pack(code: int, body: bytes) -> bytes:
    return (struct.pack('<H', code << 6 | len(body)) if len(body) < 63 else
            struct.pack('<HI', code << 6 | 63, len(body))) + body


def load(raw: bytes):
    if raw[:3] not in (b'CWS', b'FWS'):
        raise ValueError('Only zlib-compressed CWS and uncompressed FWS are supported')
    body = zlib.decompress(raw[8:]) if raw[:3] == b'CWS' else raw[8:]
    if len(body) + 8 != struct.unpack_from('<I', raw, 4)[0]:
        raise ValueError('SWF declared length mismatch')
    bits = Bits(body)
    rectangle(bits)
    header_size = bits.byte() + 4
    return body[:header_size], tags(body[header_size:])


def protected_bitmaps(items, bitmap_ids):
    protected = set()
    for tag in items:
        data = tag.data
        if tag.code in (56, 76):
            count = struct.unpack_from('<H', data)[0]
            pos = 2
            for _ in range(count):
                protected.add(struct.unpack_from('<H', data, pos)[0])
                pos = data.index(b'\0', pos + 2) + 1
        elif tag.code in (4, 5):
            protected.add(struct.unpack_from('<H', data)[0])
        elif tag.code in (26, 70, 94) and data[0] & 2:
            pos = 3 if tag.code == 26 else 4
            if tag.code != 26 and data[1] & (8 | 16):
                pos = data.index(b'\0', pos) + 1
            protected.add(struct.unpack_from('<H', data, pos)[0])
        elif tag.code in (7, 34, 46, 84):
            # Conservative: these may refer to bitmap characters. False positives
            # merely skip optimisation; a reference cannot be missed this way.
            protected.update(struct.unpack_from('<H', data, pos)[0]
                             for pos in range(len(data) - 1))
    return protected & bitmap_ids


def bitmap(data: bytes):
    ident, fmt, width, height = struct.unpack_from('<HBHH', data)
    prefix = 8 if fmt == 3 else 7
    if fmt not in (3, 5) or not width or not height:
        raise ValueError(f'Unsupported lossless bitmap {ident}')
    decoded = zlib.decompress(data[prefix:])
    palette_size = (data[7] + 1) * 4 if fmt == 3 else 0
    stride = (width + 3) & ~3 if fmt == 3 else width * 4
    if len(decoded) != palette_size + stride * height:
        raise ValueError(f'Unexpected decoded bitmap length: {ident}')
    if fmt == 3:
        palette = decoded[:palette_size]
        lookup = bytes(255 if i * 4 < len(palette) and any(palette[i * 4:i * 4 + 4])
                       else 0 for i in range(256))
        content = decoded[palette_size:].translate(lookup)
        unit = 1
    else:
        content, unit = decoded, 4
    left, top, right, bottom = width, height, 0, 0
    for y in range(height):
        row = content[y * stride:y * stride + width * unit]
        stripped = row.strip(b'\0')
        if stripped:
            left = min(left, (len(row) - len(row.lstrip(b'\0'))) // unit)
            right = max(right, (len(row.rstrip(b'\0')) + unit - 1) // unit)
            top = min(top, y)
            bottom = y + 1
    if not right:  # Keep fully empty images at their original dimensions.
        bounds = (0, 0, width, height)
    else:
        bounds = (max(0, left - 1), max(0, top - 1), min(width, right + 1), min(height, bottom + 1))
    return ident, fmt, width, height, decoded, palette_size, stride, bounds


def optimize(raw: bytes, *, crop: bool = True, deduplicate: bool = True):
    header, roots = load(raw)
    items = list(walk(roots))
    if any(tag.code in (72, 82) or tag.code == 69 and struct.unpack_from('<I', tag.data)[0] & 8
           for tag in items):
        raise ValueError('This conservative optimizer supports AVM1 movies only')
    bitmap_definitions = [tag for tag in items if tag.code == 36]
    bitmap_tags = {struct.unpack_from('<H', tag.data)[0]: tag for tag in bitmap_definitions}
    if len(bitmap_tags) != len(bitmap_definitions):
        raise ValueError('Duplicate bitmap character IDs')
    protected = protected_bitmaps(items, set(bitmap_tags))
    fills_by_id = defaultdict(list)
    fills_by_tag = {}
    for tag in items:
        if tag.code in (2, 22, 32, 83):
            fills = shape_fills(tag.data, {2: 1, 22: 2, 32: 3, 83: 4}[tag.code])
            fills_by_tag[id(tag)] = fills
            for fill in fills:
                fills_by_id[fill.bitmap].append(fill)
    updates, offsets, aliases = {}, {}, {}
    seen, changes = {}, []
    before_pixels = after_pixels = 0
    skipped = Counter()
    for ident, tag in bitmap_tags.items():
        _, fmt, width, height, decoded, palette_size, stride, bounds = bitmap(tag.data)
        before_pixels += width * height
        refs = fills_by_id[ident]
        reason = None
        if ident in protected:
            reason = 'exported_or_non_shape_reference'
        elif not refs:
            reason = 'no_shape_reference'
        elif any(fill.kind not in (0x41, 0x43) for fill in refs):
            reason = 'repeating_fill'
        x, y, right, bottom = bounds if crop and reason is None else (0, 0, width, height)
        if any((fill.transform[0] * x + fill.transform[2] * y) % 65536 or
               (fill.transform[1] * x + fill.transform[3] * y) % 65536 for fill in refs):
            reason = 'fractional_twip_translation'
            x, y, right, bottom = 0, 0, width, height
        if reason:
            skipped[reason] += 1
        new_width, new_height = right - x, bottom - y
        if (new_width, new_height) != (width, height):
            unit = 1 if fmt == 3 else 4
            new_stride = (new_width + 3) & ~3 if fmt == 3 else new_width * 4
            rows = [decoded[palette_size + row * stride + x * unit:
                            palette_size + row * stride + right * unit] +
                    b'\0' * (new_stride - new_width * unit) for row in range(y, bottom)]
            decoded = decoded[:palette_size] + b''.join(rows)
            data = struct.pack('<HBHH', ident, fmt, new_width, new_height)
            if fmt == 3:
                data += tag.data[7:8]
            data += zlib.compress(decoded, 6)
            updates[id(tag)] = data
            offsets[ident] = (x, y)
            changes.append({'id': ident, 'before': [width, height], 'after': [new_width, new_height],
                            'origin': [x, y]})
        # Deduplicate only private assets with references we can rewrite fully.
        key = hashlib.sha256(struct.pack('<BHH', fmt, new_width, new_height) +
                             (tag.data[7:8] if fmt == 3 else b'') + decoded).digest()
        if deduplicate and ident not in protected and refs:
            if key in seen:
                aliases[ident] = seen[key]
            else:
                seen[key] = ident
        if ident not in aliases:
            after_pixels += new_width * new_height

    rewritten_fills = 0
    for tag in items:
        edits = []
        for fill in fills_by_tag.get(id(tag), []):
            if fill.bitmap in aliases:
                edits.append((fill.id_at, fill.id_at + 2, struct.pack('<H', aliases[fill.bitmap])))
            if fill.bitmap in offsets:
                a, b, c, d, tx, ty = fill.transform
                x, y = offsets[fill.bitmap]
                new_matrix = (a, b, c, d, tx + (a*x + c*y)//65536, ty + (b*x + d*y)//65536)
                edits.append((fill.start, fill.end, encode_matrix(new_matrix)))
            if fill.bitmap in aliases or fill.bitmap in offsets:
                rewritten_fills += 1
        if edits:
            body = tag.data
            for start, end, replacement in sorted(edits, reverse=True):
                body = body[:start] + replacement + body[end:]
            # Validate variable-length matrix edits before writing the file.
            shape_fills(body, {2: 1, 22: 2, 32: 3, 83: 4}[tag.code])
            updates[id(tag)] = body

    def emit(sequence):
        result = []
        for tag in sequence:
            if tag.code == 36 and struct.unpack_from('<H', tag.data)[0] in aliases:
                continue
            body = updates.get(id(tag), tag.data)
            if tag.children is not None:
                body = tag.data[:4] + emit(tag.children)
            result.append(tag.raw if body == tag.data else pack(tag.code, body))
        return b''.join(result)

    body = header + emit(roots)
    output = b'CWS' + raw[3:4] + struct.pack('<I', len(body) + 8) + zlib.compress(body, 6)
    load(output)
    report = {'input_sha256': hashlib.sha256(raw).hexdigest(),
              'output_sha256': hashlib.sha256(output).hexdigest(),
              'input_bytes': len(raw), 'output_bytes': len(output),
              'lossless_bitmap_count_before': len(bitmap_tags),
              'lossless_bitmap_count_after': len(bitmap_tags) - len(aliases),
              'rgba_equivalent_mib_before': before_pixels * 4 / 1048576,
              'rgba_equivalent_mib_after': after_pixels * 4 / 1048576,
              'cropped': len(changes), 'deduplicated': len(aliases),
              'rewritten_fills': rewritten_fills, 'skipped_crop': dict(skipped),
              'crop_changes': changes, 'aliases': aliases,
              'protected_bitmap_ids': sorted(protected)}
    return output, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--no-crop', action='store_true')
    parser.add_argument('--no-deduplicate', action='store_true')
    args = parser.parse_args()
    if len({args.input.resolve(), args.output.resolve(), args.report.resolve()}) != 3 or args.output.exists() or args.report.exists():
        parser.error('Output/report must be new files, separate from the original SWF')
    output, report = optimize(args.input.read_bytes(), crop=not args.no_crop,
                              deduplicate=not args.no_deduplicate)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ('crop_changes', 'aliases', 'protected_bitmap_ids')}, indent=2))


if __name__ == '__main__':
    main()
