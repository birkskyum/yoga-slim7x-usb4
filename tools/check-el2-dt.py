#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile/compose candidate DTs in a temporary directory, never install them.

Requires a reconstructed pinned kernel, a C preprocessor, dtc, fdtoverlay,
fdtget and fdtput. Checks the composed DT, not a regex over overlay sources.
This is source validation only: it cannot check firmware or CPU boot level.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output([str(arg) for arg in args])


def get(tree, node, prop, kind='s'):
    return run('fdtget', '-t' + kind, tree, node, prop).decode().strip()


def cells(tree, node, prop):
    return [int(word, 16) for word in get(tree, node, prop, 'x').split()]


def check_non_pas(tree):
    """Reject the DSP mapping that faulted on this Yoga's earlier EL2 boot."""
    for label in ('remoteproc_adsp', 'remoteproc_cdsp'):
        node = get(tree, '/__symbols__', label)
        if 'iommus' in run('fdtget', '-p', tree, node).decode().split():
            raise ValueError(f'{label}: PAS iommus present; not the tested non-PAS policy')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'build-output/kernel')
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    with tempfile.TemporaryDirectory(prefix='yoga-el2-dt-') as tmp:
        work = Path(tmp)
        sources = [kernel / 'arch/arm64/boot/dts/qcom/x1e80100-lenovo-yoga-slim7x.dts',
                   ROOT / 'snapshot/runtime/yoga-mcu.dtso',
                   ROOT / 'snapshot/runtime/yoga-pci0-el2.dtso']
        trees = []
        for index, source in enumerate(sources):
            pre = work / f'{index}.dts'
            tree = work / f'{index}.dtb'
            pre.write_bytes(run(os.environ.get('CPP_CC', 'cc'), '-E', '-nostdinc', '-undef',
                                '-D__DTS__', '-x', 'assembler-with-cpp',
                                '-I' + str(kernel / 'include'),
                                '-I' + str(kernel / 'scripts/dtc/include-prefixes'), source))
            run('dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', tree, pre)
            trees.append(tree)
        el1, el2 = work / 'el1.dtb', work / 'el2.dtb'
        run('fdtoverlay', '-i', trees[0], '-o', el1, trees[1])
        run('fdtoverlay', '-i', el1, '-o', el2, trees[2])
        pci = '/soc@0/pcie-usb4-test@400000000'
        smmu = get(el2, '/__symbols__', 'pcie_smmu')
        its = get(el2, '/__symbols__', 'gic_its')
        assert smmu == '/soc@0/iommu@15400000'
        assert get(el1, smmu, 'status') == 'reserved'
        assert get(el2, smmu, 'status') == 'okay'
        smmu_handle = cells(el2, smmu, 'phandle')[0]
        its_handle = cells(el2, its, 'phandle')[0]
        assert cells(el2, pci, 'iommu-map') == [0, smmu_handle, 0, 0x10000]
        assert cells(el2, pci, 'msi-map') == [0, its_handle, 0x80000, 0x10000]
        assert cells(el1, pci, 'msi-map') == cells(el2, pci, 'msi-map')
        assert 'iommu-map' not in run('fdtget', '-p', el1, pci).decode().split()
        for prop in ('iommu-map-mask', 'msi-map-mask'):
            assert prop not in run('fdtget', '-p', el2, pci).decode().split()
        marker = 'birk,usb4-pci0-el2-test'
        assert marker not in run('fdtget', '-p', el1, '/').decode().split()
        assert marker in run('fdtget', '-p', el2, '/').decode().split()
        for label in ('pcie3', 'pcie4', 'pcie5', 'pcie6a', 'remoteproc_cdsp',
                      'gpu_zap_shader', 'iris', 'sbsa_watchdog'):
            assert get(el2, get(el2, '/__symbols__', label), 'status') == 'disabled'
        assert get(el2, get(el2, '/__symbols__', 'apss_watchdog'), 'status') == 'okay'
        for tree in (trees[0], el1, el2):
            check_non_pas(tree)
        # An accidental PAS overlay/input must fail even for disabled CDSP.
        apps = get(el2, '/__symbols__', 'apps_smmu')
        apps_handle = cells(el2, apps, 'phandle')[0]
        for label, sid, mask in (('remoteproc_adsp', 0x1000, 0x80),
                                 ('remoteproc_cdsp', 0xc00, 0)):
            bad_input, bad_result = work / 'pas-input.dtb', work / 'pas-result.dtb'
            shutil.copyfile(el1, bad_input)
            node = get(el2, '/__symbols__', label)
            run('fdtput', '-t', 'x', bad_input, node, 'iommus',
                f'{apps_handle:x}', f'{sid:x}', f'{mask:x}')
            run('fdtoverlay', '-i', bad_input, '-o', bad_result, trees[2])
            try:
                check_non_pas(bad_result)
            except ValueError as error:
                assert str(error).startswith(label + ':')
            else:
                raise AssertionError(f'PAS input escaped validation: {label}')
        # Each DT INTx cell triplet is an SPI number, not an MSI parent IRQ.
        irqs = cells(el2, pci, 'interrupt-map')
        assert [irqs[i + 8] for i in range(0, len(irqs), 10)] == [703, 708, 714, 716]
    print('PASS composed EL1/EL2 DTs: exact maps, disabled PCIe hosts, watchdogs, non-PAS DSP policy and two PAS-input rejection tests, INTx provenance. No boot image retained or installed.')


if __name__ == '__main__':
    main()
