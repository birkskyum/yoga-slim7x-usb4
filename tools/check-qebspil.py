#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the separately pinned EFI derivative; never execute firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--patched', action='store_true')
    args = parser.parse_args()
    record = json.loads((ROOT / 'provenance/qebspil.json').read_text())
    pin = record['qebspil']
    assert hashlib.sha256((ROOT / pin['patch']).read_bytes()).hexdigest() == pin['patch_sha256']
    assert pin['always_start'] == 0
    assert set(record['before']) == set(record['after']) == {
        'src/dtb.c', 'src/event.c', 'src/main.c', 'src/pil.c'}
    if args.source:
        revision = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
        assert revision == pin['revision']
        for name, expected in record['after' if args.patched else 'before'].items():
            assert hashlib.sha256((args.source / name).read_bytes()).hexdigest() == expected, name
    print('PASS qebspil patch provenance' + (' and source fingerprints' if args.source else ''))


if __name__ == '__main__':
    main()
