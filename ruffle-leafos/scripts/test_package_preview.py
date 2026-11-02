"""Verify bundled games, complete external assets and installation paths."""
from pathlib import Path
import hashlib
import json
import tempfile
import unittest
import zipfile

from package_preview import ROOT, bundle_games


class BundledGamesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.games = self.root / 'games'
        self.stage = self.root / 'stage'
        self.relative = '测试-定制版/_assets/fighter.swf'
        self.resource = self.games / 'flash' / self.relative
        self.resource.parent.mkdir(parents=True)
        self.resource.write_bytes(b'external SWF fixture')
        self.main = self.games / 'flash/测试-定制版/测试.swf'
        self.main.write_bytes(b'main SWF fixture')
        self.manifest = {
            'games': [{'name': '测试-定制版', 'entrypoint': '测试-定制版/测试.swf'}],
            'files_sha256': {p.relative_to(self.games / 'flash').as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in (self.main, self.resource)},
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
        self.assertEqual([g['name'] for g in games], ['拳皇Wing1.91-定制版', '死神VS火影3.3-定制版', '死神VS火影3.6-定制版'])
        manifest = json.loads((ROOT / 'games/manifest.json').read_text(encoding='utf8'))
        installed = self.stage / 'Roms/flash'
        actual = {p.relative_to(installed).as_posix() for p in installed.rglob('*') if p.is_file()}
        self.assertEqual(actual, {g['install_path'] for g in games})
        for game in games:
            output = installed / game['install_path']
            if output.suffix == '.swf':
                self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), game['main_sha256'])
                continue
            source_prefix = game['name'] + '/_assets/'
            expected = {'assets/' + path[len(source_prefix):]: digest
                        for path, digest in manifest['files_sha256'].items() if path.startswith(source_prefix)}
            expected[game['archive_main']] = game['main_sha256']
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual({name: hashlib.sha256(archive.read(name)).hexdigest()
                                  for name in archive.namelist()}, expected)
            self.assertEqual(len(expected)-1, 398 if '3.3' in game['name'] else 554)

    def test_exposed_resource_movie_is_rejected(self):
        extra = self.games / 'flash/测试-定制版/assets/fighter.swf'
        extra.parent.mkdir()
        extra.write_bytes(self.resource.read_bytes())
        relative = extra.relative_to(self.games / 'flash').as_posix()
        self.manifest['files_sha256'][relative] = hashlib.sha256(extra.read_bytes()).hexdigest()
        (self.games / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'game list differs from entrypoints'):
            bundle_games(self.stage, self.games)
        self.assertFalse(self.stage.exists())


if __name__ == '__main__':
    unittest.main()
