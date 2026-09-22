# Yoga Slim 7x USB4 development

**22 September 2026 update:** experimental normal-EL1 Omarchy now has genuine
DWC MSI-X, verified external read/write and a short ~2.6 GB/s read / ~2.8 GB/s
write filesystem demonstration. Safe removal has passed; reliable reconnect
and general plug-and-play remain unfinished. See the [Qualcomm guidance,
results and limitations](docs/EL1-USB4-2026-09-22.md) and [selected actual
implementation sources/tests](experiments/el1-pci0/README.md).

The rest of this page describes the **historical v38 export**, not the current
hardware-tested development kernel. The new source component is a partial
review export, not a complete replacement kernel or installer.

Experimental Linux USB4 bring-up for the Lenovo Yoga Slim 7x (83ED,
Snapdragon X1E80100). This publishes the source behind the v38 diagnostic
checkpoint so others can inspect it, reproduce the software tests and build
on the work.

**This is not a working USB4 driver release or an installer. MSI-X delivery
is still unresolved. Do not use it with valuable data.**

On the development machine, the private v38 build negotiated USB4, created
a PCIe tunnel, enumerated a LaCie Rugged SSD4 and read 4096 bytes into RAM.
NVMe completed through timeout polling. Genuine NVMe interrupt counts
remained zero. No filesystem was mounted and no disk-write test was performed.

The public snapshot removes the test SSD's unique identity and refuses
external-device admission by default. It is not byte-identical to the
hardware-tested private build. Firmware, boot images and raw captures are
not included. See [publication changes](docs/PROVENANCE.md).

## Start here

- [Results and unresolved MSI-X route](docs/RESULTS.md)
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

The immediate missing piece is an authoritative description or working
implementation of **USB4 PCI0's physical MSI route under EL1/Gunyah**.
Correct Linux tables and successful software interrupt injection have not
established that endpoint-originated MSI-X writes reach the ITS.

This snapshot builds on [Jim Martin's Glymur work](https://github.com/jdvmi00/glymur-usb4/tree/109b47c46634c65be22e588756e8bdd8142ac6cb),
Konrad Dybcio's PHY work and their upstream dependencies. Existing authorship
and licenses are retained. This is not an upstream-ready patch submission.
