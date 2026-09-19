# Source provenance and publication changes

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

## EL2 source-only derivative

The `el2-source-prep` branch adds an opt-in environment/DMA-domain guard,
IOVA-aware MSI address checks, supplemental DT/config sources, evidence audit
and offline tests. [EL2-PREP.md](EL2-PREP.md) records the source attribution,
IORT calibration and limits. No EL2 USB4 hardware result is claimed.

The initial candidate's ADSP mapping was subsequently removed after reviewing
the owner's earlier successful EL2/KVM boots and preceding SMMU fault storm.
The DSP policy follows Jens Glathe's non-PAS overlay split, not a new fix
claimed here. The earlier KVM test did not validate full ADSP/PMIC startup.

`provenance/export.json` retains every original `private_source_sha256`.
For modified exports it additionally retains `v38_export_sha256` and marks
`changed_since_v38_export`. Newly authored snapshot files have no private
source hash. The current `public_sha256` covers the browsable derivative.
Later edits to newly authored files retain `initial_source_sha256` and
`changed_since_initial_source`; they are not mislabeled as v38 exports.
The original `changed_for_publication` flag continues to describe the initial
export, not later development.

`tools/refresh-snapshot.py` mechanically derives the aggregate from reviewed
snapshot edits, reversing HEAD's patch and requiring the independently pinned
pre-change hashes first. New snapshot paths must be explicitly listed. It
never changes the 93 baseline fingerprints. Its output still needs review
and `tools/review.py check`; it is not a way to bless an arbitrary baseline.
