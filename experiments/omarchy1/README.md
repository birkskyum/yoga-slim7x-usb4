# General USB4 NVMe flavor (omarchy1) reference code

Published with the owner's permission, 23 September 2026, and updated on
8 October 2026 with patches 0011 to 0013 and on 9 October 2026 with patches
0014 and 0015. This is the code behind the
[23 September report](../../docs/USB4-2026-09-23.md), the
[8 October report](../../docs/USB4-2026-10-08.md) and the
[9 October report](../../docs/USB4-2026-10-09.md): any USB4 NVMe drive through
the standard NVMe driver, Files integration, Eject, pulling a drive without
Eject, and sleep and wake with or without a drive connected, with a power
domain that can go off and come back. It is **not a
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
| `patches/0002` to `0015` | The general flavor as fourteen commits on top of that, with their messages |
| `kernel/` | The full changed files after patch 0015, for reading and for the tests |
| `base/` | The pre-0002 versions of four files, which one test compares against |
| `runtime/usb4_x1d.py`, `omarchy-usb4-sleep` | The service and its systemd-sleep hook |
| `runtime/test_usb4_x1d.py` | 50 service tests against a simulated router |
| `tests/` | 33 tests that compile the real kernel functions against mocks, plus a publication check |
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
| 0010 | Keep `gcc_usb4_0_gdsc` on, and start the router only when it reports powered | Hardware: power-down, restart and prepare work. The always-on part is replaced by 0011 to 0013; the powered check stays |
| 0011 | Let `gcc_usb4_0_gdsc` go off, with none of the router's resets held across the power-off | Hardware: not enough alone, the domain went off and did not power on again |
| 0012 | Release the tunnel reset a retirement holds before the router asks for its domain | Hardware: after the same stop and ten seconds off, the power-on completed with the reset released and stuck without. Not enough alone (boot 5023f1ed) |
| 0013 | Move the PHY clock selectors before the domain goes off | Hardware: a switchable test kernel stuck without the move (boot 59a4d4ab); a build of 0011 to 0013 restarted the router three times in one boot, and a drive then connected, ejected and reconnected (boot 69f50f94) |
| 0014 | Release the tunnel reset when an idle power-down ends, because a system resume powers the domain on by itself | Hardware: without it every resume stuck the domain (boot 2a77a7cf). With it the `pm_test` levels, deep and s2idle sleeps and a suspend through systemd passed, and a drive connected afterwards (boot 06d4e5e7) |
| 0015 | End an Eject or a pulled retirement like the idle power-down, with the router's resets and the tunnel reset released and the domain off | Hardware: an Eject switched the domain off and the router came back, and a suspend with a drive mounted slept, woke and reconnected the drive (boot e4536699). The pulled case is mock-tested only |

The service prepares the router while the port is empty, connects on
attach, leaves mounting to udisks, and runs the Eject chain once the drive's
filesystems are unmounted. Before sleep it takes a logind delay lock and asks
the desktop to unmount, so an open Files window lets go of the drive. After a
restart it adopts an idle or retired router instead of failing.

## Known limits

- One port. On that port the experimental device tree limits the USB
  controller to USB 2 and disables DisplayPort, because the router holds the
  PHY while it waits. Qualcomm's per-attach model avoids that.
- Of nine real sleeps on the builds of patches 0014 and 0015, eight came
  back and one did not (boot e4536699, with the Wi-Fi firmware unresponsive
  beforehand). That one did not reproduce. Neither it nor the hang from the
  earlier reports is explained.
- Hibernation is untested and expected to stop USB4 until a restart.
- The router firmware is required and not included; the report says where
  it comes from.

## Run the tests

Python 3.12+ and a C compiler with ASan/UBSan:

```sh
python3 -B -m unittest discover -s experiments/omarchy1/tests -p 'test_*.py'
python3 -B -m unittest discover -s experiments/omarchy1/runtime -p 'test_*.py'
```
