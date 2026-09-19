# Reconstruct and build

The starting point is the **complete** pinned Glymur reconstruction, not
vanilla Linux and not a Surface DT copied onto the Yoga.

## Offline tests without firmware

Python 3.12+, Git and a C compiler supporting ASan/UBSan are required.

```sh
python3 tools/review.py check
python3 tools/review.py test
```

These run pure parsers and mocked controller/IRQ providers. They do not touch
the Yoga, load modules, access MMIO or emulate firmware.

## Reconstruct the kernel

Clone Jim's public repository and follow its `docs/REPRODUCE.md` at the pinned
commit. Use its verified archive and reconstruction procedure:

```sh
git clone https://github.com/jdvmi00/glymur-usb4.git
git -C glymur-usb4 checkout --detach 109b47c46634c65be22e588756e8bdd8142ac6cb
```

The official Linux 7.3-rc2 archive expected by that recipe has SHA256
`6b97fb9397172e95ed95b56a78524a184bf186858bea7a271b9ebe81a0e57417`.
After its reconstruction finishes, return to this repository and run:

```sh
python3 tools/review.py prepare \
  --baseline /absolute/path/to/glymur-usb4/build-output/reconstructed/linux-7.3-rc2
```

The helper checks the retained baseline fingerprints and pre-change hashes,
copies the tree into a new `build-output/kernel/`, applies the Yoga patch and
verifies every resulting changed file. Existing output is refused. It neither
downloads nor installs anything.

## Compile on an ARM64 Linux build host

Use a kernel-capable toolchain with GNU make, GCC, binutils, flex, bison,
OpenSSL/libelf development headers, bc and Python. A cross-build needs the
corresponding `CROSS_COMPILE` setting. The original build used GCC 16.1.1; see the
saved config for compiler-related settings.

```sh
bash tools/build-kernel.sh
```

This builds the Image, Yoga DTB, required modules and the standalone frontend
without installing them. The public release suffix is distinct from v38.
The SSD admission gate remains disabled. Building does not establish hardware
safety or successful interrupt delivery.

`snapshot/runtime/yoga-mcu.dtso` and the shell helpers document the original
RAM-only environment. Firmware is required to turn these into a bootable
experiment, but is deliberately not included. The frontend requires the
40,816-byte X1 MCU payload with SHA256
`cd4f5929b51f2dbb0b583693ff8d024521c87f0d2e45c7adc142fed976650b99`.
ADSP firmware and its board data are separate dependencies. A checksum is not
a redistribution license or proof that an arbitrary board is compatible.

There is no supported public image, automatic flashing command or install
procedure. Do not repurpose the old SanDisk-specific helper for another disk.

## Separate EL2 preparation

See [EL2-PREP.md](EL2-PREP.md) before using the optional configuration fragment
or supplemental overlay. The build command above retains the default EL1
policy. The EL2 branch is source-only and requires separate output directories,
boot-chain review and hardware approval; the fragment does not create an
EL2-capable loader or admit a storage device.
