#!/usr/bin/env python3
"""The router 0 power domain may go off, and a restart can power it on again.

On hardware (2026-09-23) gcc_usb4_0_gdsc never powered on again once switched
off after a router stop, even after a clean Eject. omarchy6 marked it
ALWAYS_ON in the GCC driver and refuses router register access unless the
GDSC reports powered up.

The domain does not power up while GCC_PCIE_0_TUNNEL_BCR is asserted
(hardware, 2026-10-08: with it held GDSCR stops at 0x6822f800, with it
released first the same power-on completes). The platform retirement holds
that reset, and the start asked for the router's domain before the PCIe
preparation released it. The Omarchy flavor now drops ALWAYS_ON, holds none
of the router's eleven resets across the power-off, and releases a held
tunnel reset before it asks for the domain. The harness flavor is unchanged.

That alone was not enough (hardware, 2026-10-08, boot 5023f1ed). Gating the
system clock leaves its selector on 2, which on this selector is a PHY clock
(0 is the internal source; only on P2RR2P is 2 the reference), and the domain
does not power up again after it went off like that. With the selector moved
to 0 before the power-off it does (boots 59a4d4ab and 69f50f94). The GCC
driver's power-off moves every unheld selector first.
Runs the real lease check, reset restore, tunnel reset release and power-off
against mocks.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import build_and_run  # noqa: E402
from test_idle_retire import COMMON, c_function, HOST, PCIE  # noqa: E402

GCC = HOST.parent.parent / 'clk/qcom/gcc-x1e80100.c'

PRELUDE = COMMON + r'''
typedef unsigned int u32;
#define BIT(n) (1U << (n))
#define guard(type) guard_##type
static void guard_mutex(int *l) { (void)l; }
struct gdsc { u32 gdscr; };
static struct gdsc gcc_usb4_0_gdsc = { 0x9f004 };
struct x1_gcc_usb4 { int unused; };
static int x1_usb4_gcc_lock;
static int regmap_obj, read_error;
static void *x1_usb4_gcc_regmap = &regmap_obj;
static struct x1_gcc_usb4 lease, other;
static u32 regs[2];
static bool x1_usb4_gcc_valid(struct x1_gcc_usb4 *l) { return l == &lease && x1_usb4_gcc_regmap; }
static int regmap_read(void *map, u32 reg, u32 *val) {
 assert(map == &regmap_obj && (reg == 0x9f004 || reg == 0x9f008));
 if (read_error) return read_error;
 *val = regs[reg == 0x9f008]; return 0;
}
int x1_gcc_usb4_power_on(struct x1_gcc_usb4 *lease);
'''

RESTORE_PRELUDE = COMMON + r'''
typedef unsigned int u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) >> (31 - (h))) & ((~0U) << (l)))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
struct reset_control_bulk_data { void *rstc; };
struct x1_gcc_usb4 { int unused; };
struct x1_port { bool baseline_released; };
struct x1_host {
 int lifecycle_lock; unsigned long long generation; struct x1_gcc_usb4 *gcc; struct x1_port *port;
 bool runtime_held, clocks_on, activated, reset_restore_attempted, reset_restore_complete;
 u32 reset_restore_state; int reset_restore_error;
 struct reset_control_bulk_data resets[11];
};
static bool x1_general, x1_managed;
static struct x1_gcc_usb4 gcc;
static struct x1_port port;
static struct x1_host host;
static u32 state;
static int deasserts, read_error, fail_at;
static bool stuck;
static int x1_gcc_usb4_reset_state(struct x1_gcc_usb4 *g, u32 *s) {
 assert(g == &gcc); if (read_error) return read_error; *s = state; return 0;
}
static int reset_control_deassert(void *r) {
 (void)r; if (fail_at && deasserts + 1 == fail_at) return -EIO;
 if (++deasserts == 11 && !stuck) state = 0;
 return 0;
}
static void usleep_range(unsigned long a, unsigned long b) { (void)a; (void)b; }
static void reset(bool general, bool held, u32 st, bool released) {
 host = (struct x1_host){ .lifecycle_lock = 1, .generation = 2, .gcc = &gcc, .port = &port,
                          .runtime_held = held };
 x1_general = general; x1_managed = true; port.baseline_released = released;
 state = st; deasserts = read_error = fail_at = 0; stuck = false;
}
'''

RESTORE_CASES = r'''
int main(void) {
 /* Omarchy flavor, before the domain is asked for: a held stop baseline is released. */
 reset(true, false, 0x7ff, false);
 assert(!x1_restore_resets(&host) && deasserts == 11 && !state);
 assert(host.reset_restore_attempted && host.reset_restore_complete && !host.reset_restore_error);
 /* The idle stop already released them: nothing is written, the receipt is used once. */
 reset(true, false, 0, true);
 assert(!x1_restore_resets(&host) && !deasserts && !port.baseline_released);
 assert(!host.reset_restore_attempted && host.reset_restore_complete);
 /* Released without that receipt is not a baseline this driver left. */
 reset(true, false, 0, false);
 assert(x1_restore_resets(&host) == -EIO && !deasserts && host.reset_restore_error == -EIO);
 assert(!host.reset_restore_attempted && !host.reset_restore_complete);
 reset(true, false, 0x3ff, true);
 assert(x1_restore_resets(&host) == -EIO && !deasserts && port.baseline_released);
 /* Never with the domain already held: that order is what got stuck. */
 reset(true, true, 0x7ff, false);
 assert(x1_restore_resets(&host) == -EPERM && !deasserts);
 reset(true, false, 0x7ff, false); fail_at = 4;
 assert(x1_restore_resets(&host) == -EIO && deasserts == 3);
 assert(host.reset_restore_attempted && !host.reset_restore_complete);
 reset(true, false, 0x7ff, false); stuck = true;
 assert(x1_restore_resets(&host) == -EIO && deasserts == 11 && !host.reset_restore_complete);
 reset(true, false, 0x7ff, false); read_error = -ENODEV;
 assert(x1_restore_resets(&host) == -ENODEV && !deasserts);
 reset(true, false, 0x7ff, false); host.generation = 1;
 assert(x1_restore_resets(&host) == -EPERM);
 reset(true, false, 0x7ff, false); host.clocks_on = true;
 assert(x1_restore_resets(&host) == -EPERM);
 /* Harness flavor unchanged: domain held, asserted baseline only, no receipt path. */
 reset(false, true, 0x7ff, false);
 assert(!x1_restore_resets(&host) && deasserts == 11 && host.reset_restore_complete);
 reset(false, false, 0x7ff, false);
 assert(x1_restore_resets(&host) == -EPERM && !deasserts);
 reset(false, true, 0, true);
 assert(x1_restore_resets(&host) == -EIO && !deasserts && port.baseline_released);
 printf("PASS reset restore: Omarchy flavor before the domain, released receipt used once, 8 refusals and faults; harness unchanged\n");
 return 0;
}
'''

SELECT_PRELUDE = COMMON + r'''
typedef unsigned int u32;
#define BIT(n) (1U << (n))
struct clk_hw { int id; };
struct clk_regmap { struct clk_hw hw; };
struct clk_regmap_phy_mux { struct clk_regmap clkr; };
struct generic_pm_domain { int unused; };
struct clk_ops { int (*enable)(struct clk_hw *); void (*disable)(struct clk_hw *); int (*is_enabled)(struct clk_hw *); };
enum { SYS, DP0, DP1, P2RR2P, PCIE };
static int value[5];  /* The register field: 0 or 2. */
static bool prepared[5], write_ignored[5];
static int writes;
static int mux_enable(struct clk_hw *hw) { writes++; if (!write_ignored[hw->id]) value[hw->id] = 0; return 0; }
static void mux_disable(struct clk_hw *hw) { writes++; if (!write_ignored[hw->id]) value[hw->id] = 2; }
static int mux_is_enabled(struct clk_hw *hw) { return value[hw->id] == 0; }
static const struct clk_ops clk_regmap_phy_mux_ops = { mux_enable, mux_disable, mux_is_enabled };
static bool clk_hw_is_prepared(struct clk_hw *hw) { return prepared[hw->id]; }
static struct clk_regmap_phy_mux gcc_usb4_0_phy_sys_clk_src = {{{SYS}}}, gcc_usb4_0_phy_dp0_clk_src = {{{DP0}}},
 gcc_usb4_0_phy_dp1_clk_src = {{{DP1}}}, gcc_usb4_0_phy_p2rr2p_pipe_clk_src = {{{P2RR2P}}};
static struct generic_pm_domain pd;
static int off_result, off_calls, at_off[5];
static int real_off(struct generic_pm_domain *d) { assert(d == &pd); off_calls++; memcpy(at_off, value, sizeof(value)); return off_result; }
'''

SELECT_CASES = r'''
/* As read on the Yoga after an idle stop: the system selector on 2, P2RR2P on the reference. */
static void observed(void) {
 int state[5] = { 2, 0, 0, 2, 0 };
 memcpy(value, state, sizeof(state)); memset(prepared, 0, sizeof(prepared)); memset(write_ignored, 0, sizeof(write_ignored));
 x1_usb4_0_gdsc_off = real_off; off_result = off_calls = writes = 0; memset(at_off, -1, sizeof(at_off));
}
static bool is(int sys, int dp0, int dp1, int p2rr2p) {
 return value[SYS] == sys && value[DP0] == dp0 && value[DP1] == dp1 && value[P2RR2P] == p2rr2p && value[PCIE] == 0;
}
int main(void) {
 observed();
 assert(!x1_usb4_0_gdsc_power_off(&pd) && off_calls == 1 && writes == 1);
 assert(at_off[SYS] == 0 && at_off[DP0] == 0 && at_off[DP1] == 0 && at_off[P2RR2P] == 2);
 assert(is(0, 0, 0, 2));  /* Left there: the next start selects 0 anyway. */
 /* Nothing to do the second time. */
 writes = 0; assert(!x1_usb4_0_gdsc_power_off(&pd) && !writes);
 /* The domain refuses to go off: back to how it was found. */
 observed(); off_result = -EBUSY;
 assert(x1_usb4_0_gdsc_power_off(&pd) == -EBUSY && off_calls == 1 && is(2, 0, 0, 2));
 /* A consumer holds a selector that sits on a PHY clock: nothing is written, the domain stays on. */
 observed(); prepared[SYS] = true;
 assert(x1_usb4_0_gdsc_power_off(&pd) == -EBUSY && !off_calls && !writes && is(2, 0, 0, 2));
 observed(); value[SYS] = 0; prepared[SYS] = true;  /* Held on the internal source is no obstacle. */
 assert(!x1_usb4_0_gdsc_power_off(&pd) && off_calls == 1 && !writes);
 /* A selector that does not take the write: the domain stays on. */
 observed(); write_ignored[SYS] = true;
 assert(x1_usb4_0_gdsc_power_off(&pd) == -EIO && !off_calls && is(2, 0, 0, 2));
 /* DP selectors parked the helper's way, and a P2RR2P left on the PHY. */
 observed(); value[DP0] = value[DP1] = 2; value[P2RR2P] = 0;
 assert(!x1_usb4_0_gdsc_power_off(&pd) && writes == 4 && is(0, 0, 0, 2) && at_off[P2RR2P] == 2);
 observed(); value[DP1] = 2; write_ignored[DP1] = true;  /* The system one is put back when a later one fails. */
 assert(x1_usb4_0_gdsc_power_off(&pd) == -EIO && !off_calls && is(2, 0, 2, 2));
 printf("PASS selectors before power-off: unheld ones moved to their PHY-independent source, 4 refusals and faults leave the domain on and the selectors as found\n");
 return 0;
}
'''

CASES = r'''
int main(void) {
 regs[0] = BIT(31) | 0x2; regs[1] = BIT(16);
 assert(!x1_gcc_usb4_power_on(&lease));
 regs[0] = 0x2; assert(x1_gcc_usb4_power_on(&lease) == -ENODEV);          /* switch off */
 regs[0] = BIT(31); regs[1] = BIT(15); assert(x1_gcc_usb4_power_on(&lease) == -ENODEV); /* not complete */
 regs[1] = BIT(16); read_error = -EIO; assert(x1_gcc_usb4_power_on(&lease) == -EIO);
 read_error = 0; assert(x1_gcc_usb4_power_on(&other) == -EPERM);
 assert(x1_gcc_usb4_power_on(NULL) == -EPERM);
 printf("PASS power domain check: on only with PWR_ON and power-up complete, leases enforced\n");
 return 0;
}
'''


class PowerDomain(unittest.TestCase):
    def test_lease_power_check(self):
        code = PRELUDE + c_function(GCC.read_text(), 'x1_gcc_usb4_power_on') + CASES
        print(build_and_run(code, 'x1-power-domain-'), end='')

    def test_gdsc_not_always_on(self):
        text = GCC.read_text()
        for name in ('gcc_usb4_0_gdsc', 'gcc_usb4_1_gdsc'):
            start = text.index('static struct gdsc ' + name + ' = {')
            block = text[start:text.index('};', start)]
            self.assertIn('.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE,', block, name)
            self.assertNotIn('ALWAYS_ON', block, name)

    def test_selectors_moved_before_power_off(self):
        text = GCC.read_text()
        start = text.index('} x1_usb4_0_selectors[] = {')
        table = text[start:text.index('};', start)]
        for name, zero in (('sys_clk_src', 'true'), ('dp0_clk_src', 'true'), ('dp1_clk_src', 'true'),
                           ('p2rr2p_pipe_clk_src', 'false')):
            self.assertIn('{ &gcc_usb4_0_phy_' + name + ', ' + zero + ' },', table)
        self.assertNotIn('pcie_pipe_mux', table)  # It has no input 2 and keeps its internal source.
        state = text[text.index('static const struct {\n\tstruct clk_regmap_phy_mux *mux;'):
                     text.index('static void x1_usb4_0_select(')]
        code = SELECT_PRELUDE + state + '\n'.join(c_function(text, name) for name in (
            'x1_usb4_0_select', 'x1_usb4_0_gdsc_power_off')) + SELECT_CASES
        print(build_and_run(code, 'x1-selectors-'), end='')

    def test_probe_installs_the_power_off_only(self):
        text = GCC.read_text()
        body = c_function(text, 'gcc_x1e80100_probe')
        probe = body.index('ret = qcom_cc_really_probe(')
        save = body.index('x1_usb4_0_gdsc_off = gcc_usb4_0_gdsc.pd.power_off;')
        self.assertLess(probe, save)
        self.assertLess(save, body.index('gcc_usb4_0_gdsc.pd.power_off = x1_usb4_0_gdsc_power_off;'))
        # Nothing is done around the power-on, and nothing is switchable or logged.
        for name in ('pd.power_on', 'x1_usb4_0_need', 'x1_usb4_0_restore', 'pr_info("gcc_usb4_0_gdsc'):
            self.assertNotIn(name, text)

    def test_restore_resets(self):
        code = RESTORE_PRELUDE + c_function(HOST.read_text(), 'x1_restore_resets') + RESTORE_CASES
        print(build_and_run(code, 'x1-reset-restore-'), end='')

    def test_omarchy_flavor_restores_before_power_on(self):
        body = c_function(HOST.read_text(), 'x1_activate')
        early = body.index('if (x1_general && host->generation > 1) {')
        restore = body.index('ret = x1_restore_resets(host);')
        resume = body.index('ret = pm_runtime_resume_and_get(dev);')
        self.assertLess(early, body.index('host->gcc = x1_gcc_usb4_get(dev, 0);'))
        self.assertLess(body.index('host->gcc = x1_gcc_usb4_get(dev, 0);'), restore)
        self.assertLess(restore, resume)
        # A failed power-on puts the early lease back through the release path.
        self.assertIn('if (ret < 0) {\n\t\t/* Only the Omarchy flavor holds anything yet: its early lease. */\n'
                      '\t\tif (host->gcc)\n\t\t\tgoto release;\n\t\tgoto stopped;\n\t}', body)
        # The harness flavor keeps its order: domain first, then the restore.
        late = body.index('if (!x1_general && host->generation > 1) {')
        self.assertLess(resume, late)
        self.assertLess(late, body.index('ret = x1_restore_resets(host);', late))
        self.assertEqual(body.count('ret = x1_restore_resets(host);'), 2)

    def test_tunnel_reset_released_before_the_router_domain(self):
        """Omarchy flavor only: after the MISC resets and before the domain is asked for; a refusal takes the release path."""
        body = c_function(HOST.read_text(), 'x1_activate')
        call = 'ret = qcom_usb4_x1_release_retired_reset(dev);\n\t\tif (ret)\n\t\t\tgoto release;'
        self.assertEqual(body.count('qcom_usb4_x1_release_retired_reset('), 1)
        self.assertLess(body.index('if (x1_general && host->generation > 1) {'), body.index(call))
        self.assertLess(body.index('ret = x1_restore_resets(host);'), body.index(call))
        self.assertLess(body.index(call), body.index('ret = pm_runtime_resume_and_get(dev);'))
        prelude = COMMON + r'''
#define X1_PCIE_NODE "/soc@0/pcie-imsi@400000000"
#define X1_PCIE_COMPAT "birk,yoga-x1-pci0-imsi-local"
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((int)(long)(p))
#define IS_ERR_OR_NULL(p) (!(p) || (unsigned long)(p) >= (unsigned long)-4095)
struct device { int refs; };
struct platform_device { struct device dev; };
struct device_node { int puts; };
struct reset_control { int deasserts, puts; };
struct x1_pcie_context { int unused; };
static int x1_pcie_lock, get_result, deassert_result, sleeps;
static bool x1_pcie_reset_held, node_present, compatible, device_present;
static struct x1_pcie_context *x1_pcie, ctx;
static struct device owner; static struct platform_device pdev; static struct device_node node; static struct reset_control rst;
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static struct device_node *of_find_node_by_path(const char *path) { assert(x1_pcie_lock && !strcmp(path, X1_PCIE_NODE)); return node_present ? &node : NULL; }
static bool of_device_is_compatible(struct device_node *n, const char *c) { assert(n == &node && !strcmp(c, X1_PCIE_COMPAT)); return compatible; }
static struct platform_device *of_find_device_by_node(struct device_node *n) { assert(n == &node); if (!device_present) return NULL; pdev.dev.refs++; return &pdev; }
static void of_node_put(struct device_node *n) { if (n) n->puts++; }
static struct reset_control *reset_control_get_exclusive(struct device *d, const char *id) {
 assert(d == &pdev.dev && !strcmp(id, "tunnel")); return get_result ? ERR_PTR(get_result) : &rst;
}
static int reset_control_deassert(struct reset_control *r) { assert(r == &rst && !r->puts); if (!deassert_result) r->deasserts++; return deassert_result; }
static void reset_control_put(struct reset_control *r) { assert(r == &rst); r->puts++; }
static void put_device(struct device *d) { assert(d == &pdev.dev && d->refs > 0); d->refs--; }
static void msleep(unsigned int ms) { assert(ms == 5 && rst.deasserts == 1 && rst.puts == 1); sleeps++; }
static void start(void) {
 x1_pcie_reset_held = node_present = compatible = device_present = true; x1_pcie = NULL;
 get_result = deassert_result = sleeps = 0; pdev.dev.refs = 0; node.puts = 0; rst = (struct reset_control){ 0, 0 };
}
'''
        cases = r'''
int main(void) {
 /* A reset the retirement holds: released, the lease and the device put again, the hold forgotten. */
 start(); assert(!qcom_usb4_x1_release_retired_reset(&owner) && rst.deasserts == 1 && rst.puts == 1 && sleeps == 1);
 assert(!x1_pcie_reset_held && !pdev.dev.refs && node.puts == 1 && !x1_pcie_lock);
 /* Nothing held, as at the first start of a boot: nothing is looked up. */
 start(); x1_pcie_reset_held = false; node_present = false;
 assert(!qcom_usb4_x1_release_retired_reset(&owner) && !rst.deasserts && !node.puts && !x1_pcie_lock);
 /* A context that was not retired still owns the reset. */
 start(); x1_pcie = &ctx; assert(qcom_usb4_x1_release_retired_reset(&owner) == -EBUSY && !rst.deasserts && x1_pcie_reset_held);
 start(); node_present = false; assert(qcom_usb4_x1_release_retired_reset(&owner) == -ENODEV && x1_pcie_reset_held);
 start(); compatible = false; assert(qcom_usb4_x1_release_retired_reset(&owner) == -ENODEV && node.puts == 1 && x1_pcie_reset_held);
 start(); device_present = false; assert(qcom_usb4_x1_release_retired_reset(&owner) == -ENODEV && node.puts == 1 && x1_pcie_reset_held);
 start(); get_result = -EBUSY; assert(qcom_usb4_x1_release_retired_reset(&owner) == -EBUSY && !rst.puts && !pdev.dev.refs && x1_pcie_reset_held);
 /* A release that fails keeps the hold recorded, and the lease is still put. */
 start(); deassert_result = -EIO;
 assert(qcom_usb4_x1_release_retired_reset(&owner) == -EIO && rst.puts == 1 && !sleeps && x1_pcie_reset_held && !pdev.dev.refs && !x1_pcie_lock);
 assert(qcom_usb4_x1_release_retired_reset(NULL) == -EINVAL);
 printf("PASS tunnel reset before the router's domain: a held one is released once, 7 refusals and faults keep the hold\n");
 return 0;
}
'''
        print(build_and_run(prelude + c_function(PCIE.read_text(), 'qcom_usb4_x1_release_retired_reset') + cases, 'x1-tunnel-early-'), end='')

    def test_idle_stop_releases_before_the_vote(self):
        body = c_function(HOST.read_text(), 'x1_idle_power_stop_run')
        clocks = body.index('clk_bulk_disable_unprepare(ARRAY_SIZE(host->clocks), host->clocks);')
        release = body.index('ret = x1_stop_release_resets(host);')
        self.assertLess(clocks, release)
        self.assertLess(release, body.index('ret = pm_runtime_put_sync(host->dev);'))
        self.assertIn('s->resets_released != ARRAY_SIZE(host->resets) || s->release_requests',
                      c_function(HOST.read_text(), 'x1_idle_owner_stopped'))

    def test_activation_checks_before_router_access(self):
        body = c_function(HOST.read_text(), 'x1_activate')
        check = body.index('ret = x1_gcc_usb4_power_on(host->gcc);')
        self.assertLess(body.rindex('host->gcc = x1_gcc_usb4_get(dev, 0);', 0, check), check)
        self.assertLess(check, body.index('ret = x1_startup_run('))
        self.assertIn('if (x1_general) {\n\t\tret = x1_gcc_usb4_power_on(host->gcc);\n\t\tif (ret)\n'
                      '\t\t\tgoto release;', body)


if __name__ == '__main__':
    unittest.main()
