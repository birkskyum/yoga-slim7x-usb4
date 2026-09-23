# Device tree draft: USB4 on all three Yoga Slim 7x ports

Published 23 September 2026. Two draft patches, written ahead of Qualcomm's
USB4 router driver so every port can be tested the day it appears. They are
not submitted and not meant to be merged as they are: the router binding is
still Konrad Dybcio's RFC from September 2025, and his final binding and
labels will decide the shape.

| Patch | Content |
|---|---|
| `0001-arm64-dts-qcom-hamoa-Add-USB4-Host-Routers.patch` | Three USB4 Host Router nodes and their sideband pin states, shaped to the RFC binding. Qualcomm will post its own version. |
| `0002-arm64-dts-qcom-x1e80100-lenovo-yoga-slim7x-Enable-US.patch` | The Yoga board part: enables the three routers and gives each PS8830 retimer a second endpoint towards its router, so the retimer passes USB4 and TBT3 mode changes on after programming itself. |

The base is the Qualcomm tree's `for-next` at `019fa5715726` (22 September
2026), plus the PHY binding patch from the QMP USB4 series and the RFC
binding. The RFC binding needs one addition to validate on current trees:
`usb-switch.yaml` was split, so it also has to reference
`usb-switch-ports.yaml`.

Checks run on that base: dtc with no warnings (W=1 adds none beyond the 30
that already exist), dt-validate against the whole binding tree with no
errors, and checkpatch `--strict` clean. Neither patch has been run on
hardware; the router values behind them are in the
[23 September report](../../docs/USB4-2026-09-23.md). The tunnel PCIe
controllers are not included, because their binding is Qualcomm's to define.
