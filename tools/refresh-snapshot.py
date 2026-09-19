#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Mechanically regenerate the aggregate from reviewed snapshot edits.

Derive the baseline by reversing HEAD's patch against HEAD's snapshot, and
require the existing independent 'before' hashes. Never reset those hashes.
New snapshot files must be named explicitly. Review the diff before commit.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = 'patches/0001-yoga-x1-v38-diagnostics.patch'


def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', '-C', str(cwd), *args])


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--new-file', action='append', default=[])
    args = parser.parse_args()
    old = json.loads(git('show', 'HEAD:reproduce/kernel-files.json'))
    record = json.loads(git('show', 'HEAD:provenance/export.json'))
    names = list(old['after'])
    for name in args.new_file:
        p = Path(name)
        if (p.is_absolute() or '..' in p.parts or
                p.parts[:2] not in [('snapshot', 'kernel'), ('snapshot', 'runtime')]):
            raise SystemExit('Not an allowed snapshot path: ' + name)
        if any(item['path'] == name for item in record['files']):
            raise SystemExit('Already recorded: ' + name)
        if name.startswith('snapshot/kernel/'):
            relative = name.removeprefix('snapshot/kernel/')
            names.append(relative)
            old['before'][relative] = None
        record['files'].append({'path': name, 'private_source_sha256': None,
                               'origin': 'EL2 source-only preparation',
                               'changed_for_publication': False})
    with tempfile.TemporaryDirectory(prefix='yoga-patch-derive-') as tmp:
        base = Path(tmp)
        git('init', '-q', str(base))
        for name in old['after']:
            p = base / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(git('show', 'HEAD:snapshot/kernel/' + name))
        subprocess.run(['git', '-C', str(base), 'apply', '--reverse', '-'],
                       input=git('show', 'HEAD:' + PATCH), check=True)
        for name, expected in old['before'].items():
            p = base / name
            if (p.exists() if expected is None else
                    not p.exists() or sha(p.read_bytes()) != expected):
                raise SystemExit('Independent baseline mismatch: ' + name)
        after = {}
        pieces = []
        for name in names:
            p = base / name
            before = p.read_text().splitlines(keepends=True) if p.exists() else []
            data = (ROOT / 'snapshot/kernel' / name).read_bytes()
            after[name] = sha(data)
            pieces.extend(difflib.unified_diff(before, data.decode().splitlines(keepends=True),
                          fromfile='a/' + name if before else '/dev/null', tofile='b/' + name))
        patch = ''.join(pieces).encode()
    for item in record['files']:
        digest = sha((ROOT / item['path']).read_bytes())
        if 'public_sha256' in item and digest != item['public_sha256']:
            if item.get('private_source_sha256') is None:
                # Newly authored files were not part of the v38 export.
                item.setdefault('initial_source_sha256', item['public_sha256'])
                item['changed_since_initial_source'] = True
            else:
                item.setdefault('v38_export_sha256', item['public_sha256'])
                item['changed_since_v38_export'] = True
        item['public_sha256'] = digest
    record['source_revision_note'] = 'EL2 source-only preparation; private v38 hashes retained'
    (ROOT / PATCH).write_bytes(patch)
    (ROOT / 'reproduce/kernel-files.json').write_text(
        json.dumps({'before': old['before'], 'after': after}, indent=2) + '\n')
    (ROOT / 'provenance/export.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Regenerated {len(after)}-file aggregate; baseline fingerprints unchanged.')


if __name__ == '__main__':
    main()
