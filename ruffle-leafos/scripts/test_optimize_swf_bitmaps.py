"""Focused format/semantic regression tests; no third-party games required."""
import struct
import unittest
import zlib

import optimize_swf_bitmaps as opt


def bits(fields):
    stream = ''.join(format(number & ((1 << width) - 1), f'0{width}b')
                     for number, width in fields if width)
    stream += '0' * (-len(stream) % 8)
    return int(stream, 2).to_bytes(len(stream) // 8, 'big') if stream else b''


RECT = bits([(1, 5), (0, 1), (0, 1), (0, 1), (0, 1)])


def image(ident=1, fmt=5):
    # One coloured pixel at (4, 5), surrounded by zero RGBA in an 8x8 image.
    if fmt == 5:
        pixels = bytearray(8 * 8 * 4)
        pixels[(5 * 8 + 4) * 4:(5 * 8 + 4) * 4 + 4] = bytes([255, 20, 30, 40])
        prefix = b''
    else:
        pixels = bytearray(8 * 8)
        pixels[5 * 8 + 4] = 1
        pixels = bytes([0, 0, 0, 0, 20, 30, 40, 255]) + pixels
        prefix = b'\x01'
    return struct.pack('<HBHH', ident, fmt, 8, 8) + prefix + zlib.compress(pixels)


def shape(ident=2, bitmap_id=1, kind=0x41, transform=None, extra_styles=False):
    transform = transform or (20 * 65536, 0, 0, 20 * 65536, -100, 200)
    styles = b'\x01' + bytes([kind]) + struct.pack('<H', bitmap_id) + opt.encode_matrix(transform) + b'\x00\x10'
    if extra_styles:
        # An aligned NewStyles record exercises matrices inside edge records.
        records = bits([(0, 1), (16, 5)]) + styles + b'\x00'
    else:
        records = b'\x00'
    return struct.pack('<H', ident) + RECT + styles + records


def movie(entries):
    body = RECT + struct.pack('<HH', 34 * 256, 1) + b''.join(opt.pack(code, data) for code, data in entries) + opt.pack(0, b'')
    return b'CWS\x0c' + struct.pack('<I', len(body) + 8) + zlib.compress(body)


def flat(raw):
    return list(opt.walk(opt.load(raw)[1]))


class BitmapOptimizationTests(unittest.TestCase):
    def test_argb_and_palette_crop_keep_pixels_and_transform(self):
        for fmt in (3, 5):
            with self.subTest(fmt=fmt):
                original = movie([(36, image(fmt=fmt)), (2, shape())])
                output, report = opt.optimize(original)
                self.assertEqual(report['cropped'], 1)
                self.assertEqual(report['crop_changes'][0]['origin'], [3, 4])
                tags = flat(output)
                bitmap = opt.bitmap(next(t.data for t in tags if t.code == 36))
                self.assertEqual(bitmap[2:4], (3, 3))
                fill = opt.shape_fills(next(t.data for t in tags if t.code == 2), 1)[0]
                self.assertEqual(fill.transform[-2:], (-40, 280))
                decoded, palette_size, stride = bitmap[4:7]
                if fmt == 5:
                    self.assertEqual(decoded[stride + 4:stride + 8], bytes([255, 20, 30, 40]))
                else:
                    self.assertEqual(decoded[palette_size + stride + 1], 1)

    def test_duplicates_remap_both_fills_and_keep_script_and_audio(self):
        script, audio = b'\x96\x02\x00\x00\x00\x00', b'\x09\x00opaque-audio'
        original = movie([(36, image()), (36, image(3)), (2, shape()), (2, shape(4, 3)), (12, script), (14, audio)])
        output, report = opt.optimize(original)
        self.assertEqual(report['deduplicated'], 1)
        self.assertEqual([f.bitmap for t in flat(output) if t.code == 2 for f in opt.shape_fills(t.data, 1)], [1, 1])
        self.assertEqual([(t.code, t.data) for t in flat(output) if t.code in (12, 14)], [(12, script), (14, audio)])

    def test_exported_and_directly_placed_bitmaps_remain_unchanged(self):
        for guard in [(56, b'\x01\x00\x01\x00sprite\x00'), (26, b'\x02\x01\x00\x01\x00')]:
            with self.subTest(guard=guard[0]):
                original = movie([(36, image()), (2, shape()), guard])
                output, report = opt.optimize(original)
                self.assertEqual(report['cropped'], 0)
                self.assertEqual(report['deduplicated'], 0)
                self.assertEqual(next(t.data for t in flat(output) if t.code == 36), image())

    def test_repeating_and_fractional_translation_are_not_cropped(self):
        for body in [shape(kind=0x40), shape(transform=(65537, 0, 0, 65536, 0, 0))]:
            output, report = opt.optimize(movie([(36, image()), (2, body)]))
            self.assertEqual(report['cropped'], 0)
            self.assertEqual(next(t.data for t in flat(output) if t.code == 2), body)

    def test_new_style_and_nested_sprite_rewrite(self):
        child = opt.pack(2, shape(extra_styles=True)) + opt.pack(12, b'unchanged') + opt.pack(0, b'')
        original = movie([(36, image()), (39, b'\x03\x00\x01\x00' + child)])
        output, report = opt.optimize(original)
        self.assertEqual(report['rewritten_fills'], 2)
        fills = opt.shape_fills(next(t.data for t in flat(output) if t.code == 2), 1)
        self.assertEqual([f.transform[-2:] for f in fills], [(-40, 280), (-40, 280)])
        self.assertEqual(next(t.data for t in flat(output) if t.code == 12), b'unchanged')

    def test_invalid_length_and_avm2_fail_closed(self):
        raw = movie([(36, image())])
        with self.assertRaises(ValueError):
            opt.optimize(raw[:4] + b'\x00' * 4 + raw[8:])
        with self.assertRaises(ValueError):
            opt.optimize(movie([(69, struct.pack('<I', 8)), (36, image())]))

    def test_avm2_opt_in_preserves_abc_symbols_and_exported_bitmap(self):
        abc = struct.pack('<I', 0) + b'example\0opaque-abc-payload'
        symbols = b'\x02\x00\x00\x00Document\x00\x03\x00ExportedBitmap\x00'
        original = movie([(69, struct.pack('<I', 8)), (82, abc),
                          (36, image()), (36, image(3)),
                          (2, shape()), (2, shape(4, 3)), (76, symbols)])
        output, report = opt.optimize(original, avm2_private_bitmaps=True)
        tags = flat(output)
        self.assertTrue(report['avm2_private_bitmaps'])
        self.assertEqual(report['cropped'], 1)
        self.assertEqual(report['deduplicated'], 0)
        self.assertEqual([(t.code, t.data) for t in tags if t.code in (69, 76, 82)],
                         [(t.code, t.data) for t in flat(original) if t.code in (69, 76, 82)])
        self.assertEqual(next(t.data for t in tags if t.code == 36 and t.data[:2] == b'\x03\x00'), image(3))

    def test_avm2_introspection_is_rejected_even_with_opt_in(self):
        for token in (b'readGraphicsData', b'GraphicsBitmapFill'):
            raw = movie([(82, struct.pack('<I', 0) + b'\0' + token),
                         (36, image()), (2, shape())])
            with self.subTest(token=token), self.assertRaisesRegex(ValueError, 'introspection'):
                opt.optimize(raw, avm2_private_bitmaps=True)

    def test_avm2_private_duplicates_remap_without_changing_timeline(self):
        timeline = [(43, b'attack\x00'), (1, b'')]
        abc = struct.pack('<I', 0) + b'\0unmodified-bytecode'
        original = movie([(82, abc), (36, image()), (36, image(3)),
                          (2, shape()), (2, shape(4, 3)), *timeline])
        output, report = opt.optimize(original, avm2_private_bitmaps=True)
        self.assertEqual(report['deduplicated'], 1)
        self.assertEqual([(t.code, t.data) for t in flat(output) if t.code in (43, 1, 82)],
                         [(t.code, t.data) for t in flat(original) if t.code in (43, 1, 82)])
        self.assertEqual([f.bitmap for t in flat(output) if t.code == 2
                          for f in opt.shape_fills(t.data, 1)], [1, 1])


if __name__ == '__main__':
    unittest.main()
