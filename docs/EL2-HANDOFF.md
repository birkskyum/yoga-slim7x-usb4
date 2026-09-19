# EL2 ADSP handoff checkpoint

This is a **new, unbooted service-only candidate**, separate from the EL2
PCI0/MSI-X candidate. It prepares the missing firmware handoff, not a USB4
result or installer. Earlier EL2/KVM boots did not test full ADSP/PMIC.

## One cold-boot experiment

Target: the development Yoga Slim 7x (83ED, BIOS NHCN62WW), with the previously
verified EFI shell, SLBounce and launch payload. qebspil starts full ADSP
during UEFI ExitBootServices, before SLBounce enters EL2. Linux must attach
to healthy existing firmware, never reload it.

The tree disables all ordinary PCIe hosts, GPU/zap shader, display driver,
CDSP, FastRPC, all three USB data controllers and retimers. It enables the
Linux-owned PCIe SMMU and APSS watchdog, disables SBSA watchdog, and retains
the absence of top-level ADSP/CDSP PAS IOMMU mappings. No USB4 PCI0/MCU nodes
or USB4 admission markers exist. EFI framebuffer and the built-in I2C
keyboard provide the console.

Only SanDisk is connected. LaCie, Ethernet and hubs stay disconnected. Linux
contains no NVMe module, MCU module, Type-C probe helpers or firmware files.
The SMP2P edge and PMIC platform initialize during boot. At the RAM shell,
remove SanDisk and run `check-adsp` once, confirming `YES` to attach ADSP.
Automatic module loading is blocked.

Success means **ADSP attached at EL2 and PMIC transport reporting service UP**.
The two passive clients share one service indication. They send no UCSI or
alt-mode requests. This is not proof of Type-C negotiation, tunneling, a
disk read or MSI-X. Photograph EFI messages and the final PASS/STOP, then
cold power off. Any fault, crash, timeout, hang, failed EFI load or unexpected
reboot ends the attempt. Do not retry or attach LaCie in that boot. A hang
requires holding power. SLBounce can request reboot after secure-launch
failure; that is not permission for a second attempt.

## Changes and attribution

The loader is Stephan Gerhold's [qebspil at 8e4d9e676a3b](https://github.com/stephan-gh/qebspil/tree/8e4d9e676a3b3afe136cda9b953a2139ff1a32d0).
`uefi/qebspil-yoga.patch` restricts it to the marked Yoga ADSP node, exact
firmware names and reserved regions. It requires full reserved-memory
coverage, rejects unexpected EDK2 event layout, propagates startup errors
and halts instead of continuing after failed preparation. ALWAYS_START is 0.
The EDK2-internal callback technique remains experimental. Loading this EFI
driver is not evidence that its callback succeeded or Linux entered EL2.

Separately supplied firmware, never redistributed:

| File | SHA256 | Region |
| --- | --- | --- |
| qcadsp8380.mbn | `0497f837f1d3b1788cbdb1fad665eea1e42fd5ad3a41fabffd421e5df7b9a1fc` | `0x87e00000`, size `0x3a00000` |
| adsp_dtbs.elf | `5556fa684f96b203c330ba0bc2daddedeb5c358ff423b3d190e74ddfcde8b9ad` | `0x8b800000`, size `0x80000` |

ELF/bounds checks do not establish hardware compatibility. RAM-only does
not mean DMA-isolated. The experiment changes live DSP/interrupt state,
though it does not flash firmware or write storage.

Linux takeover adapts Stephan Gerhold's SMP2P, q6v5 attach and broken-reset
work, inspected from the [linux-gaokun mirror at 7463df37160b](https://github.com/right-0903/linux-gaokun/tree/7463df37160bdecc3f2609d72f9a69cfa2b390e4/patch%20sets/el2).
Original patch IDs, all authored by Stephan Gerhold:

- SMP2P IRQ order `eced965b96efe8a717f77024b7b8906fbf4ba7b7`.
- SMP2P takeover `1ce6120c1f9eca9e5d5f2795e8b38ff1b6717d8f`.
- q6v5 state/attach `1b44b3a05be31efecd2b39f1609f7732147cdb8a` and `16295fac26e948dc4e9b0156e7c5d5c3e8b460e9`.
- Broken reset `b636390249939606c00b2bdd08700cd1c72ef93b`.
- RPMsg/QRTR races `d3e7469f02e940d2524ccb6d21920415c7fbe596`, `0c0ab479435dafd626a1ccdada199841ff325717`, `48fb65528275bee6850323ec07cd1057a4e61947`.

The last three retain the baseline's const-qualified RPMsg prototypes.
QRTR registers before opening its receive channel to avoid dropping its
initial hello. This is not a tested application of the entire older series
to 7.3-rc2, nor a new signed-off upstream submission. No sign-off is added
on another person's behalf.

The attach-only path requires the build option, EL2, exact board/node, root
marker, broken-reset, absent PAS mapping and healthy ready/handover/fatal/stop
signals. It preserves bounded firmware SMP2P entries instead of zeroing or
allocating them. Unknown states fail closed. Generic PAS remains the default
outside this marked path. Generic TZ memory is used, not SHM bridge. Recovery
is disabled, load/start/stop/detach callbacks reject calls, the module stays
loaded until power-off, sysfs controls are read-only and debugfs is unmounted.
These are test guards, not a security boundary against arbitrary root commands.
Attach-only recovery is [not established upstream](https://www.mail-archive.com/linux-remoteproc@vger.kernel.org/msg00793.html).

## Build and validation

Reconstruct/apply the aggregate per [REPRODUCE.md](REPRODUCE.md). Merge
`reproduce/el2-handoff.config` into `reproduce/v38.config`, set LOCALVERSION
to `-yoga-x1-el2-handoff`, run olddefconfig and build Image, Yoga DTB and
qcom_q6v5, qcom_q6v5_pas, qcom_common, qcom_pil_info and mdt_loader modules.

`bash tools/build-qebspil.sh NEW_DIRECTORY` pins qebspil and its dependencies
and builds the restricted EFI driver in aarch64 Linux.
`tools/package-el2-handoff.sh` takes source tree, completed build, qebspil EFI,
private firmware directory, verified old boot-assets directory and a new
output directory, in that order. It requires normal aarch64 Linux kernel
tools plus systemd-ukify, BusyBox, cpio, kmod, dtc, binutils, util-linux and
mtools. It creates a local 128 MiB image and byte-checks all UKI sections and
FAT payloads. It never opens a disk device or authorizes flashing.

ASan/UBSan guard/SMEM tests, previous mocks, aggregate/provenance checks and
composed-DT rejection tests cannot simulate UEFI ordering, authentication,
DMA or SMP2P timing. Compile/package success is not boot success. Generated
images, firmware and launch payload remain private. Re-identify SanDisk and
preserve the previous image before writing. Independently review this new
handoff plan with the owner before boot; earlier v38 approval is not a
hardware validation of this different experiment.

## Local build result, 20 September 2026

The ARM64 kernel Image and all five modules compiled, including final module
symbol resolution. The restricted qebspil EFI driver compiled. All 17 C mock
executions passed with ASan/UBSan. Both the existing PCI0 DT regression check
and the separate service-only DT/negative tests passed. The local packager
verified firmware hashes/ELF bounds, module dependencies, absence of NVMe and
probe helpers, extracted UKI sections, the GPT, and every FAT payload file.
The affected PAS, SMP2P, RPMsg and QRTR translation units also compiled with
both new experiment options disabled. This was not another complete EL1 link.

The 128 MiB local candidate image has SHA256
`39a8a17bb3f21aec34bf7f4e673f2efed5e7b2433a88a5e726102b6ae453be05`.
The UKI has SHA256
`df50e22fd5384084b137203baf9577e7c1e9842eac871cadeb7dd9442f842798`.
The qebspil EFI has SHA256
`80d297d744c0042f3c7b73672f29c6593f3ed1dab601563b3bf9dfef1f78dfb2`.
No firmware or boot binary is uploaded. No hardware boot result is claimed.
