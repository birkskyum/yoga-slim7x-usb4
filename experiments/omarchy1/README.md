# General USB4 NVMe flavor (omarchy1) reference code

Published with the owner's permission, 23 September 2026. This is the code
behind the [23 September report](../../docs/USB4-2026-09-23.md): any USB4 NVMe
drive through the standard NVMe driver, Files integration, Eject, pulling a
drive without Eject, and powering the router down for sleep. It is **not a
release, installer or general USB4 driver**. It covers router 0 (the left rear
port) of the Lenovo Yoga Slim 7x only, and Qualcomm's own router driver is the
path to mainline.

It builds on [`experiments/el1-pci0`](../el1-pci0/README.md), which has the
EL1 MSI receiver and the PCI0 host integration. Firmware, boot images, device
identities, deployment scripts and private captures are not included.

## Contents

| Path | Content |
|---|---|
| `patches/0001` | Replug after a missed cable e-marker, as a diff against `el1-pci0`'s host source |
| `patches/0002` to `0010` | The general flavor as nine commits on top of that, with their original messages |
| `kernel/` | The full changed files after patch 0010, for reading and for the tests |
| `base/` | The pre-0002 versions of four files, which one test compares against |
| `runtime/usb4_x1d.py`, `omarchy-usb4-sleep` | The service and its systemd-sleep hook |
| `runtime/test_usb4_x1d.py` | 50 service tests against a simulated router |
| `tests/` | 25 tests that compile the real kernel functions against mocks, plus a publication check |
| `export.json` | Hashes of every exported file, its private source and any path adaptation |

The kernel files and patches are byte-for-byte exports. Three test files had
their source paths adapted to this layout; `export.json` lists each change.
The general flavor is switched on by the module parameter
`thunderbolt.x1_general=1`, and the managed harness remains the default.
The service's `--account` default is the test machine's user; pass the
desktop user it should notify and unmount for.

## What each patch does, and the evidence

| Patch | Change | Evidence |
|---|---|---|
| 0001 | Re-arm an attachment that never reached USB4, so a replug can connect | Hardware: boot f339cc52's first attach stayed in plain USB mode, and one replug connected |
| 0002 | Admit any USB4 NVMe drive, bind the standard NVMe driver, remove the endpoint on Eject | Hardware: three full plug, mount, Eject, unplug, replug cycles on boot 8decd967 |
| 0003 | The private NVMe fence applies only when the device tree opts in | Hardware: the first general boot failed on the fence, the next one passed it |
| 0004 | Let queued connection-manager events finish before holding the tunnel | Hardware: a hold race on one boot; mock-tested drain |
| 0005, 0006 | Keep, then drop, the 32-bit DMA cap for the tunnel endpoint | Hardware: swiotlb stayed unused through 9+ GiB, so 64-bit DMA works and the cap did nothing |
| 0007 | Power an idle router down before system sleep, and let a retired session suspend | Hardware: first-session sleep works; see the report for the restart case |
| 0008 | Retire a router whose drive was pulled without Eject, without touching the gone device | Hardware: pull while idle and pull mid-write, no SError, journal replay on replug |
| 0009 | Accept a deferred runtime suspend in the idle power-down | Hardware: the power-down failed with -EBUSY before, passes after |
| 0010 | Keep `gcc_usb4_0_gdsc` on, and start the router only when it reports powered | Hardware: power-down, restart and prepare work; without it the domain never powers on again |

The service prepares the router while the port is empty, connects on
attach, leaves mounting to udisks, and runs the Eject chain once the drive's
filesystems are unmounted. Before sleep it takes a logind delay lock and asks
the desktop to unmount, so an open Files window lets go of the drive. After a
restart it adopts an idle or retired router instead of failing.

## Known limits

- One port. On that port the experimental device tree limits the USB
  controller to USB 2 and disables DisplayPort, because the router holds the
  PHY while it waits. Qualcomm's per-attach model avoids that.
- Sleep after the router has been restarted in the same boot never returns
  (2 out of 2). First-session sleep works. Details in the report.
- Hibernation is untested and expected to stop USB4 until a restart.
- The router firmware is required and not included; the report says where
  it comes from.

## Run the tests

Python 3.12+ and a C compiler with ASan/UBSan:

```sh
python3 -B -m unittest discover -s experiments/omarchy1/tests -p 'test_*.py'
python3 -B -m unittest discover -s experiments/omarchy1/runtime -p 'test_*.py'
```
