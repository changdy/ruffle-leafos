"""Rebuild the original SWF and legacy AMF3 storage compatibility fixtures.

The legacy writer is the exact flash-lso Git revision used by the old fork.
It runs in an isolated Cargo project/target, never replacing the player lock.
Generated helper sources, ABC and compiler output remain in ignored work trees;
only the small SWF/SOL test fixtures are checked in under core/src/player/tests.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import struct
import subprocess

from make_keyboard_fixtures import ROOT, playerglobal_abc, tag, write_movie


OLD_WRITER_REV = '998f47c926b9986aabd518fbb7394ff56936d0b0'
WRITER_SOURCE = r'''
use flash_lso::types::{AMFVersion, Attribute, ClassDefinition, Element, Lso, ObjectId, Value};
use std::rc::Rc;

fn main() {
    let values = vec![
        ("marker", Value::String("legacy-v0.8.3".into())),
        ("text", Value::String("旧档✓".into())),
        ("counter", Value::Integer(41)),
        ("ratio", Value::Number(1.25)),
        ("enabled", Value::Bool(true)),
        ("items", Value::ECMAArray(ObjectId::INVALID, vec![
            Rc::new(Value::Integer(1)), Rc::new(Value::String("two".into())),
            Rc::new(Value::Bool(false)),
        ], Vec::new(), 3)),
        ("profile", Value::Object(ObjectId::INVALID, vec![
            Element::new("level", Rc::new(Value::Integer(3))),
            Element::new("note", Rc::new(Value::String("nested".into()))),
        ], Some(ClassDefinition {
            name: String::new(),
            attributes: Attribute::Dynamic.into(),
            static_properties: Vec::new(),
        }))),
    ];
    let body = values.into_iter().map(|(name, value)| Element::new(name, Rc::new(value))).collect();
    let mut lso = Lso::new(body, "leaf-shared-compat", AMFVersion::AMF3);
    let bytes = flash_lso::write::write_to_bytes(&mut lso).expect("legacy writer failed");
    std::fs::write(std::env::args_os().nth(1).expect("output .sol path"), bytes).unwrap();
}
'''


def build(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    settings_path = ROOT / 'workspace.local.json'
    settings = json.loads(settings_path.read_text(encoding='utf8')) if settings_path.exists() else {}
    cache = Path(settings.get('host_tool_cache', ROOT.parent / 'work/toolchains'))
    rust_version = json.loads((ROOT / 'adapter/rust-toolchain.lock.json').read_text(encoding='utf8'))['rust_version']
    rust = Path(settings.get('rust', cache / ('rust-' + rust_version)))
    mingw = Path(settings.get('host_gcc', cache / 'w64devkit'))
    jdk = Path(settings.get('jdk', 'C:/Users/changdy/.jdks/azul-17.0.19-1'))
    source = output / 'SharedObjectCompatProbe.as'
    shutil.copyfile(ROOT / 'scripts/resources' / source.name, source)
    (output / 'playerglobal.abc').write_bytes(playerglobal_abc())
    abc = source.with_suffix('.abc')
    abc.unlink(missing_ok=True)
    result = subprocess.run([
        str(jdk / 'bin/java.exe'), '-classpath', str(ROOT / 'player/tools/asc/asc.jar'),
        'macromedia.asc.embedding.ScriptCompiler', '-AS3', '-import', str(output / 'playerglobal.abc'),
        '-outdir', str(output), str(source),
    ], capture_output=True, text=True)
    if result.returncode or not abc.is_file() or '[Compiler] Error' in result.stdout + result.stderr:
        raise RuntimeError(result.stdout + result.stderr)
    content = tag(82, struct.pack('<I', 0) + b'shared-object-compat\0' + abc.read_bytes())
    content += tag(76, struct.pack('<HH', 1, 0) + b'SharedObjectCompatProbe\0')
    write_movie(output / 'shared-object-compat.swf', content, avm2=True)

    helper = output / 'legacy-writer'
    (helper / 'src').mkdir(parents=True, exist_ok=True)
    (helper / 'Cargo.toml').write_text(
        '[package]\nname = "leaf-legacy-lso-fixture"\nversion = "0.0.0"\nedition = "2021"\n'
        '[workspace]\n[dependencies]\nflash-lso = { git = "https://github.com/ruffle-rs/rust-flash-lso", '
        'rev = "' + OLD_WRITER_REV + '" }\n', encoding='utf8')
    (helper / 'src/main.rs').write_text(WRITER_SOURCE, encoding='utf8')
    env = os.environ.copy()
    env.pop('RUSTFLAGS', None)
    env.pop('CARGO_ENCODED_RUSTFLAGS', None)
    env.update(
        CARGO_HOME=str(cache / 'cargo-home'),
        CARGO_TARGET_DIR=str(ROOT.parent / 'work/ruffle-upgrade-071/old-lso-target'),
        TEMP=str(ROOT / 'build/tmp'), TMP=str(ROOT / 'build/tmp'),
        CARGO_PROFILE_DEV_DEBUG='0',
    )
    env['PATH'] = os.pathsep.join([str(rust / 'bin'), str(mingw / 'bin'), env['PATH']])
    subprocess.run([
        str(rust / 'bin/cargo.exe'), 'run', '--offline', '--target', 'x86_64-pc-windows-gnu',
        '--manifest-path', str(helper / 'Cargo.toml'),
        '--config', 'target.x86_64-pc-windows-gnu.linker=' + json.dumps((ROOT / 'build/host-link.cmd').as_posix()),
        '--', str(output / 'shared-object-legacy.sol'),
    ], cwd=Path(ROOT.anchor), env=env, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    build(parser.parse_args().output.resolve())
