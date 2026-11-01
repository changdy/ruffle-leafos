"""Check the stale-ELF packaging gate without compiling or packaging real output."""
from pathlib import Path
import copy
from contextlib import redirect_stdout
import io
import json
import os
import runpy
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import package_preview
from build_inputs import (
    FIXED_INPUTS, invalidate_build_metadata, require_unchanged_inputs,
    sha256_file, snapshot_inputs, verify_player_build,
)


class BuildInputsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for relative in FIXED_INPUTS:
            self.write(relative, b'fixed input\n')
        self.inputs = {
            'player/Cargo.toml': b'[workspace]\n',
            'player/Cargo.lock': b'cargo dependency lock\n',
            'player/.cargo/config.toml': b'[build]\njobs = 4\n',
            'player/core/build.rs': b'fn main() {}\n',
            'player/core/src/player.rs': b'pub struct Player;\n',
            'player/core/src/avm2/globals/Object.as': b'public class Object {}\n',
            'player/core/build_playerglobal/asc.jar': b'original compiler jar\0',
            'player/render/wgpu/shaders/filter.wgsl': b'fn shader() {}\n',
            'player/core/assets/font.ttf': b'embedded font\0',
            'player/core/src/player/tests/probe.swf': b'FWS embedded fixture\0',
            'player/c_inc/SDL.h': b'/* target header */\n',
            'player/sdl2test-rocknix/Cargo.toml': b'[package]\nversion = "0.8.3"\n',
            'target-libs/libSDL2.so': b'fixed ELF link input\0',
            'target-libs/pkgconfig/alsa.pc': b'libdir=C:/workspace/target-libs\nLibs: -lasound\n',
            'target-libs/provenance.json': b'{"purpose":"fixed link inputs"}\n',
        }
        for relative, content in self.inputs.items():
            self.write(relative, content)
        self.write('pak/pak.json', b'{"pak_version":"0.8.3"}\n')
        self.binary = self.write('build/ruffle-leaf', b'pretend ELF bytes for hash checks')
        self.metadata = self.root / 'build/player-build.json'

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def build_record(self):
        record = {'binary_sha256': sha256_file(self.binary), 'source_inputs': snapshot_inputs(self.root)}
        self.metadata.write_text(json.dumps(record), encoding='utf8')
        return record

    def test_snapshot_is_content_based_and_independent_of_absolute_checkout(self):
        first = snapshot_inputs(self.root)
        changed_time = self.root / 'player/core/src/player.rs'
        stat = changed_time.stat()
        os.utime(changed_time, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1000000000))
        self.assertEqual(snapshot_inputs(self.root), first)
        with tempfile.TemporaryDirectory() as other:
            other_root = Path(other)
            for relative, content_hash in first['files_sha256'].items():
                dest = other_root / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes((self.root / relative).read_bytes())
                self.assertEqual(sha256_file(dest), content_hash)
            self.assertEqual(snapshot_inputs(other_root), first)

    def test_all_declared_source_asset_manifest_lock_and_link_inputs_are_hashed(self):
        before = snapshot_inputs(self.root)
        for relative in [*FIXED_INPUTS, *self.inputs]:
            with self.subTest(relative=relative):
                path = self.root / relative
                original = path.read_bytes()
                path.write_bytes(original + b' changed')
                with self.assertRaisesRegex(ValueError, relative.replace('.', r'\.')):
                    require_unchanged_inputs(before, snapshot_inputs(self.root))
                path.write_bytes(original)

    def test_uncommitted_additions_and_deletions_are_detected_without_git(self):
        before = snapshot_inputs(self.root)
        path = self.write('player/core/src/new_module.rs', b'pub fn new_module() {}')
        with self.assertRaisesRegex(ValueError, 'new_module'):
            require_unchanged_inputs(before, snapshot_inputs(self.root))
        path.unlink()
        (self.root / 'player/core/src/player.rs').unlink()
        with self.assertRaisesRegex(ValueError, 'player.rs'):
            require_unchanged_inputs(before, snapshot_inputs(self.root))

    def test_added_native_link_input_is_detected(self):
        before = snapshot_inputs(self.root)
        self.write('target-libs/libnew.so', b'new native input')
        with self.assertRaisesRegex(ValueError, 'libnew.so'):
            require_unchanged_inputs(before, snapshot_inputs(self.root))

    def test_same_size_and_mtime_do_not_hide_changed_source_bytes(self):
        path = self.root / 'player/core/src/player.rs'
        before = snapshot_inputs(self.root)
        stat = path.stat()
        path.write_bytes(b'x' * stat.st_size)
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        with self.assertRaisesRegex(ValueError, 'player.rs'):
            require_unchanged_inputs(before, snapshot_inputs(self.root))

    def test_docs_provenance_and_generated_local_files_do_not_require_rebuild(self):
        before = snapshot_inputs(self.root)
        for relative in (
            'docs/validation/report.md', 'leaf-source.lock.json', 'README.md',
            'player/README.md', 'player/core/AGENTS.md', 'player/.git/index',
            'player/target/debug/generated.rs', 'player/build/generated.rs',
            'player/.tools/compiler.exe', 'player/.env', 'player/.env.local',
            'player/workspace.local.json', 'player/core/file.rs.bk',
            'player/core/session.log', 'target-libs/__pycache__/cached.pyc',
        ):
            self.write(relative, b'not a project ELF build input')
        self.assertEqual(snapshot_inputs(self.root), before)

    def test_missing_fixed_input_fails_closed(self):
        (self.root / 'scripts/clang_target.py').unlink()
        with self.assertRaisesRegex(ValueError, 'clang_target.py'):
            snapshot_inputs(self.root)

    def test_missing_input_tree_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for relative in FIXED_INPUTS:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'fixed input')
            with self.assertRaisesRegex(ValueError, 'directory: player'):
                snapshot_inputs(root)

    def test_matching_elf_and_project_inputs_are_accepted(self):
        verify_player_build(self.root, self.binary, self.build_record())

    def test_old_or_missing_snapshot_metadata_is_rejected(self):
        for record in (None, [], {}, {'binary_sha256': sha256_file(self.binary)}):
            with self.subTest(record=record):
                with self.assertRaisesRegex(ValueError, 'metadata|snapshot'):
                    verify_player_build(self.root, self.binary, record)

    def test_corrupt_and_unsupported_snapshots_are_rejected(self):
        record = self.build_record()
        for field, value in (('schema_version', 0), ('files_sha256', []), ('sha256', 'bad digest')):
            broken = copy.deepcopy(record)
            broken['source_inputs'][field] = value
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, 'snapshot'):
                    verify_player_build(self.root, self.binary, broken)
        broken = copy.deepcopy(record)
        broken['source_inputs']['files_sha256']['player/Cargo.toml'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'snapshot'):
            verify_player_build(self.root, self.binary, broken)

    def test_different_old_elf_is_rejected_even_with_current_source_snapshot(self):
        record = self.build_record()
        self.binary.write_bytes(b'another old ELF')
        with self.assertRaisesRegex(ValueError, 'does not match player ELF'):
            verify_player_build(self.root, self.binary, record)

    def test_updating_the_source_lock_does_not_relabel_an_old_elf_as_current(self):
        record = self.build_record()
        self.write('player/core/src/player.rs', b'new source after compiling the ELF')
        self.write('leaf-source.lock.json', b'{"updated":"for the new source"}')
        with self.assertRaisesRegex(ValueError, 'player.rs'):
            verify_player_build(self.root, self.binary, record)

    def test_starting_build_invalidates_success_record_without_touching_old_elf(self):
        self.build_record()
        original = self.binary.read_bytes()
        invalidate_build_metadata(self.root)
        self.assertFalse(self.metadata.exists())
        self.assertEqual(self.binary.read_bytes(), original)
        invalidate_build_metadata(self.root)

    def test_source_change_during_build_keeps_old_elf_and_no_success_record(self):
        self.build_record()
        original = self.binary.read_bytes()
        invalidate_build_metadata(self.root)
        before = snapshot_inputs(self.root)
        self.write('player/core/src/player.rs', b'changed while Cargo ran')
        with self.assertRaisesRegex(ValueError, 'player.rs'):
            require_unchanged_inputs(before, snapshot_inputs(self.root))
        self.assertFalse(self.metadata.exists())
        self.assertEqual(self.binary.read_bytes(), original)

    def run_build_script(self, cargo_action=None, inspect_error=None):
        # Execute the actual entry point from a temporary checkout. Only the
        # compiler/ELF inspector are stubbed; metadata invalidation, snapshots,
        # target wrapper generation and publishing use the real script code.
        script = self.write('scripts/build_player.py',
                            (Path(__file__).parent / 'build_player.py').read_bytes())
        self.write('arm-gnu.lock.json', b'{"root":".tools/sdk"}')
        zig = self.write('.tools/zig.exe', b'fake tool; never executed')
        self.write('workspace.local.json', json.dumps({'zig': str(zig)}).encode())
        old_binary = self.binary.read_bytes()
        self.build_record()

        def run_cargo(*_args, **_kwargs):
            self.assertFalse(self.metadata.exists(), 'old record must be invalid before Cargo')
            self.assertEqual(self.binary.read_bytes(), old_binary)
            self.write('build/cargo/aarch64-unknown-linux-gnu/release/sdl2test-rocknix', b'new compiled ELF')
            returncode = cargo_action() if cargo_action else 0
            return SimpleNamespace(returncode=returncode)

        def inspect_elf(_path):
            if inspect_error:
                raise inspect_error
            return {'machine': 183}

        with redirect_stdout(io.StringIO()), \
                patch('sys.path', list(sys.path)), \
                patch('sys.argv', ['build_player.py', 'build', '--offline', '--isolated-config']), \
                patch('subprocess.run', side_effect=run_cargo), \
                patch('subprocess.check_output', return_value='rustc test version'), \
                patch.dict('sys.modules', {'elf_info': SimpleNamespace(inspect=inspect_elf)}):
            runpy.run_path(str(script), run_name='__main__')

    def test_actual_build_entry_point_publishes_matching_source_and_elf_record(self):
        self.run_build_script()
        self.assertEqual(self.binary.read_bytes(), b'new compiled ELF')
        record = json.loads(self.metadata.read_text(encoding='utf8'))
        verify_player_build(self.root, self.binary, record)
        self.assertEqual(record['target'], 'aarch64-unknown-linux-gnu')

    def test_actual_build_entry_point_keeps_old_elf_after_compiler_failure(self):
        old = self.binary.read_bytes()
        with self.assertRaises(SystemExit) as error:
            self.run_build_script(cargo_action=lambda: 1)
        self.assertEqual(error.exception.code, 1)
        self.assertEqual(self.binary.read_bytes(), old)
        self.assertFalse(self.metadata.exists())

    def test_actual_build_entry_point_rejects_source_changed_while_cargo_runs(self):
        old = self.binary.read_bytes()
        def change_source():
            self.write('player/core/src/player.rs', b'changed during Cargo')
            return 0
        with self.assertRaisesRegex(ValueError, 'player.rs'):
            self.run_build_script(cargo_action=change_source)
        self.assertEqual(self.binary.read_bytes(), old)
        self.assertFalse(self.metadata.exists())
        self.assertFalse(list((self.root / 'build').glob('ruffle-leaf.*.pending')))

    def test_actual_build_entry_point_keeps_old_elf_when_inspection_fails(self):
        old = self.binary.read_bytes()
        with self.assertRaisesRegex(ValueError, 'invalid test ELF'):
            self.run_build_script(inspect_error=ValueError('invalid test ELF'))
        self.assertEqual(self.binary.read_bytes(), old)
        self.assertFalse(self.metadata.exists())

    def test_package_entry_point_checks_binding_before_inspecting_or_staging(self):
        record = self.build_record()
        for case in ('missing', 'old', 'corrupt-json', 'source-change', 'old-elf'):
            with self.subTest(case=case):
                self.metadata.write_text(json.dumps(record), encoding='utf8')
                self.binary.write_bytes(b'pretend ELF bytes for hash checks')
                self.write('player/core/src/player.rs', self.inputs['player/core/src/player.rs'])
                if case == 'missing':
                    self.metadata.unlink()
                elif case == 'old':
                    self.metadata.write_text(json.dumps({'binary_sha256': sha256_file(self.binary)}))
                elif case == 'corrupt-json':
                    self.metadata.write_text('{broken')
                elif case == 'source-change':
                    self.write('player/core/src/player.rs', b'new unbuilt source')
                else:
                    self.binary.write_bytes(b'another ELF')
                with patch.object(package_preview, 'ROOT', self.root), patch('sys.argv', [
                    'package_preview.py', '--output', str(self.root / 'output'),
                ]), patch.object(package_preview, 'inspect') as inspect_elf:
                    with self.assertRaises(ValueError):
                        package_preview.main()
                    inspect_elf.assert_not_called()
                self.assertFalse(any((self.root / 'output').iterdir()))

    def test_package_entry_point_accepts_binding_then_runs_elf_checks(self):
        self.build_record()
        class ReachedElfCheck(Exception):
            pass
        with patch.object(package_preview, 'ROOT', self.root), patch('sys.argv', [
            'package_preview.py', '--output', str(self.root / 'output'),
        ]), patch.object(package_preview, 'inspect', side_effect=ReachedElfCheck) as inspect_elf:
            with self.assertRaises(ReachedElfCheck):
                package_preview.main()
            inspect_elf.assert_called_once_with(self.binary)
        self.assertFalse(any((self.root / 'output').iterdir()))

    def prepare_package_locks(self):
        self.build_record()
        self.write('leaf-source.lock.json', json.dumps({
            'leaf_cargo_lock_sha256': sha256_file(self.root / 'player/Cargo.lock'),
            'leaf_files_sha256': {},
        }).encode())
        for name in ('jawaka', 'catastrophe', 'ruffle-wgpu'):
            self.write('vendor/' + name + '.lock.json', b'{"files_sha256":{}}')
        (self.root / 'pak/emulators/ruffle-probe').mkdir(parents=True)

    def test_package_rejects_elf_replaced_between_initial_check_and_copy(self):
        self.prepare_package_locks()
        real_copyfile = shutil.copyfile
        def replace_elf_during_copy(source, destination, *args, **kwargs):
            if Path(source) == self.binary:
                self.binary.write_bytes(b'a different ELF published during packaging')
            return real_copyfile(source, destination, *args, **kwargs)
        with patch.object(package_preview, 'ROOT', self.root), patch('sys.argv', [
            'package_preview.py', '--output', str(self.root / 'output'),
        ]), patch.object(package_preview, 'inspect', return_value={
            'machine': 183, 'type': 3, 'glibc_versions': ['GLIBC_2.35'],
        }), patch.object(package_preview.shutil, 'copyfile', side_effect=replace_elf_during_copy):
            with self.assertRaisesRegex(ValueError, 'staged player ELF'):
                package_preview.main()
        self.assertFalse(list((self.root / 'output').glob('*.zip')))

    def test_package_rejects_source_changed_after_initial_check_while_copying_elf(self):
        self.prepare_package_locks()
        real_copyfile = shutil.copyfile
        def change_source_during_copy(source, destination, *args, **kwargs):
            result = real_copyfile(source, destination, *args, **kwargs)
            if Path(source) == self.binary:
                self.write('player/core/src/player.rs', b'changed after initial package verification')
            return result
        with patch.object(package_preview, 'ROOT', self.root), patch('sys.argv', [
            'package_preview.py', '--output', str(self.root / 'output'),
        ]), patch.object(package_preview, 'inspect', return_value={
            'machine': 183, 'type': 3, 'glibc_versions': ['GLIBC_2.35'],
        }), patch.object(package_preview.shutil, 'copyfile', side_effect=change_source_during_copy):
            with self.assertRaisesRegex(ValueError, 'player.rs'):
                package_preview.main()
        self.assertFalse(list((self.root / 'output').glob('*.zip')))

    def test_staged_source_copy_must_match_elf_snapshot_not_just_current_source_lock(self):
        record = self.build_record()
        with tempfile.TemporaryDirectory() as directory:
            staged = Path(directory)
            for relative in record['source_inputs']['files_sha256']:
                destination = staged / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(self.root / relative, destination)
            require_unchanged_inputs(record['source_inputs'], snapshot_inputs(staged))
            (staged / 'player/core/src/player.rs').write_bytes(b'edited while the source tree was copied')
            (staged / 'leaf-source.lock.json').write_text('{"fresh":"source lock"}')
            with self.assertRaisesRegex(ValueError, 'player.rs'):
                require_unchanged_inputs(record['source_inputs'], snapshot_inputs(staged))

    def test_symbolic_source_input_is_not_silently_skipped(self):
        target = self.write('external.rs', b'external source')
        link = self.root / 'player/core/src/linked.rs'
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError) as error:
            self.skipTest('host cannot create test symlink: ' + str(error))
        with self.assertRaisesRegex(ValueError, 'symbolic build input'):
            snapshot_inputs(self.root)


if __name__ == '__main__':
    unittest.main()
