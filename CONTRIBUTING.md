# Contributing

Keep changes focused and identify the source revision, board, affected layer,
observed failure and expected result. Run `python3 tools/review.py check` and
`python3 tools/review.py test`. A source change must update both its aggregate
patch and browsable snapshot, with a reviewed derivation record and hashes.
Do not refresh fingerprints merely to make checks pass.

Report source checks, compilation, simulated tests and hardware results
separately. A RAM witness or software-generated interrupt is not proof of
endpoint MSI-X delivery. Enumeration is not a disk-read result.

Preserve exact board/resource/device checks, namespace-write and passthrough
blocks, disabled internal SSD/Wi-Fi, reserved PCIe SMMU ownership and cold-off
requirements. Do not widen a device allowlist to make a test pass. The public
snapshot intentionally has no admitted SSD identity.

The separately selected [EL2 source candidate](docs/EL2-PREP.md) deliberately
requires Linux-owned PCIe SMMU DMA domains instead of reserved ownership.
Preserve both sets of guards; do not make the default EL1 route accept EL2.

RAM-only boot is not a guarantee of DMA isolation. Initialization writes to
controller registers and runs separately supplied firmware; MMIO can hang.
No automatic retry, firmware flashing, EL2 takeover or guessed MSI routing.
Hardware experiments need an independently reviewed plan and explicit approval
from the machine owner. Do not publish keys, device serials, raw storage data,
firmware or private captures in issues or commits.

Before upstream submission, check overlap with current maintainer work and
split the experimental aggregate into reviewable changes while retaining
original authorship. Do not add another person's sign-off.
