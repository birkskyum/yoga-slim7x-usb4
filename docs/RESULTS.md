# Results as of 19 September 2026

These are bounded observations from one Yoga Slim 7x and one direct-connected
LaCie Rugged SSD4 on the left-rear port. They are not general compatibility or
reliability claims.

## Hardware checkpoint history

| Checkpoint | Observation | What it does not prove |
| --- | --- | --- |
| v29–v38 | USB4 negotiation, PCIe enumeration and a 4096-byte offset-zero read into RAM succeeded, using timeout polling | Reliable interrupts, throughput, writes or general storage support |
| v30/v33 | MSI-X enabled, two vectors, one I/O queue; table and cached messages matched; masks clear; real IRQ counts zero | That an endpoint write reached the ITS |
| v32 | Software ITS INT commands through the EL1/Gunyah-visible ITS reached the NVMe IRQ handlers | A physical device-table binding or endpoint MSI-X delivery |
| v36 | Temporarily retargeting one vector to owned coherent RAM captured the endpoint payload; original entry restored and verified | Delivery to the actual ITS address |
| v37 | Internal DWC MSI address/enable/mask/status registers were zero before NVMe; selected PARF state recorded | Controller state during every later I/O or a justified alternate receiver setup |
| v38 | Cached EL1-visible ITS capability lacked the optional unmapped-message reporting bit, so diagnostic MMIO was skipped; zero genuine NVMe IRQs and 26 polled completions | The physical ITS's UMSI capability, absence of MSI-X support, or a firmware defect |

v36's RAM witness and v32's software injection are **not enabled in v38**.
Full shutdown is required between experiments. No filesystem was mounted and
no disk-write test was performed in these diagnostic checkpoints.

## Remaining route question

The observed endpoint is segment 0, RID `0x0100`. A **published same-model IORT
dump**, not a capture of this machine's current BIOS, and the test DT agree
on ITS DeviceID `0x80100`. MSI-X messages
use address `0x17050040`, event data 0/1. This establishes software consistency,
not the actual incoming DeviceID, address forwarding or firmware-owned binding.

In the hardware-tested v38 environment, the PCIe SMMU at `0x15400000` remains
reserved to firmware and the ITS view is mediated by Gunyah at EL1. Neither
v32 nor v38 observes Gunyah's physical device mapping or stage-2 configuration.
SMMU ownership was not taken over and v38 refuses EL2. The NHI's separate
translated DMA domain does not establish isolation for the PCIe endpoint.

The baseline's `arch/arm64/boot/dts/qcom/x1-el2.dtso` explicitly describes
Gunyah ITS emulation problems for some X1 PCIe controllers. That is a relevant
same-SoC precedent, **not proof that PCI0 has the same defect**. The source-only
[EL2 candidate and calibrated evidence](EL2-PREP.md) investigate this lead.
No EL2 USB4 hardware result is available.

Useful help would be a documented PCI0 MSI forwarding/DeviceID setup for this
EL1/Gunyah configuration, a known-working same-controller implementation, or
confirmed internal-receiver IRQ and sequencing. We have no basis for a
DeviceID sweep or guessing a parent IRQ.

## Newly reviewed upstream patch

The [17 September Qualcomm MSI initialization patch](https://lists.openwall.net/linux-kernel/2026/09/17/1319)
skips the internal DWC MSI receiver when `msi-map` selects an external
controller. It changes `qcom_pcie_ecam_host_init()`.

Our diagnostic instead supplies `x1_ecam_ops`, without an `.init` callback,
to `pci_ecam_create()`. It never calls that Qualcomm initializer or the DWC
MSI initialization helpers. Applying that patch alone would not change the
USB4 PCI0 path being tested. No hardware run was performed for this lead.

## Public snapshot validation

See [VALIDATION.md](VALIDATION.md) for checks actually rerun on the public
export. The private v38 kernel was built and tested previously; those results
do not automatically validate the publication changes. The public default is
fail-closed and has not been boot-tested.
