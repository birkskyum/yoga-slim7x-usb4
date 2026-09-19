#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Firmware-free publication checks, mocks and pinned source reconstruction."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/0001-yoga-x1-v38-diagnostics.patch'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require(value, message):
    if not value:
        raise SystemExit(message)

def check():
    record = json.loads((ROOT / 'provenance/export.json').read_text())
    for item in record['files']:
        require(digest(ROOT / item['path']) == item['public_sha256'], item['path'])
    files = json.loads((ROOT / 'reproduce/kernel-files.json').read_text())
    for name, expected in files['after'].items():
        require(digest(ROOT / 'snapshot/kernel' / name) == expected, name)
    # Apply the public patch to a minimal baseline fixture reconstructed by
    # reversing it from the public snapshot; then require a lossless roundtrip.
    # The independent full-baseline check is performed by `prepare`.
    with tempfile.TemporaryDirectory(prefix='yoga-source-check-') as tmp:
        dest = Path(tmp)
        subprocess.run(['git', 'init', '-q', str(dest)], check=True)
        for name in files['after']:
            target = dest / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'snapshot/kernel' / name, target)
        subprocess.run(['git', '-C', str(dest), 'apply', '--reverse', str(PATCH)], check=True)
        for name, expected in files['before'].items():
            p = dest / name
            require(not p.exists() if expected is None else digest(p) == expected,
                    'Baseline roundtrip mismatch: ' + name)
        subprocess.run(['git', '-C', str(dest), 'apply', str(PATCH)], check=True)
        for name, expected in files['after'].items():
            require(digest(dest / name) == expected, 'Patch roundtrip mismatch: ' + name)
    print(f'PASS source hashes and patch roundtrip ({len(files["after"])} kernel files).', flush=True)

def test():
    check()
    subprocess.run([sys.executable, str(ROOT / 'tests/test_iort.py')], check=True)
    compiler = os.environ.get('CC', 'cc')
    names = ['link', 'enum', 'external', 'event', 'topology', 'policy', 'pan',
             'pdlog', 'receiver', 'its', 'pci0_init', 'nvme_irq_v33', 'public_identity',
             'pcie_environment']
    with tempfile.TemporaryDirectory(prefix='yoga-mocks-') as tmp:
        for name in names:
            executable = str(Path(tmp) / name)
            flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                     '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'snapshot'),
                     '-I' + str(ROOT / 'tests/support')]
            if sys.platform == 'darwin':
                flags += ['-DEREMOTEIO=121', '-DEUCLEAN=117']
            config = ['-DCONFIG_USB4_X1_NATIVE=1'] if name == 'nvme_irq_v33' else []
            subprocess.run([compiler, *flags, *config, str(ROOT / 'tests' / f'test_{name}.c'), '-o', executable], check=True)
            subprocess.run([executable], check=True)
        executable = str(Path(tmp) / 'disabled_nvme')
        subprocess.run([compiler, *flags, '-DCONFIG_USB4_X1_NATIVE=0',
                        str(ROOT / 'tests/test_nvme_irq_v33.c'), '-o', executable], check=True)
        subprocess.run([executable], check=True)
        executable = str(Path(tmp) / 'synthetic_identity')
        subprocess.run([compiler, *flags, '-DX1_NATIVE_LACIE_UID=0x123456789abcdef0ULL',
                        str(ROOT / 'tests/test_public_identity.c'), '-o', executable], check=True)
        subprocess.run([executable], check=True)
    print('PASS 16 mock executions under ASan/UBSan; no hardware access.', flush=True)

def prepare(baseline):
    check()
    baseline = Path(baseline).resolve()
    dest = ROOT / 'build-output/kernel'
    require(baseline.is_dir() and not dest.exists(), 'Need a baseline directory and absent output directory.')
    require(dest.resolve() != baseline and baseline not in dest.resolve().parents,
            'Output cannot be inside the baseline.')
    for line in (ROOT / 'reproduce/baseline-source.sha256').read_text().splitlines():
        expected, name = line.split(maxsplit=1)
        require(digest(baseline / name) == expected, 'Pinned baseline mismatch: ' + name)
    files = json.loads((ROOT / 'reproduce/kernel-files.json').read_text())
    for name, expected in files['before'].items():
        p = baseline / name
        require(not p.exists() if expected is None else digest(p) == expected, 'Baseline mismatch: ' + name)
    shutil.copytree(baseline, dest, symlinks=True, ignore=shutil.ignore_patterns('.git', '.config', '.config.old'))
    subprocess.run(['git', 'init', '-q', str(dest)], check=True)
    subprocess.run(['git', '-C', str(dest), 'apply', '--check', str(PATCH)], check=True)
    subprocess.run(['git', '-C', str(dest), 'apply', str(PATCH)], check=True)
    for name, expected in files['after'].items():
        require(digest(dest / name) == expected, 'Output mismatch: ' + name)
    print('PASS pinned baseline and reconstructed Yoga source:', dest, flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'test', 'prepare'])
    parser.add_argument('--baseline')
    args = parser.parse_args()
    if args.action == 'prepare':
        require(args.baseline, '--baseline is required')
        prepare(args.baseline)
    elif args.action == 'test':
        test()
    else:
        check()
