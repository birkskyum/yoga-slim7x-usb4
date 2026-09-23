# Yoga Slim 7x USB4 development

**23 September 2026 update:** at EL1 on normal Omarchy, the experimental
kernel now runs any USB4 NVMe drive with genuine DWC MSI-X through the
standard NVMe driver. Files, Eject, replug and pulling the drive without
Eject all work, and so does 64-bit DMA through the tunnel. Sleep works once
per boot; a suspend after the router has been restarted still hangs. Only
the left rear port runs, and this is still not a release. Qualcomm's own
router driver is the path to mainline. See the
[23 September report](docs/USB4-2026-09-23.md), the
[22 September report with Qualcomm's MSI guidance](docs/EL1-USB4-2026-09-22.md),
and the reference code in [experiments/el1-pci0](experiments/el1-pci0/README.md)
and [experiments/omarchy1](experiments/omarchy1/README.md).

The rest of this page describes the **historical v38 export**, not the current
hardware-tested development kernel. The new source component is a partial
review export, not a complete replacement kernel or installer.

Experimental Linux USB4 bring-up for the Lenovo Yoga Slim 7x (83ED,
Snapdragon X1E80100). This publishes the source behind the v38 diagnostic
checkpoint so others can inspect it, reproduce the software tests and build
on the work.

**This is not a working USB4 driver release or an installer. Do not use it
with valuable data.** At the v38 checkpoint MSI-X delivery was still
unresolved; it has since been solved, as described above.

On the development machine, the private v38 build negotiated USB4, created
a PCIe tunnel, enumerated a LaCie Rugged SSD4 and read 4096 bytes into RAM.
NVMe completed through timeout polling. Genuine NVMe interrupt counts
remained zero. No filesystem was mounted and no disk-write test was performed.

The public snapshot removes the test SSD's unique identity and refuses
external-device admission by default. It is not byte-identical to the
hardware-tested private build. Firmware, boot images and raw captures are
not included. See [publication changes](docs/PROVENANCE.md).

## Start here

- [23 September report](docs/USB4-2026-09-23.md)
- [22 September report: Qualcomm's MSI route and verified I/O](docs/EL1-USB4-2026-09-22.md)
- [Historical v38 results](docs/RESULTS.md)
- [Reconstruct and build](docs/REPRODUCE.md)
- [Authorship and publication changes](docs/PROVENANCE.md)
- [Safety and contribution rules](CONTRIBUTING.md)

`patches/0001-yoga-x1-v38-diagnostics.patch` is the complete Yoga kernel delta
against Jim Martin's pinned Glymur reconstruction, not vanilla Linux.
`snapshot/kernel/` contains the same changed files for browsing.
`snapshot/module/` contains the startup frontend and helpers.
`snapshot/runtime/` preserves the RAM-only shell workflow as reference source,
not a turnkey boot image. `tests/` contains offline C mocks.

Run the firmware-free checks with Python 3.12+, Git and a C compiler with
ASan/UBSan:

```sh
python3 tools/review.py check
python3 tools/review.py test
```

At the v38 checkpoint the missing piece was **USB4 PCI0's physical MSI
route under EL1/Gunyah**. It is solved: Qualcomm supplied the interrupt
assignment for the DesignWare internal MSI receiver, and genuine MSI-X
works (see the 22 September report).

This snapshot builds on [Jim Martin's Glymur work](https://github.com/jdvmi00/glymur-usb4/tree/109b47c46634c65be22e588756e8bdd8142ac6cb),
Konrad Dybcio's PHY work and their upstream dependencies. Existing authorship
and licenses are retained. This is not an upstream-ready patch submission.
