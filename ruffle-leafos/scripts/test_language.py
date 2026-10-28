"""Exercise Leaf launch-language fallback using the actual theme shell helper."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LaunchLanguageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        settings_file = ROOT / 'workspace.local.json'
        settings = json.loads(settings_file.read_text(encoding='utf8')) if settings_file.exists() else {}
        shell = Path(settings.get('host_gcc', ROOT / '.tools/w64devkit')) / 'bin/sh.exe'
        cls.shell = str(shell) if shell.exists() else shutil.which('sh')
        if not cls.shell:
            raise unittest.SkipTest('POSIX shell unavailable')

    def resolve(self, **language_env):
        temporary = ROOT / 'build/tmp'
        temporary.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='ruffle-language-', dir=temporary) as directory:
            root = Path(directory)
            database = root / '.umrk/mlp1/library.db'
            database.parent.mkdir(parents=True)
            database.write_bytes(b'fixture')
            env = os.environ.copy()
            for key in ('UMRK_LANGUAGE', 'JAWAKA_LANGUAGE', 'RUFFLE_SYSTEM_LANGUAGE', 'CAT_FONT_PATH'):
                env.pop(key, None)
            env.update(language_env, SDCARD_PATH=root.as_posix(), HOME=root.as_posix())
            script = '''
sqlite3() {
    case "$*" in *"key='language'"*)
        [ "$1" = -readonly ] || exit 9
        printf 'zh_CN\\n'
        ;;
    esac
}
. "$1"
ruffle_load_theme
printf '%s' "${RUFFLE_SYSTEM_LANGUAGE:-unset}"
'''
            return subprocess.check_output([self.shell, '-c', script, 'test',
                                            (ROOT / 'pak/emulators/ruffle-probe/theme.sh').as_posix()],
                                           env=env, text=True).strip()

    def test_old_launch_reads_language_from_selected_card_database(self):
        self.assertEqual(self.resolve(), 'zh_CN')

    def test_canonical_leaf_environment_skips_database_language(self):
        self.assertEqual(self.resolve(UMRK_LANGUAGE='en'), 'unset')

    def test_compatibility_environment_skips_database_language(self):
        self.assertEqual(self.resolve(JAWAKA_LANGUAGE='en'), 'unset')


if __name__ == '__main__':
    unittest.main()
