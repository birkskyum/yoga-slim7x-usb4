# Public-export validation

19 September 2026. These results apply to the public, fail-closed derivative,
not an unchanged copy of the private v38 boot image.

Completed locally before publication:

- All exported source fingerprints matched the publication manifest.
- The 51-file kernel patch reversed to independently pinned baseline file
  hashes and reapplied to the exact browsable source bytes.
- The retained 93 baseline fingerprints passed, and the public `prepare`
  command reconstructed a new complete Yoga kernel tree successfully.
- Fifteen standalone mock executions passed with ASan/UBSan on arm64 macOS
  using Apple clang 21.0.0, and in a network-disabled ARM64 Linux container
  using GCC 16.1.1 and Python 3.14.7.
- Coverage includes protocol/parser bounds, PCI0 initialization, read-only
  DWC/ITS diagnostics, MSI-X-only queue policy, configuration-off behavior,
  the rejecting public identity default and an exact synthetic identity.
- Shell syntax and Python compilation checks passed for the new tooling.
- Two existing whitespace irregularities in the imported NVMe source are
  retained byte-for-byte; patch context is not reformatted for publication.
- The allowlisted publication payload was checked for private device IDs,
  local paths, credentials, firmware and boot-image files.

macOS uses the existing host errno values and explicitly supplies Linux's
EREMOTEIO/EUCLEAN definitions where absent. Tests simulate providers; they
do not emulate the controller, Gunyah, electrical behavior or real interrupts.
The imported historical binary/disassembly and whole-image/QEMU checks are
not claimed as part of this public test suite.

GitHub Actions independently passed the source checks and all fifteen mock
executions on Ubuntu for public commit
`6704af8427ddbe31e0eb7629741c9571427e052a`.
See the [successful CI run](https://github.com/birkskyum/yoga-slim7x-usb4/actions/runs/35465196041).

The original public v38 derivative's clean ARM64 Image, Yoga DTB, requested
modules and standalone frontend build subsequently completed successfully.
That is not a build result for the later EL2 changes below. The public export
has not been hardware-booted and does not establish working endpoint MSI-X.

## EL2 source-only branch

The separate candidate was checked without firmware, hardware access or a
SanDisk write:

- The 52-file aggregate reverses to the retained baseline hashes and
  reapplies to the exact current snapshot. Original private/v38-export
  provenance hashes are retained rather than overwritten.
- Sixteen C mock executions pass with ASan/UBSan on arm64 macOS (Apple clang)
  and in a network-disabled ARM64 Linux container (GCC 16.1.1).
- New coverage includes all 128 environment-policy combinations, rejected
  domain types, 32/64-bit IOVA and 1:1 address cases, missing/wrong/partial
  mappings, and use of the resolver by both NVMe and ITS diagnostics.
- Four Python IORT tests pass using synthetic tables only. The separately
  supplied pinned public IORT also passes checksum, bounds and PCI4/PCI6/PCI0
  calibration at RID 0, 0x100 and 0xffff. It is not a current-machine dump.
- The actual Yoga DT and both overlays compile and compose. Inspection of
  the merged trees confirms the EL1/EL2 markers, exact SID/MSI maps, disabled
  PCIe hosts, watchdog changes, ADSP mapping and existing INTx numbers.
  Existing base/runtime-overlay dtc warnings remain; this is not a DT binding
  schema validation claim.
- The changed PCI0 driver, ITS driver and NVMe PCI translation units compile
  for ARM64 with the EL2 option both off and on, using GCC 16.1.1 and `W=1`
  without warnings in that targeted build. The build
  outputs and source tree are separate from the original public v38 build.

Full candidate Image/module builds were started, then stopped while rebuilding
unrelated subsystems in the broad saved configuration. They are **not**
reported as successful full builds. The targeted compilation above completed.
No candidate was packaged as a boot image, installed or hardware-tested.
Mocks cannot validate Gunyah, physical SIDs, MSI delivery or EL2 boot safety.
