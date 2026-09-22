# Source provenance and publication changes

## Separate 22 September EL1 reference export

The owner renewed permission to publish the Qualcomm interrupt assignment,
sanitized findings and important implementation code. The separate
[`experiments/el1-pci0`](../experiments/el1-pci0/README.md) component records
original/exported hashes, exact source scope, attribution and test adaptations
in its `export.json`. It is a partial reference export, not a full kernel
overlay or a replacement for the old aggregate below. The existing v38
snapshot, aggregate and source fingerprints are unchanged. Current hardware
results and their limits are in [the dated report](EL1-USB4-2026-09-22.md).

No firmware, boot image, private disk identity, credential, raw capture,
activation/deployment script or private machine manifest is included. This
publication does not claim the partial export reproduces the complete tested
kernel by itself or establishes reliable reconnect.

## Historical v38 export

The baseline is Jim Martin's public
[`jdvmi00/glymur-usb4` at `109b47c46634c65be22e588756e8bdd8142ac6cb`](https://github.com/jdvmi00/glymur-usb4/tree/109b47c46634c65be22e588756e8bdd8142ac6cb).
Its reconstruction applies 39 baseline prerequisites and nine review patches
to Linux 7.3-rc2. Preserve that project's original source attribution and
licenses, including Konrad Dybcio's PHY series and the other contributors.
This repository does not claim authorship of the whole reconstructed kernel.

The Yoga delta is exported from the private v38 diagnostic checkpoint of
19 September 2026. `provenance/export.json` records original and public
SHA256 values for the exported files. `reproduce/kernel-files.json` records
baseline and resulting hashes for each changed kernel file. The baseline's
93 source fingerprints are retained separately, not updated to fit our work.

The aggregate patch and `snapshot/kernel/` are two representations of the same
kernel changes. Apply the patch once; do not overlay the snapshot afterward.
The standalone module and shell helpers are not part of that kernel patch.

## Deliberate public-export changes

1. The native external-SSD UID is removed. `X1_NATIVE_LACIE_UID` defaults to
   zero and the predicate explicitly rejects zero. Tests exercise both the
   rejecting default and a synthetic, non-device identity. This is not an
   authorization to admit another disk without review.
2. The older, non-native external-configuration path has its private UID
   removed and is explicitly disabled. Its historical word ordering is not
   silently presented as a validated general-purpose identity decoder.
3. Only standalone C mock suites are exported initially. Private historical
   inverse-diff checks, binary-disassembly oracles and photo-dependent fixtures
   are not represented as reproducible public tests.
4. Repository tooling and documentation are new. Original runtime helpers are
   preserved for review, but machine-specific flashing helpers, local directory
   dependencies, boot images, firmware, raw logs and photographs are excluded.

Most initialization constants were investigated from public reference driver
packages and the development machine's firmware descriptions, then expressed
as experimental Linux code. No proprietary executable or firmware payload is
included, executed by the review tooling, or asserted to be redistributable.
The sequence is hardware-specific and is not a supported Qualcomm interface.

The hardware-tested private image remains unchanged. The public snapshot is a
distinct derivative, not a claim of bit-for-bit reproduction of that image.
