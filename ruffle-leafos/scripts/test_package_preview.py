"""Verify bundled games, complete external assets and installation paths."""
from pathlib import Path
import hashlib
import json
import shutil
import tempfile
import unittest
import zipfile

from package_preview import ROOT, bundle_games, bundle_samples, write_install_archives


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
            self.assertEqual(output.suffix, '.zip')
            source_prefix = game['name'] + '/_assets/'
            expected = {'assets/' + path[len(source_prefix):]: digest
                        for path, digest in manifest['files_sha256'].items() if path.startswith(source_prefix)}
            expected[game['archive_main']] = game['main_sha256']
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual({name: hashlib.sha256(archive.read(name)).hexdigest()
                                  for name in archive.namelist()}, expected)
            self.assertEqual(len(expected)-1, 398 if '3.3' in game['name'] else 554 if '3.6' in game['name'] else 0)
        # Inspect the delivered outer ZIPs, not only the staged source files.
        main = self.root / 'main'
        bundle_samples(main)
        (main / 'Apps/mlp1/RuffleProbe.pak').mkdir(parents=True)
        (main / 'Apps/mlp1/RuffleProbe.pak/launch.sh').write_bytes(b'#!/bin/sh\n')
        (main / 'Images/flash').mkdir(parents=True)
        install_zip, games_zip = write_install_archives(main, self.stage, self.root, '0.8.6')
        with zipfile.ZipFile(install_zip) as archive:
            roms = {i.filename for i in archive.infolist()
                    if not i.is_dir() and i.filename.startswith('Roms/')}
            self.assertEqual(roms, {'Roms/flash/Leaf-GPU-Probe.swf',
                                    'Roms/flash/Bloxorz-翻滚方块.swf'})
            self.assertIn('Images/flash/', archive.namelist())
            script = archive.getinfo('Apps/mlp1/RuffleProbe.pak/launch.sh')
            self.assertTrue((script.external_attr >> 16) & 0o111)
            self.assertEqual(archive.read('Roms/flash/Bloxorz-翻滚方块.swf'),
                             (ROOT / 'games/samples/Bloxorz-翻滚方块.swf').read_bytes())
        with zipfile.ZipFile(games_zip) as archive:
            self.assertEqual({i.filename for i in archive.infolist() if not i.is_dir()},
                             {'Roms/flash/' + g['install_path'] for g in games})

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


class SampleGamesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.samples = self.root / 'samples'
        shutil.copytree(ROOT / 'games/samples', self.samples)
        self.stage = self.root / 'stage'

    def test_bloxorz_original_and_diagnostic_are_preserved(self):
        games = bundle_samples(self.stage, self.samples)
        self.assertEqual(len(games), 1)
        self.assertEqual(games[0]['language'], 'en')
        path = self.stage / 'Roms/flash' / games[0]['install_path']
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         '296c5c17b7a7b6529f6b8faa19aae78ebf4382c161c07e5070ab338fd8264803')
        probe = self.stage / 'Roms/flash/Leaf-GPU-Probe.swf'
        self.assertEqual(probe.read_bytes()[:3], b'FWS')

    def test_changed_sample_fails_before_staging(self):
        (self.samples / 'Bloxorz-翻滚方块.swf').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'sample game checksum mismatch'):
            bundle_samples(self.stage, self.samples)
        self.assertFalse(self.stage.exists())

    def test_missing_or_extra_sample_fails_before_staging(self):
        original = self.samples / 'Bloxorz-翻滚方块.swf'
        for extra in (False, True):
            with self.subTest(extra=extra):
                if extra:
                    (self.samples / 'unexpected.swf').write_bytes(b'extra')
                else:
                    original.unlink()
                with self.assertRaisesRegex(ValueError, 'sample game files differ'):
                    bundle_samples(self.stage, self.samples)
                self.assertFalse(self.stage.exists())
                shutil.copyfile(ROOT / 'games/samples/Bloxorz-翻滚方块.swf', original)


if __name__ == '__main__':
    unittest.main()
