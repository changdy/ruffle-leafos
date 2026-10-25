"""Detect version changes and publish one installable Pocket 1 ZIP."""
from pathlib import Path
import argparse
import hashlib
import html
import json
import os
import re
import subprocess
from subprocess import run
import tomllib

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / 'ruffle-leafos'
MANIFEST = 'ruffle-leafos/pak/pak.json'
VERSION = re.compile(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, encoding='utf8').strip()


def version():
    manifest = json.loads((ROOT / MANIFEST).read_text(encoding='utf8'))
    current = manifest['pak_version']
    if not isinstance(current, str) or not VERSION.fullmatch(current):
        raise ValueError('pak_version must have the form MAJOR.MINOR.PATCH')
    cargo = tomllib.loads((PROJECT / 'player/sdl2test-rocknix/Cargo.toml').read_text(encoding='utf8'))
    lock = json.loads((PROJECT / 'leaf-source.lock.json').read_text(encoding='utf8'))
    if any(v != current for v in (manifest['version'], cargo['package']['version'], lock['frontend_version'])):
        raise ValueError('pak version, pak_version, Cargo version and frontend_version must agree')
    for relative, expected in lock['leaf_files_sha256'].items():
        if hashlib.sha256((PROJECT / 'player' / relative).read_bytes()).hexdigest() != expected:
            raise ValueError('Update leaf-source.lock.json fingerprint for ' + relative)
    if hashlib.sha256((PROJECT / 'player/Cargo.lock').read_bytes()).hexdigest() != lock['leaf_cargo_lock_sha256']:
        raise ValueError('Update leaf-source.lock.json Cargo.lock fingerprint')
    return current


def check_tag(tag):
    existing = git('tag', '--list', tag)
    if existing and git('rev-parse', tag + '^{commit}') != git('rev-parse', 'HEAD'):
        raise ValueError(tag + ' already points to another commit; use a new version')


def detect(event_name, event):
    current = version()
    if event_name == 'workflow_dispatch':
        changed = True
    elif event_name == 'push':
        before = event.get('before', '')
        if before and set(before) != {'0'}:
            # Use the pre-push tree, so multi-commit pushes and merges work too.
            previous = json.loads(git('show', before + ':' + MANIFEST))['pak_version']
            changed = previous != current
        else:
            changed = True
    else:
        raise ValueError('Only push and workflow_dispatch can publish releases')
    if changed:
        check_tag('v' + current)
    return {'changed': str(changed).lower(), 'version': current, 'tag': 'v' + current}


def previous_tag(current):
    candidates = [tag for tag in git('tag', '--merged', 'HEAD', '--sort=-version:refname').splitlines()
                  if tag != 'v' + current and tag.startswith('v') and VERSION.fullmatch(tag[1:])]
    return candidates[0] if candidates else None


def notes(output):
    current = version()
    check_tag('v' + current)
    archive = output / f'ruffle-leafos-{current}.zip'
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    head = git('rev-parse', 'HEAD')
    repository = os.environ['GITHUB_REPOSITORY']
    base_url = os.environ.get('GITHUB_SERVER_URL', 'https://github.com') + '/' + repository
    previous = previous_tag(current)
    revision = previous + '..HEAD' if previous else 'HEAD'
    commits = git('log', '--reverse', '--format=%H%x09%s%x09%an', revision).splitlines()
    lines = [f'# Ruffle Flash {current} 开发预览', '',
             '适用于迷你龙 Miniloong Pocket 1 / LeafOS（ARM64，最低 Leaf 0.13.0）。', '',
             f'下载 **{archive.name}**，退出播放器后解压，将 `Apps/`、`Roms/`、`Images/` '
             '按原层级复制到系统 SD 卡根目录，合并目录即可安装或覆盖升级。',
             '建议先备份现有安装和用户数据。游戏放入 `Roms/flash/`。', '',
             f'构建提交：[{head}]({base_url}/commit/{head})',
             '自动验证：ARM64 release 编译、主机输入/状态回归、诊断库测试和安装包校验。'
             '本次 CI 未进行实机验证。', '', '## Commits', '']
    for commit in commits:
        sha, subject, author = commit.split('\t', 2)
        # Commit text is data, and must not become HTML, links or mentions.
        lines.append(f'- [{sha[:8]}]({base_url}/commit/{sha}) '
                     f'<code>{html.escape(subject)}</code> — <code>{html.escape(author)}</code>')
    if previous:
        lines += ['', f'[完整变更：{previous} → v{current}]({base_url}/compare/{previous}...v{current})']
    lines += ['', '## SHA-256', '', f'```text\n{checksum}  {archive.name}\n```', '']
    (output / 'release-notes.md').write_text('\n'.join(lines), encoding='utf8')


def publish(output):
    current = version()
    tag = 'v' + current
    check_tag(tag)
    # Check API failures explicitly; an authentication/server error is not a missing release.
    result = run(['gh', 'api', f'repos/{os.environ["GH_REPO"]}/releases/tags/{tag}'],
                            text=True, encoding='utf8', capture_output=True)
    if result.returncode == 0:
        release = json.loads(result.stdout)
        if not release['draft']:
            print('Release already published: ' + release['html_url'])
            return
        run(['gh', 'release', 'upload', tag, str(output / f'ruffle-leafos-{current}.zip'),
                        '--clobber'], check=True)
        run(['gh', 'release', 'edit', tag, '--draft=false', '--prerelease',
                        '--notes-file', str(output / 'release-notes.md')], check=True)
    elif '(HTTP 404)' in result.stderr:
        run(['gh', 'release', 'create', tag, str(output / f'ruffle-leafos-{current}.zip'),
                        '--target', git('rev-parse', 'HEAD'), '--title', f'Ruffle Flash {current}',
                        '--notes-file', str(output / 'release-notes.md'), '--prerelease'], check=True)
    else:
        raise RuntimeError(result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['detect', 'notes', 'publish'])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.action == 'detect':
        event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf8'))
        values = detect(os.environ['GITHUB_EVENT_NAME'], event)
        with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf8') as output:
            output.write(''.join(f'{key}={value}\n' for key, value in values.items()))
        print(json.dumps(values))
    elif args.output is None:
        parser.error('--output is required for notes and publish')
    elif args.action == 'notes':
        notes(args.output.resolve())
    else:
        publish(args.output.resolve())


if __name__ == '__main__':
    main()
