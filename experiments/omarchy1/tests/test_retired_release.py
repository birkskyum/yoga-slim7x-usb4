#!/usr/bin/env python3
"""Run the end of an Eject or pulled retirement with mocks.

On hardware (boot 06d4e5e7), once the idle power-down released the tunnel BCR
before its vote went, gcc_usb4_0_gdsc came back by itself on every system
resume. An Eject still ended with the eleven router resets and the tunnel
BCR held, and with the router's domain on only because the platform
retirement cancels the suspend it queues. This test pins that the general
flavor now takes its own vote before the platform retirement, releases the
resets and then the tunnel BCR under it, and only then lets the domain go;
that any failure keeps the vote; and that the harness flavor is unchanged.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import build_and_run  # noqa: E402
from test_idle_retire import COMMON, c_function, HOST  # noqa: E402

PRELUDE = COMMON + r'''
typedef unsigned int u32;
#define BIT(n) (1U << (n))
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((int)(long)(p))
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define WRITE_ONCE(x, v) ((x) = (v))
enum { CHECK = 1, GET, PLATFORM, LEASE, DEASSERT, READBACK, LEASE_PUT, TUNNEL, PUT_SYNC, KFREE, DESTROY };
struct device { int votes; };
struct x1_power_stop { unsigned int resets_asserted, resets_released; u32 release_requests; };
struct reset_control_bulk_data { void *rstc; };
struct x1_gcc_usb4 { int unused; };
struct x1_port { bool baseline_released; };
struct x1_pcie_state { bool platform_retired; };
struct tb_nhi { void *tx_rings, *rx_rings; };
struct x1_host {
 struct device *dev; struct x1_power_stop power_stop; struct x1_pcie_state pcie; struct x1_port *port;
 struct { struct tb_nhi nhi; int ownership_lock; } qnhi;
 struct x1_gcc_usb4 *gcc; bool clocks_on, runtime_held, fully_retired;
 struct reset_control_bulk_data resets[11];
};
static bool x1_general;
static struct device dev; static struct x1_host host; static struct x1_port port; static struct x1_gcc_usb4 lease;
static int rings[2];
static int check_result, platform_result, get_error, deassert_fail_at, read_error, tunnel_result, put_result;
static u32 read_state;
static int x1_platform_owner_stopped(void *c) {
 assert(c == &host && !host.runtime_held && !host.pcie.platform_retired);
 note(CHECK); return check_result;
}
static void pm_runtime_get_noresume(struct device *d) {
 assert(d == &dev && x1_general && !host.pcie.platform_retired); d->votes++; note(GET);
}
static int qcom_usb4_x1_retire_platform(struct device *o, struct x1_pcie_state *s, int (*check)(void *), void *c) {
 assert(o == &dev && s == &host.pcie && check == x1_platform_owner_stopped && c == &host);
 /* Its own stop check refuses a vote recorded in runtime_held. */
 assert(dev.votes == (x1_general ? 1 : 0) && !host.runtime_held);
 note(PLATFORM);
 if (!platform_result) s->platform_retired = true;
 return platform_result;
}
/* Everything below needs the domain on: the vote is held and the platform is gone. */
static bool powered(void) { return x1_general && dev.votes == 1 && host.pcie.platform_retired && host.runtime_held; }
static struct x1_gcc_usb4 *x1_gcc_usb4_get(struct device *d, unsigned int p) {
 assert(d == &dev && !p && powered()); note(LEASE);
 return get_error ? ERR_PTR(get_error) : &lease;
}
static int reset_control_deassert(void *r) {
 (void)r; assert(powered());
 if (deassert_fail_at && (int)host.power_stop.resets_released + 1 == deassert_fail_at) return -EIO;
 note(DEASSERT); return 0;
}
static void usleep_range(unsigned long a, unsigned long b) { (void)a; (void)b; }
static int x1_gcc_usb4_reset_state(struct x1_gcc_usb4 *g, u32 *state) {
 assert(g == &lease && powered() && host.power_stop.resets_released == 11); note(READBACK);
 if (read_error) return read_error;
 *state = read_state; return 0;
}
static void x1_gcc_usb4_put(struct x1_gcc_usb4 *g) { assert(g == &lease && powered()); note(LEASE_PUT); }
static int qcom_usb4_x1_release_retired_reset(struct device *o) {
 assert(o == &dev && powered() && host.power_stop.resets_released == 11 && port.baseline_released);
 note(TUNNEL); return tunnel_result;
}
static int pm_runtime_put_sync(struct device *d) { assert(d == &dev && powered()); d->votes--; note(PUT_SYNC); return put_result; }
static void devm_kfree(struct device *d, void *p) { assert(d == &dev && p && !host.fully_retired); note(KFREE); }
static void mutex_destroy(int *m) { assert(m == &host.qnhi.ownership_lock); note(DESTROY); }
static void reset(bool general) {
 memset(&host, 0, sizeof(host)); memset(&dev, 0, sizeof(dev)); memset(&port, 0, sizeof(port));
 x1_general = general; host.dev = &dev; host.port = &port;
 host.qnhi.nhi.tx_rings = &rings[0]; host.qnhi.nhi.rx_rings = &rings[1];
 host.power_stop.resets_asserted = 11;
 check_result = platform_result = get_error = deassert_fail_at = read_error = tunnel_result = put_result = 0;
 read_state = 0; ncalls = 0;
}
static int x1_retire_session(struct x1_host *host);
/* A failure keeps the vote, frees nothing and never reports the session retired. */
static void kept(int want, int votes) {
 assert(x1_retire_session(&host) == want && dev.votes == votes && !host.fully_retired);
 assert(host.qnhi.nhi.tx_rings && host.qnhi.nhi.rx_rings);
 for (int i = 0; i < ncalls; i++) assert(calls[i] != PUT_SYNC && calls[i] != KFREE && calls[i] != DESTROY);
}
static int count(int c) { int n = 0; for (int i = 0; i < ncalls; i++) n += calls[i] == c; return n; }
'''

CASES = r'''
static void passes(int result) {
 reset(true); put_result = result;
 assert(!x1_retire_session(&host));
 int ok[] = { CHECK, GET, PLATFORM, LEASE, DEASSERT, DEASSERT, DEASSERT, DEASSERT, DEASSERT, DEASSERT,
              DEASSERT, DEASSERT, DEASSERT, DEASSERT, DEASSERT, READBACK, LEASE_PUT, TUNNEL, PUT_SYNC,
              KFREE, KFREE, DESTROY };
 assert(order(ok, 22));
 assert(host.fully_retired && !dev.votes && !host.runtime_held && port.baseline_released);
 assert(host.power_stop.resets_released == 11 && !host.power_stop.release_requests);
 assert(!host.qnhi.nhi.tx_rings && !host.qnhi.nhi.rx_rings);
}
int main(void) {
 passes(0);
 passes(1);
 passes(-EBUSY);  /* The vote is dropped either way; a deferred suspend is not a failure. */
 reset(false);    /* Harness flavor: as before, nothing released and no vote taken. */
 assert(!x1_retire_session(&host));
 int harness[] = { CHECK, PLATFORM, KFREE, KFREE, DESTROY };
 assert(order(harness, 5) && host.fully_retired && !dev.votes && !host.runtime_held);
 assert(!host.power_stop.resets_released && !port.baseline_released);
 reset(true); check_result = -EPERM; kept(-EPERM, 0);
 assert(ncalls == 1 && calls[0] == CHECK);
 reset(true); platform_result = -EIO; kept(-EIO, 1);
 assert(!count(LEASE) && !count(TUNNEL) && !host.runtime_held);
 reset(true); get_error = -EBUSY; kept(-EBUSY, 1);
 assert(!count(DEASSERT) && !count(TUNNEL) && host.runtime_held && !port.baseline_released);
 reset(true); deassert_fail_at = 6; kept(-EIO, 1);
 assert(count(DEASSERT) == 5 && count(LEASE_PUT) == 1 && !count(TUNNEL) && !port.baseline_released);
 reset(true); read_state = BIT(8); kept(-EIO, 1);
 assert(host.power_stop.release_requests == BIT(8) && !count(TUNNEL) && !port.baseline_released);
 reset(true); read_error = -ENODEV; kept(-ENODEV, 1);
 assert(!count(TUNNEL) && !port.baseline_released);
 reset(true); tunnel_result = -EBUSY; kept(-EBUSY, 1);
 assert(count(DEASSERT) == 11 && count(TUNNEL) == 1 && host.runtime_held && port.baseline_released);
 reset(true); host.power_stop.resets_asserted = 10; kept(-EPERM, 1);  /* Not the stop's full set. */
 assert(!count(LEASE) && !count(TUNNEL));
 printf("PASS retired release: resets, then the tunnel BCR, under a vote taken before the platform retirement\n");
 printf("PASS retired release: 8 refusals and faults keep the vote and the session unretired; harness flavor unchanged\n");
 return 0;
}
'''


class RetiredRelease(unittest.TestCase):
    def test_release_before_the_vote_goes(self):
        text = HOST.read_text()
        code = PRELUDE + c_function(text, 'x1_stop_release_resets') + '\n' + \
            c_function(text, 'x1_retired_release') + '\n' + \
            c_function(text, 'x1_retire_session') + CASES
        print(build_and_run(code, 'x1-retired-release-'), end='')

    def test_every_retirement_ends_released(self):
        text = HOST.read_text()
        session = c_function(text, 'x1_retire_session')
        self.assertLess(session.index('pm_runtime_get_noresume(host->dev)'),
                        session.index('qcom_usb4_x1_retire_platform('))
        self.assertLess(session.index('x1_retired_release(host)'), session.index('host->fully_retired = true'))
        # The Eject and the pulled retirement both end in x1_retire_session().
        for name in ('power_quiesce_once_store', 'pulled_retire_once_store'):
            self.assertIn('x1_retire_session(host)', c_function(text, name), name)
        # The idle power-down has its own release, before its own vote goes.
        idle = c_function(text, 'idle_retire_once_store')
        self.assertLess(idle.index('qcom_usb4_x1_release_retired_reset(dev)'),
                        idle.index('pm_runtime_put_sync(host->dev)'))


if __name__ == '__main__':
    unittest.main()
