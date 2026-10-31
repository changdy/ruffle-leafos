"""Check standalone repacking boundaries and byte-preserving embedded assets."""
import io
import struct
import unittest
import zipfile
import zlib

from optimize_swf_bitmaps import load, pack
from package_bvn_single_swf import (
    AFTER, BEFORE, DISABLED_LOADER, INSTALL_LOADER, asset_zip, embed,
)


def movie(loader=DISABLED_LOADER, archive=None):
    if archive is None:
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w'):
            pass
        archive = buffer.getvalue()
    # Minimal zero-size RECT, 30 FPS, single root frame.
    header = b'\x08\x00\x00\x1e\x01\x00'
    body = header + b''.join((
        pack(9, b'\x12\x34\x56'),
        pack(82, b'\0' * 4 + b'test\0' + loader),
        pack(87, struct.pack('<HI', 222, 0) + archive),
        pack(1, b''), pack(0, b''),
    ))
    return b'FWS\x13' + struct.pack('<I', len(body) + 8) + body


class SingleSwfTests(unittest.TestCase):
    def test_zip_is_deterministic_and_preserves_paths_bytes_and_fzip_adler(self):
        assets = {'assets/map/test.swf': b'CWS' + b'abc' * 100, 'assets/config.xml': b'<test/>'}
        archive = asset_zip(assets)
        self.assertEqual(archive, asset_zip(dict(reversed(list(assets.items())))))
        with zipfile.ZipFile(io.BytesIO(archive)) as check:
            self.assertIsNone(check.testzip())
            self.assertEqual(check.namelist(), sorted(assets))
            for name, data in assets.items():
                self.assertEqual(check.read(name), data)
                self.assertEqual(check.getinfo(name).extra, struct.pack('<HHI', 0xDADA, 4, zlib.adler32(data)))

    def test_restores_only_the_loader_and_embeds_exact_assets(self):
        original = movie()
        archive = asset_zip({'assets/test.xml': b'<test/>'})
        candidate, changed = embed(original, archive)
        old_header, old_tags = load(original)
        header, tags = load(candidate)
        self.assertEqual(header, old_header)
        self.assertEqual(changed, [1, 2])
        self.assertEqual(tags[1].data, old_tags[1].data.replace(DISABLED_LOADER, BEFORE + INSTALL_LOADER + AFTER))
        self.assertEqual(tags[2].data[6:], archive)
        for index in (0, 3, 4):
            self.assertEqual(tags[index].raw, old_tags[index].raw)

    def test_missing_or_ambiguous_loader_fails(self):
        for instructions in (b'unknown', DISABLED_LOADER * 2):
            with self.subTest(instructions=instructions):
                with self.assertRaises(ValueError):
                    embed(movie(loader=instructions), asset_zip({}))

    def test_refuses_to_replace_existing_embedded_resources(self):
        with self.assertRaisesRegex(ValueError, 'already has embedded assets'):
            embed(movie(archive=asset_zip({'assets/existing.xml': b'x'})), asset_zip({}))


if __name__ == '__main__':
    unittest.main()
