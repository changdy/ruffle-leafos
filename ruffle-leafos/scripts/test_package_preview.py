"""Verify bundled games, complete external assets and installation paths."""
from pathlib import Path
import hashlib
import json
import tempfile
import unittest

from package_preview import ROOT, bundle_games


class BundledGamesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.games = self.root / 'games'
        self.stage = self.root / 'stage'
        self.relative = '测试-定制版/assets/fighter.swf'
        self.resource = self.games / 'flash' / self.relative
        self.resource.parent.mkdir(parents=True)
        self.resource.write_bytes(b'external SWF fixture')
        self.manifest = {
            'games': [{'name': '测试-定制版', 'entrypoint': self.relative}],
            'files_sha256': {self.relative: hashlib.sha256(self.resource.read_bytes()).hexdigest()},
        }
        (self.games / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf8')

    def test_missing_or_unexpected_external_assets_fail_packaging(self):
        self.resource.rename(self.resource.with_suffix('.missing'))
        with self.assertRaisesRegex(ValueError, 'bundled game files differ'):
            bundle_games(self.stage, self.games)
        self.assertFalse(self.stage.exists())

    def test_changed_asset_fails_packaging(self):
        self.resource.write_bytes(b'changed resource')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            bundle_games(self.stage, self.games)

    def test_missing_entrypoint_fails_packaging(self):
        self.manifest['games'][0]['entrypoint'] = 'missing.swf'
        (self.games / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'entrypoint missing'):
            bundle_games(self.stage, self.games)

    def test_repository_games_install_with_all_assets(self):
        games = bundle_games(self.stage)
        self.assertEqual([g['name'] for g in games], ['拳皇Wing1.91-定制版', '死神VS火影3.3-定制版'])
        manifest = json.loads((ROOT / 'games/manifest.json').read_text(encoding='utf8'))
        installed = self.stage / 'Roms/flash'
        actual = {p.relative_to(installed).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in installed.rglob('*') if p.is_file()}
        self.assertEqual(actual, manifest['files_sha256'])
        self.assertEqual(len(actual), 402)
        for game in games:
            self.assertTrue((installed / game['entrypoint']).is_file())
        self.assertEqual(len(list((installed / '死神VS火影3.3-定制版/assets').rglob('*.*'))), 398)


if __name__ == '__main__':
    unittest.main()
