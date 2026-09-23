#!/usr/bin/env python3
"""The router 0 power domain stays on, and activation checks it in hardware.

On hardware (2026-09-23) gcc_usb4_0_gdsc never powered on again once switched
off after a router stop, even after a clean Eject. omarchy6 marks it
ALWAYS_ON in the GCC driver and refuses router register access unless the
GDSC reports powered up. Runs the real lease check against a mocked regmap.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import build_and_run  # noqa: E402
from test_idle_retire import COMMON, c_function, HOST  # noqa: E402

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

    def test_gdsc_always_on(self):
        text = GCC.read_text()
        start = text.index('static struct gdsc gcc_usb4_0_gdsc = {')
        block = text[start:text.index('};', start)]
        self.assertIn('.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE | ALWAYS_ON,', block)
        other = text[text.index('static struct gdsc gcc_usb4_1_gdsc = {'):]
        self.assertNotIn('ALWAYS_ON', other[:other.index('};')])

    def test_activation_checks_before_router_access(self):
        body = c_function(HOST.read_text(), 'x1_activate')
        check = body.index('ret = x1_gcc_usb4_power_on(host->gcc);')
        self.assertLess(body.rindex('host->gcc = x1_gcc_usb4_get(dev, 0);', 0, check), check)
        self.assertLess(check, body.index('ret = x1_startup_run('))
        self.assertIn('if (x1_general) {\n\t\tret = x1_gcc_usb4_power_on(host->gcc);\n\t\tif (ret)\n'
                      '\t\t\tgoto release;', body)


if __name__ == '__main__':
    unittest.main()
