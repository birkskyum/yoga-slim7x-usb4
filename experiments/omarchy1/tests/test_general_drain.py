#!/usr/bin/env python3
"""Run the actual x1_general_drain_events from the omarchy host driver with mocks."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import function, build_and_run  # noqa: E402

SOURCE = Path(__file__).resolve().parent.parent / 'kernel/drivers/thunderbolt/qcom-usb4-x1-host.c'
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define HZ 100
#define time_after(a,b) ((long)((b)-(a)) < 0)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define lockdep_assert_held(p) assert(*(p))
#define dev_warn(d, ...) (warned++)
#define dev_info(d, ...) (infos++)
struct device { int unused; };
struct tb { int lock; };
struct x1_host { struct tb *tb; struct device *dev; };
static unsigned long jiffies;
static int warned, infos, unlocks, lives, live_result, polls;
static unsigned int script[8]; static int script_len;
static struct tb tb; static struct device dev; static struct x1_host host = { &tb, &dev };
static unsigned int tb_x1_events_pending(struct tb *t) {
 assert(t==&tb && tb.lock);
 unsigned int v = polls < script_len ? script[polls] : script[script_len-1]; polls++; return v;
}
static void mutex_unlock(int *l) { assert(*l); *l=0; unlocks++; }
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void msleep(unsigned int ms) { assert(!tb.lock); jiffies += ms * HZ / 1000; }
static int x1_batch_live(struct x1_host *h) { assert(h==&host && tb.lock); lives++; return live_result; }
'''
CASES = r'''
static void init(unsigned int *values, int n, int live) {
 tb.lock=1; jiffies=1000; warned=infos=unlocks=lives=polls=0; live_result=live; script_len=n;
 for(int i=0;i<n;i++) script[i]=values[i];
}
int main(void) {
 unsigned int none[]={0}; init(none,1,0);
 assert(!x1_general_drain_events(&host) && tb.lock && !unlocks && lives==1 && !infos);
 unsigned int some[]={2,1,0}; init(some,3,0);
 assert(!x1_general_drain_events(&host) && tb.lock && unlocks==2 && lives==1 && infos==1);
 unsigned int stuck[]={1}; init(stuck,1,0);
 assert(x1_general_drain_events(&host)==-EBUSY && tb.lock && !lives && warned==1 && unlocks>=240);
 init(some,3,-ENOLINK);
 assert(x1_general_drain_events(&host)==-ENOLINK && tb.lock && lives==1);
 printf("PASS drain: no-op, drain then live recheck, 5 s bound, live failure propagates\n");
 return 0;
}
'''


class Drain(unittest.TestCase):
    def test_drain_before_hold(self):
        code = PRELUDE + function(SOURCE.read_text(), 'x1_general_drain_events') + CASES
        print(build_and_run(code, 'x1-general-drain-'), end='')

    def test_harness_hold_path_unchanged(self):
        text = SOURCE.read_text()
        start = text.index('if (!ret && x1_tunnel_quiesce) {')
        block = text[start:text.index('mutex_unlock(&host->tb->lock);', start)]
        self.assertIn('if (x1_general)\n\t\t\tret = x1_general_drain_events(host);', block)
        self.assertIn('if (!ret)\n\t\t\tret = tb_x1_hold_pcie(', block)


if __name__ == '__main__':
    unittest.main()
