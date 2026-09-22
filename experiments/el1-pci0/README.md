# EL1 PCI0 receiver and lifecycle reference code

Published with renewed owner permission, 22 September 2026. This makes the
important implementation available for review; it is **not a complete kernel
tree, apply-and-boot patch series, installer or general USB4 driver release**.
The original v38 snapshot/aggregate and its fingerprints remain unchanged.

Read [the result and design report](../../docs/EL1-USB4-2026-09-22.md) first.
Firmware, boot images, disk identities, credentials, raw logs, private runtime
manifests and machine-specific activation/deployment scripts are not included.

## Included code

| Component | Purpose | Evidence boundary |
|---|---|---|
| `pcie0-interrupts.dtsi` | Qualcomm-supplied MSI/INTx assignment; remove `msi-map` | Platform advice used in the successful EL1 integration; not sufficient alone |
| `kernel/drivers/thunderbolt/x1-imsi.c`, `.h` | Standard DWC MSI-domain/target setup, checked programming, stop/release/retirement | Current receiver source from the hardware-tested managed candidate |
| `x1-imsi-policy.c`, `.h` | Explicit PARF/global and reserved-target/BAR-window policy | Experimental policy, not a generic Qualcomm binding or DMA-isolation claim |
| `qcom-usb4-x1-pcie.c`, `.h`, `x1-pci0-mem.h` | Guarded PCI0 decoder/host integration, receiver caller, PCI retirement and reset-before-genpd ordering | Full source files for integration context; extra kernel dependencies not bundled |
| `qcom-usb4-x1-host.c` | X1 controller frontend, one-shot generations, ordered retirement, miscellaneous-reset restoration | First attach/read-write/safe removal passed; reliable reconnect remains unresolved |
| `root-port-fixup.c` | Built-in, exact-host early class/no-MSI fixup excerpt | Must be integrated into built-in PCI code, not a loadable fixup table |
| `runtime/mount_guard.py` | Strict mount and parent propagation classifier | Used by managed eject; not permission to ignore foreign mounts |
| `runtime/boot_evidence.py` | Current-boot EL1 kernel-journal proof independent of rolling dmesg | First-generation proof passed; second-generation filesystem gate not yet validated |
| `runtime/eject_guard.py` | Waits up to 45 s for a transient namespace copy, such as the one systemd-hostnamed holds after Files starts it | **Installed for the next hardware test; not yet hardware-validated** |

The kernel sources are byte-for-byte extracts of the listed development
checkpoints, not rewritten pseudocode. Some header comments still say
“PRIVATE”, “dormant” or “not hardware-tested”: these reflect their earlier
history. Publication is now authorized; the table and dated report above state
the current evidence, without rewriting the original source hashes.

`export.json` records each original/exported hash and test-path adaptations.
The host, PCI and receiver source hashes match the retained/changed source
records for the current managed module. No private identity has been replaced
with a permissive wildcard, and no safety predicate was relaxed for publication.
No boot activation is supplied.

## Dependencies and what is deliberately not claimed

The work builds on Jim Martin's pinned Glymur reconstruction at
[`109b47c46634c65be22e588756e8bdd8142ac6cb`](https://github.com/jdvmi00/glymur-usb4/tree/109b47c46634c65be22e588756e8bdd8142ac6cb),
its Linux 7.3-rc2 prerequisite/review series, Konrad Dybcio's PHY work and other
retained upstream authors. The DWC receiver/domain logic is Linux's existing
implementation; the new component supplies guarded X1 integration and
lifetime handling. Credit Qiang Yu for the PCI0 interrupt assignment, not for
unreviewed code, and do not add a sign-off on his behalf.

This selected source export omits parts of the full development stack:
X1 decoder/CFG0/training/inventory/startup/DROM helpers, common Qualcomm
USB4/NHI/Type-C changes, clock/PHY providers, native-CM lifecycle extensions,
NVMe read/write/quiesce test hooks, complete board DT/config, and the managed
runtime/GIO packaging. Interfaces in these source files depend on those parts.
**Do not copy these files over the old v38 snapshot or build a kernel from this
directory alone.** Publishing a complete, sanitized, reconstructible kernel
branch remains a separate next step; this is a reviewable first code export.

The root-port fixup's `no_msi` does not validate root-port INTx/AER/hotplug
services. The tested frontend deliberately holds that service binding. No
global-handler or surprise-unplug support is implied by providing the DT map.

The firmware-owned PCIe SMMU is not reconfigured by this EL1 experiment.
Working MSI delivery does not prove arbitrary endpoint DMA containment.
The one-board/one-drive/scratch-filesystem guards and current cold-off-on-error
policy are bring-up scaffolding, not an upstream ABI or finished product.

## Reproducible offline checks

Python 3 and a C compiler with ASan/UBSan are required. These commands never
touch hardware or load a module:

```sh
python3 -B -m unittest discover -s experiments/el1-pci0/tests -p 'test_*.py' -v
python3 -B -m unittest discover -s experiments/el1-pci0/runtime -p 'test_*.py' -v
python3 tools/review.py check
```

The receiver tests compile **actual extracted stop/release functions** with
mocked IRQ/MMIO/devres effects, covering refusal paths, ordering and failures.
They are not a full kernel compilation or evidence of endpoint IRQ delivery.
The eject tests cover the 45-second helper, including the observed
30-second service lifetime that the previous 10-second bound could not
outlast, and show with the unchanged classifier why a read-only service copy
is a conflict. Integration tests against the private frozen unmount worker
also passed (transient conflict then one normal unmount; persistent conflict,
no unmount; kernel EBUSY reported once; strict post-unmount check; evidence
failure fails closed). That worker is not part of this export, and hardware
first-click eject validation is still required.

Publication checks passed on 22 September: 7 component/export tests (including
actual receiver stop/release under ASan/UBSan), 8 eject-helper tests (11 after the
23 September helper update), the existing
source-hash/patch-roundtrip check and all 15 original public mock executions.
The old receiver-release fixture was updated to model the real positive
resource-count return value and reject zero; no production function was changed
to make a test pass. No new kernel build or hardware run was made for this export.

Original SPDX identifiers/authorship remain. New code/tests are GPL-2.0-only;
new documentation follows the repository's CC0-1.0 policy. No third-party
firmware is distributed or relicensed.
