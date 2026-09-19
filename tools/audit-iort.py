#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only, narrow audit of a revision-0 Yoga IORT (binary or ACPICA DSL).

Accept only a unique RC -> SMMUv3 -> ITS chain with non-single ID mappings.
Do not execute AML or infer hardware transactions from firmware declarations.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct


def require(ok, message):
    if not ok:
        raise ValueError(message)


def raw_table(data):
    if data.startswith(b'IORT'):
        return data
    raw = bytearray()
    for offset, words in re.findall(rb'^\s*([0-9A-Fa-f]+): ((?:[0-9A-Fa-f]{2} ?)+)\s*//', data, re.M):
        require(int(offset, 16) == len(raw), 'Noncontiguous ACPICA raw table')
        raw.extend(bytes.fromhex(words.decode()))
    require(bool(raw), 'No ACPICA raw table')
    return bytes(raw)


def decode(data):
    require(len(data) >= 48 and data[:4] == b'IORT', 'Short/non-IORT table')
    require(struct.unpack_from('<I', data, 4)[0] == len(data), 'Table length mismatch')
    require(data[8] == 0 and sum(data) % 256 == 0, 'Unsupported revision/bad checksum')
    count, offset = struct.unpack_from('<II', data, 36)
    require(0 < count <= (len(data) - 48) // 16 and offset >= 48, 'Invalid node header')
    nodes = {}
    for _ in range(count):
        require(offset + 16 <= len(data), 'Truncated node')
        kind, length, revision, _, mappings, mapoff = struct.unpack_from('<BH BIII', data, offset)
        require(revision == 0 and length >= 16 and offset + length <= len(data), 'Invalid node span/revision')
        require(not mappings or mapoff >= 16 and mapoff + 20 * mappings <= length, 'Invalid mappings span')
        node = {'kind': kind, 'raw': data[offset:offset + length], 'maps': []}
        for i in range(mappings):
            m = struct.unpack_from('<IIIII', data, offset + mapoff + i * 20)
            require(m[0] + m[1] <= 0xffffffff and m[2] + m[1] <= 0xffffffff, 'ID range overflow')
            node['maps'].append(m)
        nodes[offset] = node
        offset += length
    require(offset == len(data), 'Trailing bytes or wrong node count')
    return nodes


def translate(nodes, node, value):
    # IORT ID Count is the range size minus one. Both endpoints are inclusive.
    matches = [m for m in node['maps'] if m[0] <= value <= m[0] + m[1]]
    require(len(matches) == 1, 'Missing/ambiguous mapping')
    start, _, output, reference, flags = matches[0]
    require(flags == 0 and reference in nodes, 'Unsupported flags/dangling reference')
    return nodes[reference], output + value - start


def chain(nodes, segment, rid):
    roots = [n for n in nodes.values() if n['kind'] == 2 and len(n['raw']) >= 36
             and struct.unpack_from('<I', n['raw'], 28)[0] == segment]
    require(len(roots) == 1, 'Missing/duplicate PCI segment')
    smmu, sid = translate(nodes, roots[0], rid)
    require(smmu['kind'] == 4 and len(smmu['raw']) >= 68, 'Not an SMMUv3 chain')
    require(struct.unpack_from('<Q', smmu['raw'], 16)[0] == 0x15400000, 'Wrong PCIe SMMU')
    its, devid = translate(nodes, smmu, sid)
    require(its['kind'] == 0 and len(its['raw']) >= 24 and not its['maps'], 'Not an ITS leaf')
    require(struct.unpack_from('<II', its['raw'], 16) == (1, 0), 'Unexpected ITS identifiers')
    return sid, devid


def calibrate(nodes):
    result = []
    # First calibrate the two hosts with existing EL1 DT ITS routes.
    for segment in (4, 6, 0):
        for rid in (0, 0x100, 0xffff):
            sid, devid = chain(nodes, segment, rid)
            require((sid, devid) == ((segment << 16) + rid, 0x80000 + (segment << 16) + rid),
                    'Calibration differs from pinned X1 DT; do not generate an overlay')
            result.append((segment, rid, sid, devid))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('table', type=Path)
    parser.add_argument('--sha256', required=True, help='Expected hash of input file, before extraction')
    args = parser.parse_args()
    data = args.table.read_bytes()
    require(hashlib.sha256(data).hexdigest() == args.sha256, 'Input provenance hash mismatch')
    for segment, rid, sid, devid in calibrate(decode(raw_table(data))):
        print(f'PCI{segment} RID={rid:04x} SID={sid:05x} ITS_DeviceID={devid:05x}')
    print('Firmware declaration only; not an observation of physical MSI/DMA delivery.')


if __name__ == '__main__':
    main()
