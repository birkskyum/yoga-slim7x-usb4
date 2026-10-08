#!/usr/bin/env python3
"""Run the actual idle power stop with mocks, including the PM core's answers.

omarchy8: before the runtime vote goes, the stop releases the eleven resets it
asserted and reads them back, so that no router reset is held while
gcc_usb4_0_gdsc is off. A failed release keeps the vote, so the domain stays on.

On hardware (boot 2e50fbff) the last step, pm_runtime_put_sync(), returned
-EBUSY: with no receiver forbid holding the router, its idle check sees the
still-registered domain child. The hardware steps had all completed. This
test pins that a deferred runtime suspend is not a failure, while any other
error still is.
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
#define GENMASK(h, l) (((~0U) >> (31 - (h))) & ((~0U) << (l)))
struct x1_power_stop {
 bool attempted, finished, mcu_request_cleared, rx_reference;
 bool phy_off, phy_exited, route_cleared, memory_force_cleared;
 bool icc_released, gcc_released, clocks_released, runtime_released;
 u32 mcu_control, host_control, reset_requests;
 unsigned int resets_asserted;
 u32 release_requests;
 unsigned int resets_released;
 const char *stage;
 int error;
};
struct qcom_usb4_nhi_stop { bool finished, ctl_closed, irq_disabled, callbacks_drained; int error;
 unsigned int rings_disabled, rx_options, tx_options; };
struct reset_control_bulk_data { void *rstc; };
struct clk_bulk_data { void *clk; };
struct x1_gcc_usb4 { int unused; };
struct x1_mcu { u32 control_offset; void *base; };
struct x1_port { bool baseline_released; };
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((int)(long)(p))
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define WRITE_ONCE(x, v) ((x) = (v))
struct x1_host {
 struct x1_power_stop power_stop; bool idle_retire_attempted, cold_owned, activated, command_attempted;
 unsigned int connected_command; bool tunnel_held, tunnel_stop_attempted;
 struct { bool attempted; } training, inventory;
 struct { bool inventory_attempted, host_attempted; } pcie;
 bool ready, stopping, suspended, clocks_on, runtime_held, phy_on, phy_initialized, host_stop_attempted;
 struct x1_gcc_usb4 *gcc; struct qcom_usb4_nhi_stop host_stop;
 struct { bool retained_stop_attempted, retained_irq_disabled, accept_rearm; } qnhi;
 struct x1_mcu mcu; void *router, *phy, *tcsr, *apps, *ddr, *dev; struct x1_port *port;
 struct reset_control_bulk_data resets[11]; struct clk_bulk_data clocks[6];
};
static bool x1_general = true;
static struct x1_gcc_usb4 gcc, release_lease;
static struct x1_port port;
static char router[0x30000];
static int put_result, reset_reads, put_calls, disabled;
static int get_error, get_calls, lease_puts, deasserts, deassert_fail_at, release_read_error;
static u32 release_state;
static int x1_gcc_usb4_reset_state(struct x1_gcc_usb4 *g, u32 *state) {
 if (g == &release_lease) {  /* The stop's own lease is gone; the clocks are off and the vote is held. */
  assert(disabled == 1 && !put_calls && deasserts);
  if (release_read_error) return release_read_error;
  *state = release_state; return 0;
 }
 assert(g == &gcc); *state = reset_reads++ ? 0x7ff : 0; return 0;
}
static struct x1_gcc_usb4 *x1_gcc_usb4_get(void *dev, unsigned int p) {
 (void)dev; assert(!p && disabled == 1 && !put_calls); get_calls++;
 return get_error ? ERR_PTR(get_error) : &release_lease;
}
static int reset_control_deassert(void *r) {
 (void)r; assert(get_calls == 1 && !put_calls);
 if (deassert_fail_at && deasserts + 1 == deassert_fail_at) return -EIO;
 deasserts++; return 0;
}
static int mcu_stops, asserts, phy_offs, phy_exits, route_clears, mem_clears, icc_releases, notes;
#define dev_info(dev, ...) ((void)(dev), notes++)
static void qcom_usb4_mcu_stop(struct x1_mcu *m) { (void)m; mcu_stops++; }
static u32 readl(void *addr) { (void)addr; return 0; }
static int reset_control_assert(void *r) { (void)r; asserts++; return 0; }
static void usleep_range(unsigned long a, unsigned long b) { (void)a; (void)b; }
static int x1_gcc_usb4_rx_select(struct x1_gcc_usb4 *g, bool phy) { assert(g == &gcc && !phy); return 0; }
static int phy_power_off(void *p) { (void)p; phy_offs++; return 0; }
static int phy_exit(void *p) { (void)p; phy_exits++; return 0; }
static int regmap_update_bits(void *m, u32 reg, u32 mask, u32 val) { (void)m; assert(reg == 0x1b000 && mask == BIT(0) && !val); route_clears++; return 0; }
static int regmap_read(void *m, u32 reg, u32 *v) { (void)m; (void)reg; *v = 0; return 0; }
static int x1_gcc_usb4_sys_force_mem(struct x1_gcc_usb4 *g, bool on) { assert(g == &gcc && !on); mem_clears++; return 0; }
static int icc_set_bw(void *p, int a, int b) { (void)p; assert(!a && !b); icc_releases++; return 0; }
static void x1_gcc_usb4_put(struct x1_gcc_usb4 *g) {
 if (g == &release_lease) { assert(!put_calls); lease_puts++; return; }
 assert(g == &gcc);
}
static void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *c) { (void)c; assert(n == 6); disabled++; }
static int pm_runtime_put_sync(void *d) { (void)d; put_calls++; return put_result; }
static struct x1_host host;
static void reset(int result) {
 memset(&host, 0, sizeof(host));
 host.idle_retire_attempted = host.cold_owned = host.activated = true;
 host.stopping = host.clocks_on = host.runtime_held = host.phy_on = host.phy_initialized = true;
 host.host_stop_attempted = true; host.gcc = &gcc; host.router = router;
 host.host_stop = (struct qcom_usb4_nhi_stop){ true, true, true, true, 0, 2, 0, 0 };
 host.qnhi.retained_stop_attempted = host.qnhi.retained_irq_disabled = true;
 host.mcu = (struct x1_mcu){ 0x22000, router };
 host.port = &port; port.baseline_released = false;
 put_result = result; reset_reads = put_calls = disabled = 0;
 mcu_stops = asserts = phy_offs = phy_exits = route_clears = mem_clears = icc_releases = notes = 0;
 get_error = get_calls = lease_puts = deasserts = deassert_fail_at = release_read_error = 0;
 release_state = 0;
}
static int x1_idle_power_stop_run(struct x1_host *host);
static void release_fails(int want, int released) {
 struct x1_power_stop *s = &host.power_stop;
 assert(x1_idle_power_stop_run(&host) == want);
 assert(s->error == want && !s->finished && !strcmp(s->stage, "reset-release"));
 assert(!put_calls && host.runtime_held && !s->runtime_released);  /* The domain stays on. */
 assert(!port.baseline_released && (int)s->resets_released == released && s->resets_asserted == 11);
 assert(get_calls == 1 && lease_puts == (get_error ? 0 : 1));
}
'''

CASES = r'''
static void passes(int result) {
 reset(result);
 assert(!x1_idle_power_stop_run(&host) && put_calls == 1 && disabled == 1);
 struct x1_power_stop *s = &host.power_stop;
 assert(s->attempted && s->finished && !s->error && s->runtime_released && !host.runtime_held);
 assert(!strcmp(s->stage, "provider-votes-released-idle") && !host.gcc && !host.clocks_on);
 assert(!host.phy_on && !host.phy_initialized && s->resets_asserted == 11 && s->reset_requests == 0x7ff);
 assert(s->resets_released == 11 && !s->release_requests && deasserts == 11);
 assert(get_calls == 1 && lease_puts == 1 && port.baseline_released);
 assert(x1_idle_power_stop_run(&host) == -EALREADY);
}
int main(void) {
 passes(0);
 passes(1);
 passes(-EBUSY);  /* The domain child is still registered: suspend deferred. */
 passes(-EAGAIN);
 reset(-EINVAL);
 assert(x1_idle_power_stop_run(&host) == -EINVAL);
 assert(host.power_stop.error == -EINVAL && !host.power_stop.finished && host.power_stop.runtime_released);
 assert(!strcmp(host.power_stop.stage, "runtime-vote-release"));
 reset(0); host.stopping = false;
 assert(x1_idle_power_stop_run(&host) == -EPERM && !host.power_stop.attempted && !put_calls);
 reset(0); host.command_attempted = true;
 assert(x1_idle_power_stop_run(&host) == -EPERM && !host.power_stop.attempted);
 reset(0); release_state = BIT(8); release_fails(-EIO, 11);       /* AHB still held on readback */
 assert(host.power_stop.release_requests == BIT(8));
 reset(0); release_read_error = -ENODEV; release_fails(-ENODEV, 11);
 reset(0); deassert_fail_at = 6; release_fails(-EIO, 5);
 reset(0); get_error = -EBUSY; release_fails(-EBUSY, 0);
 assert(!deasserts);
 printf("PASS idle power stop: deferred runtime suspend (-EBUSY/-EAGAIN) completes, other errors fail\n");
 printf("PASS idle power stop: eleven resets released and read back before the vote goes; 4 release faults keep the vote\n");
 return 0;
}
'''


class IdlePowerStop(unittest.TestCase):
    def test_deferred_runtime_suspend(self):
        text = HOST.read_text()
        code = PRELUDE + c_function(text, 'x1_idle_session') + '\n' + \
            c_function(text, 'x1_stop_release_resets') + '\n' + \
            c_function(text, 'x1_idle_power_stop_run') + CASES
        print(build_and_run(code, 'x1-idle-power-stop-'), end='')


if __name__ == '__main__':
    unittest.main()
