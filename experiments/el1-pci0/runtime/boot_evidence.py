"""Private immutable EL1 boot proof, separate from the rolling fault log.

Only root's current-boot kernel journal supplies this receipt. Never synthesize
a dmesg line, accept a prior boot, or relax the current I/O/fault classifier.
The baseline classifier below is the pinned dev13 implementation with only
its boot-message check replaced; an AST regression checks that derivation.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def require(ok, message):
    if not ok: raise ValueError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def validate_records(records, boot):
    require(re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot) is not None,
            'Invalid current boot ID')
    require(type(records) is list and len(records) == 1, 'Missing/ambiguous kernel boot evidence')
    record = records[0]
    require(type(record) is dict and record.get('_BOOT_ID') == boot.replace('-', '') and
            record.get('_TRANSPORT') == 'kernel' and
            record.get('MESSAGE') == 'CPU: All CPU(s) started at EL1' and
            type(record.get('__MONOTONIC_TIMESTAMP')) is str and
            re.fullmatch('[0-9]+', record['__MONOTONIC_TIMESTAMP']) is not None,
            'Current-boot kernel EL1 evidence not proven')


def collect(boot, release):
    require(os.geteuid() == 0, 'Root journal reader required')
    # journalctl's boot descriptor requires the compact sd-id128 form.
    command = ['/usr/bin/journalctl', '--boot=' + boot.replace('-', ''), '--dmesg', '--no-pager',
               '--output=json', '--grep=CPU:.*started at EL']
    raw = subprocess.check_output(command, text=True, timeout=10)
    require(0 < len(raw) <= 65536, 'Missing/oversized boot evidence')
    records = [json.loads(line) for line in raw.splitlines() if line.strip()]
    validate_records(records, boot)
    require(Path('/proc/sys/kernel/random/boot_id').read_text().strip() == boot and
            os.uname().release == release, 'Boot changed while collecting evidence')
    return dict(version=1, boot_id=boot, release=release, source='current-boot-kernel-journal',
                records=records, records_sha256=digest(records))


def validate(proof, boot, release):
    require(type(proof) is dict and proof.get('version') == 1 and
            proof.get('source') == 'current-boot-kernel-journal' and
            proof.get('boot_id') == boot and proof.get('release') == release,
            'Wrong boot evidence receipt')
    records = proof.get('records')
    validate_records(records, boot)
    require(proof.get('records_sha256') == digest(records), 'Boot evidence receipt changed')


def save(path, proof):
    # Caller admits the root-owned private directory before this exclusive write.
    with path.open('x') as stream:
        os.fchmod(stream.fileno(), 0o600)
        json.dump(proof, stream, indent=2); stream.write('\n')
        stream.flush(); os.fsync(stream.fileno())


def classify_baseline(log, target, snapshot, boot_id, *, proof, release, settled, faults):
    """Same pre-arm and fault guards as dev13; boot evidence is not a log line."""
    settled(snapshot)
    w = snapshot['write']
    require(w['state'] == '0' and w['terminal'] == '0' and w['enabled'] == '1' and
            all(w[k] == '0' for k in ('write_submitted', 'write_completed', 'flush_submitted', 'flush_completed')),
            'Uncertain pre-arm baseline')
    validate(proof, boot_id, release)
    require('CPU: All CPU(s) started at EL2' not in log, 'Conflicting EL2 evidence')
    expression = re.compile(r'^\[\s*\d+\.\d{6}\] I/O error, dev ' + re.escape(target['devices'][0].name) +
        r', sector 0 op 0x1:\(WRITE\) flags 0x800 phys_seg 0 prio class 2$')
    errors = [line for line in log.splitlines() if faults.search(line)]
    require(len(errors) <= 8 and len(errors) == len(set(errors)) and all(expression.fullmatch(line) for line in errors),
            'Unclassified baseline fault')
    return dict(boot_id=boot_id, target=str(target['devices'][0]), classified=errors,
                kernel_log=log, sha256=hashlib.sha256(log.encode()).hexdigest(), evidence=snapshot,
                classification='closed-state software refusal; origin untraced; no future exemption')
