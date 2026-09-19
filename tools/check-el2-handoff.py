#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline validation of the no-peripheral handoff DT and optional local firmware."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dtcheck', ROOT / 'tools/check-el2-dt.py')
dt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dt)


def validate(tree):
    dt.check_non_pas(tree)
    props = dt.run('fdtget', '-p', tree, '/').decode().split()
    assert 'birk,adsp-el2-service-test' in props
    assert not any(p.startswith('birk,usb4') for p in props)
    assert dt.get(tree, '/', 'model') == 'Lenovo Yoga Slim 7x'
    for label in ('pcie3', 'pcie4', 'pcie5', 'pcie6a', 'remoteproc_cdsp',
                  'gpu', 'gpu_zap_shader', 'iris', 'mdss', 'sbsa_watchdog',
                  'usb_1_ss0', 'usb_1_ss1', 'usb_1_ss2'):
        assert dt.get(tree, dt.get(tree, '/__symbols__', label), 'status') == 'disabled', label
    for label in ('apss_watchdog', 'pcie_smmu', 'remoteproc_adsp'):
        assert dt.get(tree, dt.get(tree, '/__symbols__', label), 'status') == 'okay', label
    adsp = dt.get(tree, '/__symbols__', 'remoteproc_adsp')
    assert adsp == '/soc@0/remoteproc@6800000'
    assert 'qcom,broken-reset' in dt.run('fdtget', '-p', tree, adsp).decode().split()
    assert dt.get(tree, adsp + '/glink-edge/fastrpc', 'status') == 'disabled'
    assert dt.get(tree, adsp, 'firmware-name').split() == [
        'qcom/x1e80100/LENOVO/83ED/qcadsp8380.mbn',
        'qcom/x1e80100/LENOVO/83ED/adsp_dtbs.elf']
    for label in ('i2c1', 'i2c3', 'i2c7'):
        assert dt.get(tree, dt.get(tree, '/__symbols__', label) + '/typec-mux@8', 'status') == 'disabled'
    soc = dt.run('fdtget', '-l', tree, '/soc@0').decode().split()
    assert not any('usb4-test' in n or 'usb4-mcu-test' in n for n in soc)


def firmware(path, expected, capacity):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == expected, path.name + ' hash'
    assert data[:7] == b'\x7fELF\x01\x01\x01', path.name + ' ELF32 little endian'
    hdr = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    off, entsize, count = hdr[5], hdr[9], hdr[10]
    assert entsize == 32 and off + count * entsize <= len(data)
    loads, metadata = [], 0
    for i in range(count):
        typ, offset, va, pa, filesz, memsz, flags, align = struct.unpack_from('<8I', data, off + i * entsize)
        assert not filesz or offset + filesz <= len(data)
        if typ == 0:
            metadata += filesz
        if typ == 1:
            assert filesz <= memsz and pa + memsz <= 0xffffffff
            loads.append((pa, pa + memsz))
    assert loads and 0 < metadata <= 4 * 4096
    assert max(b for a, b in loads) - min(a for a, b in loads) <= capacity
    print('PASS local firmware hash, ELF and reserved-region capacity:', path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--firmware-dir', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='yoga-handoff-dt-') as tmp:
        work = Path(tmp)
        trees = []
        for i, source in enumerate((args.kernel / 'arch/arm64/boot/dts/qcom/x1e80100-lenovo-yoga-slim7x.dts', ROOT / 'snapshot/runtime/yoga-adsp-el2.dtso')):
            pre, tree = work / f'{i}.dts', work / f'{i}.dtb'
            pre.write_bytes(dt.run(os.environ.get('CPP_CC', 'cc'), '-E', '-nostdinc', '-undef',
                '-D__DTS__', '-x', 'assembler-with-cpp', '-I' + str(args.kernel / 'include'),
                '-I' + str(args.kernel / 'scripts/dtc/include-prefixes'), source))
            dt.run('dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', tree, pre)
            trees.append(tree)
        out = work / 'handoff.dtb'
        dt.run('fdtoverlay', '-i', trees[0], '-o', out, trees[1])
        validate(out)
        for node, prop, val in ((dt.get(out, '/__symbols__', 'pcie6a'), 'status', 'okay'),
                               (dt.get(out, '/__symbols__', 'remoteproc_cdsp'), 'status', 'okay')):
            bad = work / 'bad.dtb'
            shutil.copyfile(out, bad)
            dt.run('fdtput', '-t', 's', bad, node, prop, val)
            try:
                validate(bad)
            except AssertionError:
                pass
            else:
                raise AssertionError('Unsafe DT accepted')
        if args.output:
            if args.output.exists():
                raise SystemExit('Output already exists; preserve it and choose a new path')
            shutil.copyfile(out, args.output)
    if args.firmware_dir:
        firmware(args.firmware_dir / 'qcadsp8380.mbn', '0497f837f1d3b1788cbdb1fad665eea1e42fd5ad3a41fabffd421e5df7b9a1fc', 0x3a00000)
        firmware(args.firmware_dir / 'adsp_dtbs.elf', '5556fa684f96b203c330ba0bc2daddedeb5c358ff423b3d190e74ddfcde8b9ad', 0x80000)
    print('PASS service-only DT and negative PCIe/CDSP tests. No hardware access.')


if __name__ == '__main__':
    main()
