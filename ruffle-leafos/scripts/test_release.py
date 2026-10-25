"""Exercise release decisions against real Git history without contacting GitHub."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import release
import setup_release_tools


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / 'ruffle-leafos'
        self.project.mkdir()
        for key, value in [('ROOT', self.root), ('PROJECT', self.project)]:
            patched = patch.object(release, key, value)
            patched.start()
            self.addCleanup(patched.stop)
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', '测试开发者')
        self.git('config', 'user.email', 'test@example.invalid')
        self.write_version('0.6.8')
        self.first = self.commit('初始版本')

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, encoding='utf8',
                                       stderr=subprocess.DEVNULL).strip()

    def write_version(self, version):
        manifest = self.project / 'pak/pak.json'
        manifest.parent.mkdir(exist_ok=True)
        manifest.write_text(json.dumps({'version': version, 'pak_version': version}), encoding='utf8')
        cargo = self.project / 'player/sdl2test-rocknix/Cargo.toml'
        cargo.parent.mkdir(parents=True, exist_ok=True)
        cargo.write_text(f'[package]\nversion = "{version}"\n', encoding='utf8')
        cargo_lock = self.project / 'player/Cargo.lock'
        cargo_lock.write_text('# fixture\n', encoding='utf8')
        lock = {'frontend_version': version,
                'leaf_cargo_lock_sha256': hashlib.sha256(cargo_lock.read_bytes()).hexdigest(),
                'leaf_files_sha256': {'sdl2test-rocknix/Cargo.toml': hashlib.sha256(cargo.read_bytes()).hexdigest()}}
        (self.project / 'leaf-source.lock.json').write_text(json.dumps(lock), encoding='utf8')

    def commit(self, subject):
        self.git('add', 'ruffle-leafos')
        self.git('commit', '--allow-empty', '-m', subject)
        return self.git('rev-parse', 'HEAD')

    def test_same_version_skips_even_when_manifest_metadata_changes(self):
        manifest = self.project / 'pak/pak.json'
        data = json.loads(manifest.read_text())
        data['description'] = 'updated'
        manifest.write_text(json.dumps(data), encoding='utf8')
        self.commit('metadata only')
        self.assertEqual(release.detect('push', {'before': self.first})['changed'], 'false')

    def test_multi_commit_push_detects_version_from_before_entire_push(self):
        self.write_version('0.6.9')
        self.commit('版本更新')
        last = self.commit('后续文档')
        self.assertEqual(release.detect('push', {'before': last})['changed'], 'false')
        result = release.detect('push', {'before': self.first})
        self.assertEqual(result, {'changed': 'true', 'version': '0.6.9', 'tag': 'v0.6.9'})

    def test_initial_push_and_manual_retry(self):
        self.assertEqual(release.detect('push', {'before': '0' * 40})['changed'], 'true')
        self.assertEqual(release.detect('workflow_dispatch', {})['changed'], 'true')

    def test_missing_before_commit_fails_instead_of_publishing(self):
        with self.assertRaises(subprocess.CalledProcessError):
            release.detect('push', {'before': '1' * 40})

    def test_versions_and_source_fingerprints_must_match(self):
        cargo = self.project / 'player/sdl2test-rocknix/Cargo.toml'
        cargo.write_text('[package]\nversion = "0.6.9"\n', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'must agree'):
            release.version()
        self.write_version('0.6.8')
        cargo.write_text(cargo.read_text() + '# changed input\n', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'fingerprint'):
            release.version()

    def test_unsafe_version_is_rejected(self):
        self.write_version('0.6.9\nchanged=false')
        with self.assertRaisesRegex(ValueError, 'MAJOR.MINOR.PATCH'):
            release.version()

    def test_existing_tag_can_only_be_retried_at_original_commit(self):
        self.git('tag', '-a', 'v0.6.8', '-m', 'release')
        self.assertEqual(release.detect('workflow_dispatch', {})['changed'], 'true')
        self.commit('same version, other source')
        with self.assertRaisesRegex(ValueError, 'another commit'):
            release.detect('workflow_dispatch', {})

    def prepare_archive(self):
        output = self.root / 'output'
        output.mkdir()
        archive = output / f'ruffle-leafos-{release.version()}.zip'
        archive.write_bytes(b'installable ZIP fixture')
        return output, archive

    def test_notes_include_build_commit_and_all_changes_after_previous_tag(self):
        self.git('tag', 'v0.6.8')
        self.commit('修改 <html> & "引号"')
        self.write_version('0.6.9')
        head = self.commit('升级版本')
        output, archive = self.prepare_archive()
        with patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo'}):
            release.notes(output)
        notes = (output / 'release-notes.md').read_text(encoding='utf8')
        self.assertIn(head, notes)
        self.assertIn('修改 &lt;html&gt; &amp; &quot;引号&quot;', notes)
        self.assertIn('升级版本', notes)
        self.assertNotIn('初始版本', notes)
        self.assertIn('/compare/v0.6.8...v0.6.9', notes)
        self.assertIn(hashlib.sha256(archive.read_bytes()).hexdigest(), notes)
        self.assertIn('SD 卡根目录', notes)

    def test_first_release_notes_include_history(self):
        output, _ = self.prepare_archive()
        with patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo'}):
            release.notes(output)
        self.assertIn('初始版本', (output / 'release-notes.md').read_text(encoding='utf8'))

    def test_publish_only_attaches_installation_zip_to_exact_commit(self):
        output, archive = self.prepare_archive()
        missing = subprocess.CompletedProcess([], 1, '', 'gh: Not Found (HTTP 404)')
        with patch.dict(os.environ, {'GH_REPO': 'owner/repo'}), \
                patch.object(release, 'run', return_value=missing) as run:
            release.publish(output)
        command = run.call_args_list[-1].args[0]
        self.assertEqual(command[:4], ['gh', 'release', 'create', 'v0.6.8'])
        self.assertIn(str(archive), command)
        self.assertEqual(command[command.index('--target') + 1], self.first)
        self.assertIn('--notes-file', command)

    def test_api_error_is_not_treated_as_missing_release(self):
        output, _ = self.prepare_archive()
        failure = subprocess.CompletedProcess([], 1, '', 'HTTP 403 forbidden')
        with patch.dict(os.environ, {'GH_REPO': 'owner/repo'}), \
                patch.object(release, 'run', return_value=failure) as run:
            with self.assertRaisesRegex(RuntimeError, '403'):
                release.publish(output)
        self.assertEqual(run.call_count, 1)

    def test_published_release_retry_does_not_replace_assets(self):
        output, _ = self.prepare_archive()
        existing = subprocess.CompletedProcess([], 0, json.dumps({'draft': False, 'html_url': 'https://example.invalid'}), '')
        with patch.dict(os.environ, {'GH_REPO': 'owner/repo'}), \
                patch.object(release, 'run', return_value=existing) as run:
            release.publish(output)
        self.assertEqual(run.call_count, 1)

    def test_interrupted_draft_upload_can_be_resumed(self):
        output, archive = self.prepare_archive()
        draft = subprocess.CompletedProcess([], 0, json.dumps({'draft': True}), '')
        with patch.dict(os.environ, {'GH_REPO': 'owner/repo'}), \
                patch.object(release, 'run', return_value=draft) as run:
            release.publish(output)
        upload = run.call_args_list[1].args[0]
        publish = run.call_args_list[2].args[0]
        self.assertEqual(upload, ['gh', 'release', 'upload', 'v0.6.8', str(archive), '--clobber'])
        self.assertEqual(publish[:4], ['gh', 'release', 'edit', 'v0.6.8'])
        self.assertIn('--draft=false', publish)


class DownloadTests(unittest.TestCase):
    def test_archives_with_and_without_outer_directory(self):
        import zipfile
        for prefix in ('toolchain/', ''):
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                archive = root / 'tool.zip'
                with zipfile.ZipFile(archive, 'w') as target:
                    target.writestr(prefix + 'bin/compiler.exe', b'locked compiler')
                    target.writestr(prefix + 'manifest.txt', b'manifest')
                with patch.object(setup_release_tools, 'TOOLS', root), \
                        patch.object(setup_release_tools, 'download', return_value=archive):
                    setup_release_tools.install_tree('https://example.invalid/tool.zip', 'hash', root / 'installed')
                self.assertEqual((root / 'installed/bin/compiler.exe').read_bytes(), b'locked compiler')

    def test_bad_tool_checksum_fails_before_installation(self):
        from io import BytesIO
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(setup_release_tools, 'DOWNLOADS', Path(directory)), \
                patch.object(setup_release_tools.urllib.request, 'urlopen', return_value=BytesIO(b'bad archive')):
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                setup_release_tools.download('https://example.invalid/tool.zip', '0' * 64)
            self.assertFalse((Path(directory) / 'tool.zip').exists())


if __name__ == '__main__':
    unittest.main()
