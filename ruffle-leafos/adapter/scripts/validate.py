from pathlib import Path
import argparse
import json
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--pak', required=True, type=Path)
parser.add_argument('--contract', required=True, type=Path)
args = parser.parse_args()
root = args.contract.resolve(); pak = args.pak.resolve()
sys.path[:0] = [str(root/'contracts'/'leaf-content'/'scripts'), str(root/'contracts'/'leaf-services'/'scripts')]
import content_model
import minischema

manifest = json.loads((pak/'pak.json').read_text(encoding='utf8'))
schema = json.loads((root/'contracts'/'leaf-content'/'content-paks-v1.schema.json').read_text(encoding='utf8'))
valid, reason = minischema.is_valid(manifest, schema)
if not valid: raise SystemExit(f'FAIL schema: {reason}')
violations = content_model.validate_manifest(manifest, str(pak), {'install_lane':'platform', 'source_id':'primary'})
if violations: raise SystemExit(f'FAIL contract: {sorted(violations)}')
assert (pak/'launch.sh').is_file(), 'hybrid probe needs an Apps entry'
assert manifest['type'] == 'TOOL'
assert (pak/manifest['icon']).is_file()
assert manifest['provides']['systems'][0]['rom_root'] == 'Roms/flash'
assert manifest['provides']['systems'][0]['patterns'] == ['flash']
for rel in ['emulators/ruffle-probe/bin/ruffle', 'emulators/ruffle-probe/lib/libruffle_probe.so', 'emulators/ruffle-probe/res/menu-font.ttf', 'emulators/ruffle-probe/res/Leaf-GPU-Probe.swf']:
    assert (pak/rel).is_file(), rel
for rel in ['launch.sh', 'collect-diagnostics.sh', 'emulators/ruffle-probe/launch-game.sh', 'emulators/ruffle-probe/paths.sh', 'emulators/ruffle-probe/diagnostics.sh']:
    assert b'\r' not in (pak/rel).read_bytes(), 'shell wrapper must use LF: '+rel
print('PASS: official Leaf CONTENT-1 schema, paths, executable entries, payload, hybrid Apps/content pak')

assert manifest['provides']['cores'][0]['id'] == 'ruffle_probe'
assert manifest['provides']['cores'][0]['supports_menu'] is False
assert manifest['provides']['cores'][0]['supports_savestate'] is False
assert manifest['provides']['systems'][0]['id'] == 'FLASH'
