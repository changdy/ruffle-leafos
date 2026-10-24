"""Exercise CONTENT-ART-1 fixtures, including real files and unchanged CAT-1."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

import art_model
import canonical
import catalog_model
import content_model
import minischema

# A complete 1x1 PNG; full decode/fallback behavior belongs to the UI tests.
PNG = base64.b64decode(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=')


def run(root, fail, version=1):
    root = Path(root)
    document = json.loads((root / ('art/fixtures.json' if version == 1 else 'art/grid-fixtures.json')).read_text())
    schema = json.loads((root / f'content-art-v{version}.schema.json').read_text())
    old_schema = json.loads((root / 'content-paks-v1.schema.json').read_text())
    stamp_schema = json.loads((root / 'effective-catalog-v1.schema.json').read_text())

    def check(condition, label):
        if not condition:
            fail('art/' + label)

    with tempfile.TemporaryDirectory(prefix='content-art-') as directory:
        pak_dir = Path(directory) / 'pak'
        (pak_dir / 'art').mkdir(parents=True)
        (pak_dir / 'art/mark.png').write_bytes(PNG)
        (pak_dir / 'art/bad.png').write_text('not a PNG')
        (pak_dir / 'art/grid.png').write_bytes(PNG)
        (pak_dir / 'art/color.png').write_bytes(PNG)
        for name, width in [('large', 1025), ('zero', 0)]:
            (pak_dir / f'art/{name}.png').write_bytes(PNG[:16] + width.to_bytes(4, 'big') + PNG[20:])
        (pak_dir / 'art/header.png').write_bytes(art_model.PNG_SIGNATURE)
        (pak_dir / 'escape').symlink_to(Path(directory), target_is_directory=True)
        (pak_dir / 'run.sh').write_text('#!/bin/sh\nexit 0\n')
        (pak_dir / 'run.sh').chmod(0o755)
        reasons = set()
        for case in document['cases']:
            pak = copy.deepcopy(document['paks']['owner'])
            pak.pop('content_art')
            if 'content_art' in case:
                pak['content_art'] = case['content_art']
            expected = set() if case.get('valid') else {case['reason']}
            if expected and version == 1:
                check(case['reason'] not in reasons, 'duplicate reason ' + case['reason'])
                reasons.update(expected)
            if case.get('setup') == 'unreadable':
                with patch('builtins.open', side_effect=PermissionError):
                    actual = art_model.validate(pak, str(pak_dir), max_schema=version)
            else:
                actual = art_model.validate(pak, str(pak_dir), max_schema=version)
            check(actual == expected, f"{case['name']}: {actual} != {expected}")
            shape_ok = minischema.is_valid(pak, schema)[0]
            if version == 2:
                check(not minischema.is_valid(pak, json.loads((root / 'content-art-v1.schema.json').read_text()))[0], case['name'] + ': old art schema accepted v2')
            filesystem_only = case.get('reason', '').startswith('content-art-') or \
                case.get('reason') in {'duplicate-content-art-system',
                                      'unsupported-content-art-image',
                                      'unreadable-content-art-image', 'invalid-content-art-grid-dimensions',
                                      'invalid-content-art-wordmark-dimensions'}
            check(shape_ok == (not expected or filesystem_only), case['name'] + ': schema')
            check(minischema.is_valid(pak, old_schema)[0] and
                  not content_model.validate_manifest(pak, str(pak_dir)),
                  case['name'] + ': optional art changed CONTENT-1 acceptance')

        # Boundary/type variants share a reason; keep one named invalid fixture per rule.
        for value in (True, 1.5, '1', None):
            pak = copy.deepcopy(document['paks']['owner'])
            pak['content_art']['schema'] = value
            check(art_model.validate(pak, str(pak_dir), max_schema=version) == {'unknown-content-art-schema'}
                  and not minischema.is_valid(pak, schema)[0], f'schema type {value!r}')
        # JSON number spellings with the same value are the same schema version.
        pak = copy.deepcopy(document['paks']['owner'])
        pak['content_art']['schema'] = float(version)
        check(not art_model.validate(pak, str(pak_dir), max_schema=version)
              and minischema.is_valid(pak, schema)[0], 'numeric schema spelling')
        for value in ('a\0.png', '', 'a' * 4097, False):
            pak = copy.deepcopy(document['paks']['owner'])
            pak['content_art']['schema'] = version
            pak['content_art']['systems'][0]['wordmark'] = value
            check(art_model.validate(pak, str(pak_dir), max_schema=version) == {'malformed-content-art-wordmark'}
                  and not minischema.is_valid(pak, schema)[0], 'wordmark string bounds')
            if version == 2:
                pak['content_art']['systems'][0]['wordmark'] = 'art/mark.png'
                pak['content_art']['systems'][0]['wordmark_color'] = value
                check(art_model.validate(pak, str(pak_dir), max_schema=version) == {'malformed-content-art-wordmark-color'}
                      and not minischema.is_valid(pak, schema)[0], 'wordmark_color string bounds')
        # Every wordmark shares the grid icon's header read; boundary variants beyond
        # the named fixture keep its reason. Dimensions are not a JSON Schema concern.
        for image, reason in (('large', 'invalid-content-art-wordmark-dimensions'),
                              ('zero', 'invalid-content-art-wordmark-dimensions'),
                              ('header', 'unsupported-content-art-image')):
            pak = copy.deepcopy(document['paks']['owner'])
            pak['content_art'] = dict(schema=version, systems=[dict(id='SCUMMVM', wordmark=f'art/{image}.png')])
            check(art_model.validate(pak, str(pak_dir), max_schema=version) == {reason}
                  and minischema.is_valid(pak, schema)[0], f'wordmark header {image}')

        for case in document['generation_cases']:
            contributors = [dict(provider=f'mlp1/{name}.pak',
                                 pak=copy.deepcopy(document['paks'][name]),
                                 pak_dir=str(pak_dir)) for name in case['contributors']]
            for c in contributors:
                check(not content_model.validate_manifest(c['pak'], str(pak_dir)),
                      case['name'] + ': invalid CONTENT-1 fixture')
            merged, _ = content_model.merge(document['base'], [
                dict(provider=c['provider'], provides=c['pak']['provides'])
                for c in contributors])
            before = copy.deepcopy(merged)
            output, files, diagnostics = art_model.decorate(merged, contributors)
            expected = copy.deepcopy(merged)
            expected_files = {}
            for system in expected['systems']:
                for slot, field, rel in [('wordmark', 'applied', 'art/mark.png'),
                                         ('wordmark_color', 'color_applied', 'art/color.png'),
                                         ('grid_icon', 'grid_applied', 'art/grid.png')]:
                    name = case.get(field, {}).get(system['id'])
                    if name:
                        provider = f'mlp1/{name}.pak'
                        system[slot] = rel
                        system[slot + '_provider'] = provider
                        expected_files.setdefault(provider, set()).update([rel, 'pak.json'])
            expected_files = {p: sorted(files) for p, files in expected_files.items()}
            expected_diags = sorted([
                dict(provider=f'mlp1/{p}.pak', reason=r, detail=d)
                for p, r, d in case['diagnostics']],
                key=lambda d: (d['provider'], d['reason'], d['detail']))
            check(output == expected and merged == before,
                  case['name'] + ': catalog/ownership/core changed')
            check(files == expected_files, case['name'] + ': fingerprint paths')
            check(diagnostics == expected_diags, case['name'] + ': diagnostics')
            check(art_model.decorate(merged, list(reversed(contributors))) ==
                  (output, files, diagnostics), case['name'] + ': order dependence')
            if case['name'] != 'release-extension':
                continue

            # Use the existing CAT-1 stamp and structural/provenance reference readers.
            state = json.loads((root / 'generations/fixtures.json').read_text())['resolution'][0]['state']
            stamp = copy.deepcopy(next(iter(state['generations'].values()))['stamp'])
            provider = contributors[0]['provider']
            pak = contributors[0]['pak']
            (pak_dir / 'pak.json').write_bytes(canonical.canonical_bytes(pak))
            stamp['contributors'] = [dict(
                provider=provider, source_id='primary', pak_version='1.0.0',
                provides_sha256=canonical.canonical_sha256(pak['provides']),
                files=[dict(rel=rel, sha256=hashlib.sha256((pak_dir / rel).read_bytes()).hexdigest())
                       for rel in files[provider]])]
            stamp['output']['systems_sha256'] = canonical.canonical_sha256(
                {'systems': output['systems']})
            check(stamp['output']['systems_sha256'] != canonical.canonical_sha256(
                {'systems': merged['systems']}), 'decorated systems digest')
            check(minischema.is_valid(stamp, stamp_schema)[0], 'unchanged CAT-1 schema')
            name = canonical.generation_name(stamp)
            state['current'] = name + '\n'
            state['generations'] = {name: dict(stamp=stamp)}
            check(catalog_model.resolve(state)['resolution'] == 'generation',
                  'old structural reader')
            state['on_disk'] = dict(contributors=copy.deepcopy(stamp['contributors']))
            check(catalog_model.validate_provenance(state)['action'] == 'none',
                  'original provenance')
            changed = copy.deepcopy(stamp)
            changed['contributors'][0]['files'][0]['sha256'] = hashlib.sha256(PNG + b'changed').hexdigest()
            check(canonical.generation_name(changed) != name,
                  'same path asset replacement must change generation')
            state['on_disk']['contributors'] = changed['contributors']
            result = catalog_model.validate_provenance(state)
            check(result['reason'] == 'stamp-contributor-mismatch' and
                  result['invalidate_selector_first'], 'replacement invalidation')
            # Move the same inputs to another install root; serialized results stay identical.
            relocated = Path(directory) / 'relocated'
            (relocated / 'art').mkdir(parents=True)
            (relocated / 'art/mark.png').write_bytes(PNG)
            moved = [dict(c, pak_dir=str(relocated)) for c in contributors]
            check(art_model.decorate(merged, moved) == (output, files, diagnostics),
                  'root relocation')

    print(f"CONTENT-ART-{version}: {len(document['cases'])} validation cases, "
          f"{len(document['generation_cases'])} merge cases, compatibility and freshness checks")
