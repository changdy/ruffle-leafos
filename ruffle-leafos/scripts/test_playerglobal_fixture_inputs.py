"""Keep fixture compilers on current, completed ASC import outputs."""
from pathlib import Path
import os
import tempfile
import unittest

from make_keyboard_fixtures import playerglobal_import_path


class PlayerglobalFixtureInputsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def output(self, profile, suffix, abc, modified, complete=True):
        out = self.root / profile / 'build' / ('ruffle_core-' + suffix) / 'out'
        out.mkdir(parents=True)
        path = out / 'playerglobal_import.abc'
        path.write_bytes(abc)
        os.utime(path, ns=(modified, modified))
        if complete:
            movie = out / 'playerglobal_avm2.swf'
            movie.write_bytes(b'current movie marker')
            os.utime(movie, ns=(modified + 1, modified + 1))
        return path

    def test_old_release_globals_never_satisfy_current_import(self):
        out = self.root / 'release/build/ruffle_core-old/out'
        out.mkdir(parents=True)
        (out / 'playerglobal.swf').write_bytes(b'old API remains on disk')
        with self.assertRaisesRegex(RuntimeError, 'current playerglobal_import'):
            playerglobal_import_path(self.root)

    def test_newest_complete_debug_or_release_import_wins(self):
        self.output('release', 'older', b'older current API', 1_000_000_000)
        newest = self.output('debug', 'newer', b'newest current API', 2_000_000_000)
        self.output('release', 'incomplete', b'not completed', 3_000_000_000, complete=False)
        self.assertEqual(playerglobal_import_path(self.root), newest)
        self.assertEqual(playerglobal_import_path(self.root).read_bytes(), b'newest current API')

    def test_empty_or_still_building_import_is_rejected(self):
        path = self.output('debug', 'empty', b'', 1_000_000_000)
        with self.assertRaisesRegex(RuntimeError, 'completed current'):
            playerglobal_import_path(self.root)
        path.write_bytes(b'replacement not finished')
        os.utime(path, ns=(2_000_000_000, 2_000_000_000))
        with self.assertRaisesRegex(RuntimeError, 'completed current'):
            playerglobal_import_path(self.root)


if __name__ == '__main__':
    unittest.main()
