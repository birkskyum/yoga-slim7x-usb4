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

The clean ARM64 kernel/module build is in progress. The public export has
not been hardware-booted and does not establish working endpoint MSI-X.
