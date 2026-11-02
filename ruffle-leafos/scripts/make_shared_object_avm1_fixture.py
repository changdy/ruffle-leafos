"""Generate an original AVM1 SharedObject probe and genuinely old AMF0 bytes.

The SWF is assembled from original actions, with no ASC or playerglobal input.
The SOL is produced by the exact flash-lso revision pinned by the old fork,
using its actual Amf0Writer/ObjWriter path, including ECMA Array and Object.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess

from make_keyboard_fixtures import ROOT, action, push, tag, write_movie


OLD_WRITER_REV = '998f47c926b9986aabd518fbb7394ff56936d0b0'
WRITER_SOURCE = r'''
use flash_lso::amf0::writer::{Amf0Writer, CacheKey, ObjWriter};

fn main() {
    let identities = [0u8, 1u8];
    let mut writer = Amf0Writer::default();
    writer.string("marker", "legacy-avm1-v0.8.3");
    writer.string("text", "旧档✓");
    writer.number("counter", 41.0);
    writer.number("ratio", 1.25);
    writer.bool("enabled", true);
    {
        let (array, _) = writer.array(CacheKey::from_ptr(&identities[0]));
        let mut array = array.unwrap();
        array.number("0", 1.0);
        array.string("1", "two");
        array.bool("2", false);
        array.commit("items", 3);
    }
    {
        let (profile, _) = writer.object(CacheKey::from_ptr(&identities[1]));
        let mut profile = profile.unwrap();
        profile.number("level", 3.0);
        profile.string("note", "nested");
        profile.commit("profile");
    }
    let mut lso = writer.commit_lso("leaf-shared-avm1");
    let bytes = flash_lso::write::write_to_bytes(&mut lso).expect("old AMF0 writer failed");
    std::fs::write(std::env::args_os().nth(1).expect("output .sol path"), bytes).unwrap();
}
'''


def variable(name):
    return push(name) + action(0x1c)  # GetVariable


def member(base, name):
    return base + push(name) + action(0x4e)  # GetMember


def set_member(base, name, value):
    return base + push(name) + value + action(0x4f)  # SetMember


def make_probe(path):
    # Equivalent AS1: stop(); var progress=SharedObject.getLocal(name,"/");
    # var savedData=progress.data; trace(all legacy fields); mutate; trace(dirty).
    get_local = (push('/') + push('leaf-shared-avm1') + push(2)
                 + variable('SharedObject') + push('getLocal') + action(0x52))
    script = action(0x07) + push('progress') + get_local + action(0x1d)
    script += push('savedData') + member(variable('progress'), 'data') + action(0x1d)
    data = variable('savedData')
    items = member(data, 'items')
    profile = member(data, 'profile')
    joined = push(',') + push(1) + items + push('join') + action(0x52)
    parts = [member(data, field) for field in ['marker', 'text', 'counter', 'ratio', 'enabled']]
    parts += [joined, member(profile, 'level') + push(',') + action(0x21)
              + member(profile, 'note') + action(0x21)]
    trace = push('AMF0_COMPAT ')
    for index, expression in enumerate(parts):
        if index:
            trace += push('|') + action(0x21)  # StringAdd
        trace += expression + action(0x21)
    script += trace + action(0x26)  # Trace
    script += set_member(data, 'marker', push('migrated-avm1'))
    script += set_member(data, 'counter', member(data, 'counter') + push(1) + action(0x47))
    script += push('new') + push(1) + items + push('push') + action(0x52) + action(0x17)
    script += set_member(profile, 'level', member(profile, 'level') + push(1) + action(0x47))
    script += set_member(profile, 'note', push('persisted'))
    script += push('AMF0_COMPAT DIRTY') + action(0x26) + b'\0'
    write_movie(path, tag(12, script), avm2=False)


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    make_probe(output / 'shared-object-avm1.swf')
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
    cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
    version = json.loads((ROOT / 'adapter/rust-toolchain.lock.json').read_text(encoding='utf8'))['rust_version']
    rust = Path(settings.get('rust', cache / ('rust-' + version)))
    mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
    helper = output / 'old-amf0-writer'
    (helper / 'src').mkdir(parents=True, exist_ok=True)
    (helper / 'Cargo.toml').write_text(
        '[package]\nname="leaf-legacy-amf0-fixture"\nversion="0.0.0"\nedition="2021"\n'
        '[workspace]\n[dependencies]\nflash-lso={git="https://github.com/ruffle-rs/rust-flash-lso",rev="'
        + OLD_WRITER_REV + '"}\n', encoding='utf8')
    (helper / 'src/main.rs').write_text(WRITER_SOURCE, encoding='utf8')
    env = os.environ.copy()
    env.pop('RUSTFLAGS', None)
    env.pop('CARGO_ENCODED_RUSTFLAGS', None)
    env.update(CARGO_HOME=str(cache / 'cargo-home'),
               CARGO_TARGET_DIR=str(ROOT.parent / 'work/ruffle-upgrade-071/old-lso-target'),
               TEMP=str(ROOT / 'build/tmp'), TMP=str(ROOT / 'build/tmp'),
               CARGO_PROFILE_DEV_DEBUG='0')
    env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
    subprocess.run([
        str(rust / 'bin/cargo.exe'), 'run', '--offline', '--target', 'x86_64-pc-windows-gnu',
        '--manifest-path', str(helper / 'Cargo.toml'),
        '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps((ROOT / 'build/host-link.cmd').as_posix()),
        '--', str(output / 'shared-object-amf0-legacy.sol'),
    ], cwd=Path(ROOT.anchor), env=env, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    build(parser.parse_args().output.resolve())
