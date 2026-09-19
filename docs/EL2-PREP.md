# EL2 source-only preparation

This branch prepares a separate experiment. **It is not a v39 hardware result,
a bootable image, or a claim that MSI-X works.** The default configuration
remains EL1-only. The public SSD identity remains zero and rejects admission.
No SanDisk write, Yoga boot, hardware MMIO or new storage read was performed
for this preparation.

The subsequent [ADSP handoff checkpoint](EL2-HANDOFF.md) implements the
separate firmware-service prerequisite and local image packaging. It does
not combine it with this PCI0 experiment and has no hardware result yet.

## Earlier EL2 boot is already proven; ADSP handoff is not

The owner's 14 September 2026 KVM test records establish two successful
physical cold boots on this Yoga, BIOS NHCN62WW. Linux reported all CPUs
starting at EL2 and KVM VHE initialized. On each boot, one-vCPU and two-vCPU
Linux guests passed arithmetic and timer checks and shut down. This was
kernel 7.2.4, not this branch's 7.3-rc2 USB4 kernel. The retained evidence is
photographs and targeted owner-run log checks, not complete exported logs.

The working boot used Nikita Travkin's
[SLBounce at c090a8cdafa2](https://github.com/TravMurav/slbounce/tree/c090a8cdafa25e4c99df90f8d6f73f3805d9b397),
the locally retained `tcblaunch.exe` 10.0.26100.1742, and Jens Glathe's
[non-PAS overlay split at ce20a20db795](https://github.com/jglathe/linux_ms_dev_kit/commit/ce20a20db795f299983f092fb899e0ad1c005be3).
The local image has SHA256
`245e39ad26f327015ceb93f54ee6e7a615b6cb195a75ac78b35ef02a62a6f0ce`;
its source-built non-PAS DTB has SHA256
`76c4d6a31d978da218cd94d39e0492f4767dfaeab92a6609ea06b63a16f816ba`.
Those two artifacts and the retained boot assets were rehashed during this
review. SLBounce is
`cc1b62e8bafeac98b80c99397405f3290af965801a4df0d2416ef120a2fbf6bd`;
the launch payload is
`5dfcd0253b6ee99499ab33cac221e8a9cea47f3fdf6d4e11de9a9f3c4770d03d`.
No launch binary, boot image or private photo is distributed here.

The preceding PAS-mapped boot faulted on apps SMMU 0x15000000 with SID
0x1000, FSR 0x402 and IOVA 0x86b020c0. The successful comparison removed only
the ADSP/CDSP top-level `iommus` properties; neither SMMU was disabled.
That agrees with the
[published firmware-handoff explanation](https://www.spinics.net/lists/kernel/msg6126299.html).
It does not independently establish all of this BIOS's PAS capabilities.

**Correction to the first version of this branch:** it copied the ADSP PAS
mapping from the pinned baseline without incorporating that prior hardware
result. The candidate now omits both DSP mappings. The DT checker requires
their absence in the base and composed trees and rejects PAS-mapped inputs.
Simply omitting a property from an overlay does not remove it from an input
DTB. Do not compose this supplement on top of the baseline's PAS EL2 overlay.

This removes a known boot regression, not the remaining USB4 prerequisite.
The successful KVM initramfs omitted remoteproc modules and did not test full
ADSP or PMIC GLINK. The v38 `probe-usb4` instead explicitly loads
`qcom_q6v5_pas` and waits for PMIC logging, Type-C and UCSI services. Its
EL1 startup must not be reused at EL2 without reviewing a non-PAS handoff.
Preloading full ADSP in UEFI (for example with
[qebspil](https://github.com/stephan-gh/qebspil)) would be a new,
separately reviewed step, not something the earlier KVM test already proved.
That project's documented X1E path also needs matching Linux takeover
patches. The current reconstructed `qcom_q6v5_pas.c` checks PAS availability
before probe, and its X1E ADSP descriptor does not set `early_boot`; merely
preloading a firmware binary or enabling the generic attach callback is not
a reviewed handoff implementation. Do not turn on an unconditional DSP start
or fabricate PAS support to get past those checks.

The existing SLBounce source checks that the final DT has the GPU zap shader
disabled before doing its ExitBootServices transition. The candidate retains
that property. A source match is not a new boot result, and loading the EFI
driver alone does not establish that Linux has actually entered EL2.

## Why test a different boot environment?

The pinned baseline's `arch/arm64/boot/dts/qcom/hamoa.dtsi` routes pcie4 and
pcie6a to ITS at EL1. Its `x1-el2.dtso` adds ITS routes for pcie3 and pcie5 and
enables the PCIe SMMUv3. The overlay explicitly attributes the EL1 restriction
to problems with Gunyah ITS emulation on some controllers. The
[published EL2 overlay submission](https://lists.openwall.net/linux-kernel/2025/05/03/172)
documents that precedent.

This makes an EL1 firmware/hypervisor limitation a strong hypothesis for the
tunnel controller. It does not identify the exact fault. An EL2 success would
implicate the EL1 environment but would not distinguish ITS emulation,
SMMU/stage-2 mapping or another firmware-dependent initialization difference.
Changing both privilege and SMMU ownership is not a one-register experiment.

v32's INT command tests the host-visible software interrupt path, not a
physical device mapping. v38's cached TYPER describes the host-visible ITS,
not necessarily physical UMSI capability. v36's DRAM witness proves neither
doorbell-page forwarding nor the incoming physical DeviceID. The observations
constrain endpoint/Linux faults; they do not exhaustively refute them.

## IORT calibration and provenance correction

The IORT used by this project is the
[published Yoga dump at a pinned aarch64-laptops commit](https://github.com/aarch64-laptops/build/blob/2e58842f5fa2f87771c2df017ae4d8c65225ef10/misc/lenovo-yoga-slim-7x/acpi/iort.dsl).
It is **not this development machine's current BIOS IORT**. Earlier wording
in RESULTS.md calling it a captured firmware IORT was too strong.

The original DSL bytes have SHA256
`038b39c9e65687e6d702f98d2eb24d38698d05986b54a12abacf8d70d2fa8db9`.
No copy of the dump is added to this repository. The read-only
`tools/audit-iort.py` verifies an explicitly supplied input hash, ACPI length
and checksum, node/mapping bounds and unique RC -> SMMUv3 -> ITS references.
It rejects unsupported revisions, single mappings and ambiguous mappings.
Synthetic corruption and range-boundary tests run in the normal test suite.

The actual pinned dump passed calibration at RID 0, 0x100 and 0xffff. IORT's
ID Count is inclusive (0xffff means 65,536 IDs), not the DT map length.

| ACPI root | RID | Declared SMMUv3 SID | Declared ITS DeviceID | Comparison |
| --- | --- | --- | --- | --- |
| PCI4 | 0x0100 | 0x40100 | 0xc0100 | Matches pcie4 DT MSI base 0xc0000 |
| PCI6 | 0x0100 | 0x60100 | 0xe0100 | Matches pcie6a DT MSI base 0xe0000 |
| PCI0 | 0x0100 | 0x00100 | 0x80100 | Candidate SID base 0; retains existing MSI base |

All three reference the SMMUv3 node at table offset 0x1435, MMIO base
0x15400000. Its input range 0..0x7ffff maps to ITS output base 0x80000 via
node offset 0x148d, ITS identifier 0. These are firmware declarations, not
observations of actual transactions, and a newer BIOS could differ.

```sh
python3 tools/audit-iort.py /path/to/pinned/iort.dsl \
  --sha256 038b39c9e65687e6d702f98d2eb24d38698d05986b54a12abacf8d70d2fa8db9
```

## DSDT interrupt provenance

The development machine's existing private DSDT was inspected as text; AML
was not executed. The disassembled file's SHA256 is
`a05802e6ac3c344efab8ef6a9607220dbddde237e8fd2f831cd4816ec16b849a`.
Only these derived numeric facts are published, not the private capture.

| PCI0 pin | `_PRT` direct GSI | DT SPI (GSI minus 32) |
| --- | --- | --- |
| INTA | 735 / 0x2df | 703 |
| INTB | 740 / 0x2e4 | 708 |
| INTC | 746 / 0x2ea | 714 |
| INTD | 748 / 0x2ec | 716 |

These explain the existing `yoga-mcu.dtso` INTx entries. They are **not DWC
MSI receiver parent interrupts** and are unused by the MSI-X-only NVMe policy.
PCI0 `_CBA` is 0x400000000. Its `_CRS` supplies bus and memory windows, not a
named MSI interrupt. PCI4 and PCI6 `_CRS` likewise supply bus/memory windows.
Their `_PRT` entries decode to SPIs 149..152 and 843/844/845/772, respectively;
these differ from the DT MSI0/global pairs (141/156 and 773/672).
This audit does not establish PCI0's MSI parent IRQ. No such IRQ is guessed.

## What changes in the candidate

- `CONFIG_USB4_X1_EL2_TEST` defaults off. Enabling it requires a matching DT
  marker, `is_hyp_mode_available()`, an available/non-reserved PCIe SMMU,
  exact PCI0 IOMMU/MSI maps and all four ordinary PCIe hosts disabled.
  The default build refuses both EL2 and the EL2 marker.
- These checks run before PCI0 driver registration and again before PCI0
  resource acquisition. They do not themselves change exception level or
  take ownership of an IOMMU.
- NVMe admission at probe additionally requires a Linux DMA or DMA_FQ domain
  in the EL2 build. Missing, identity, blocked and unmanaged domains fail.
  PCI core performs DMA configuration before invoking the driver's probe.
  The domain getter is not called on the unbound bus-rescan candidate. No
  sleeping group iterator is added to the hard-IRQ admission/counter path.
- The NVMe and ITS audit gates now share an address resolver. At EL1 the
  exact existing physical address is required. At EL2 the cached 64-bit
  IOVA must resolve through that device's DMA domain to the four bytes at
  0x17050040..0x17050043. Failed translation is not silently treated as a
  physical address. Logs distinguish the cached and resolved addresses.
  This checks Linux's mapping; it does not establish real device delivery.
- `yoga-pci0-el2.dtso` is a supplement to `yoga-mcu.dtso`, not its replacement.
  It takes the watchdog swap and GPU zap/IRIS disabling from the pinned EL2
  overlay, but follows Jens's non-PAS DSP policy validated by the earlier
  Yoga boot: no ADSP/CDSP top-level IOMMU mapping. Unused CDSP and PCIe hosts stay disabled.
  PCI0 uses the calibrated SID base 0, without changing its MSI DeviceID.

There is no register-sequence change, identity widening, synthetic interrupt,
MSI/INTx fallback, firmware payload, retry loop or disk-write capability.
The runtime scripts and private hardware-tested image are unchanged.

## Source validation

Run the normal hash/patch-roundtrip and mock checks. The additional DT check
requires the reconstructed pinned tree, a C preprocessor and dtc tools:

```sh
python3 tools/review.py test
python3 tools/check-el2-dt.py --kernel /path/to/reconstructed/kernel
```

The DT checker compiles the actual Yoga base and both overlays, composes them
in order, and inspects the resulting properties. Its temporary artifacts are
not installed or retained. Existing base/overlay dtc warnings are not claimed
as a binding-schema pass. See [validation results](VALIDATION.md).

`reproduce/el2-prep.config` records the separate candidate's configuration
fragment. It is **not** automatically merged by `tools/build-kernel.sh`.
Use a separate output directory; never substitute the candidate into an old
EL1 boot entry. Compiling a kernel is not constructing or approving a boot kit.

## Still required before a hardware attempt

1. Reuse and verify the earlier successful SLBounce boot assets, but review
   the final USB4 DT/command line and full ADSP/PMIC handoff separately. A
   marker or Kconfig option cannot remove Gunyah. **Do not boot this overlay at EL1:**
   SMMUv3 can probe before PCI0's guard executes. Audit ADSP/PMIC startup and
   required modules/firmware in that boot environment, not just the DT.
   First validate a bounded no-peripheral boot/service checkpoint; do not
   bundle an unvalidated DSP handoff and a LaCie MSI-X attempt into one boot.
2. Obtain the current-machine IORT or explicitly review the remaining
   same-model mapping assumption against the current BIOS. Do not guess IDs.
3. On a separate, ordinary EL1 boot with the normal internal-SSD DT, capture
   its NVMe interrupt lines to check the pcie6a ITS route. This observation
   has **not** been made here, and the internal SSD must remain disabled in
   the experimental RAM-only image.
4. Review a bounded cold-boot plan and the private exact-device identity
   separately. Do not run a vector-masking negative control in the same boot
   without its own reviewed lifetime/stop policy.

For a future approved run, genuine success needs non-synthetic hard-IRQ
entries, corresponding NVMe LPI counts, a matching read and no timeout-polled
completions. Timing alone or nonzero IRQ counts alone are insufficient.

An SMMU event can provide useful SID evidence, but an incorrect map is not
guaranteed to produce a recoverable or uniquely attributable fault. Stop on
SMMU global/unexpected-stream errors, SError, lost link or vanished endpoint;
do not automatically retarget and retry. If physical UMSI reporting is absent,
no further diagnostic register access is justified by that absence. If it is
present, a sticky global UMSIR record still needs careful attribution.
