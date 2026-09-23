#!/usr/bin/env python3
"""Run the actual omarchy-branch idle power-down functions with mocks; no hardware.

Covers the sleep path for a prepared router that never connected: the CM
freeze and root removal (tb.c), the refusable freeze wait and the store's
order and failure stages (host.c), the domain release, the suspend gate and
the PCIe0 platform release (pcie.c). Also checks that the harness functions
the idle path reuses are unchanged from the omarchy branch before it.
"""
from pathlib import Path
import re
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import build_and_run  # noqa: E402

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / 'kernel'
TB = SRC / 'drivers/thunderbolt/tb.c'
HOST = SRC / 'drivers/thunderbolt/qcom-usb4-x1-host.c'
PCIE = SRC / 'drivers/thunderbolt/qcom-usb4-x1-pcie.c'


def c_function(text, name):
    match = re.search(r'^[A-Za-z_][\w \*]*?\b' + name + r'\([^;{]*\)\s*\{', text, re.M)
    if not match:
        raise ValueError(name)
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define lockdep_assert_held(p) assert(*(p))
#define smp_store_release(p, v) (*(p) = (v))
static int calls[64], ncalls;
static void note(int c) { assert(ncalls < 64); calls[ncalls++] = c; }
static bool order(const int *want, int n) {
 if (n != ncalls) return false;
 for (int i = 0; i < n; i++) if (calls[i] != want[i]) return false;
 return true;
}
'''

TB_PRELUDE = COMMON + r'''
#define TB_SECURITY_NOPCIE 5
struct tb_port;
struct list_head { int count; struct tb_port *items[4]; };
#define list_empty(h) (!(h)->count)
#define list_for_each_entry(pos, head, member) \
 for (int _i = 0; _i < (head)->count && ((pos) = (head)->items[_i], 1); _i++)
struct device { bool registered; };
struct tb_switch;
struct tb_port { struct tb_switch *sw; void *remote, *xdomain; };
struct tb_switch { struct device dev; bool is_unplugged; int refs, max_port; struct tb_port ports[4]; };
#define tb_switch_for_each_port(s, p) for ((p) = &(s)->ports[1]; (p) <= &(s)->ports[(s)->max_port]; (p)++)
struct tb_nhi { bool going_away; };
struct tb_ctl { int unused; };
struct tb_cm_ops { int unused; };
struct workqueue_struct { int unused; };
static const struct tb_cm_ops tb_cm_ops = { 1 }, other_ops = { 2 };
struct tb { int lock; const struct tb_cm_ops *cm_ops; struct tb_nhi *nhi; int security_level;
 struct tb_ctl *ctl; struct tb_switch *root_switch; struct workqueue_struct *wq; };
struct delayed_work { int unused; };
struct tb_bandwidth_group { struct list_head ports; struct delayed_work release_work; };
typedef struct { int counter; } atomic_t;
#define atomic_read(a) ((a)->counter)
struct tb_cm {
 bool x1_retained, x1_idle_frozen, hotplug_active, x1_hold_attempted, x1_pcie_attempted;
 bool x1_cm_retire_attempted, x1_cm_retired;
 atomic_t x1_pending_events;
 struct list_head tunnel_list, dp_resources;
 struct delayed_work remove_work;
 struct tb_bandwidth_group groups[2];
};
struct tb_x1_cm_retire { bool attempted, workers_drained, tunnel_released, routers_removed, finished;
 int error; const char *stage; };
enum { CHECK = 1, CANCEL, FLUSH, GET, DP, REMOVE, PUT };
static struct tb_cm cm;
#define tb_priv(t) (&cm)
static struct tb tb;
static struct tb_nhi nhi;
static struct tb_ctl ctl;
static struct tb_switch root, other;
static struct tb_port dp_other = { &other };
static struct workqueue_struct wq;
static int owner_result, retired_ctl, keep_registered, stop_result, drain_events, drain_device;
static int tb_x1_pcie_owner(struct tb *t) { assert(t == &tb && tb.lock); return owner_result; }
static bool tb_x1_retained_mode(struct tb *t) { return tb_priv(t)->x1_retained; }
static bool tb_ctl_x1_retired(struct tb_ctl *c, struct tb_nhi *n) { return retired_ctl && c == &ctl && n == &nhi; }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void cancel_delayed_work_sync(struct delayed_work *w) {
 (void)w; assert(!tb.lock); note(CANCEL);
 cm.x1_pending_events.counter += drain_events; drain_events = 0;
 if (drain_device) root.ports[2].remote = &other;
}
static void flush_workqueue(struct workqueue_struct *q) { assert(q == &wq && !tb.lock); note(FLUSH); }
static struct tb_switch *tb_switch_get(struct tb_switch *s) { s->refs++; note(GET); return s; }
static void tb_switch_put(struct tb_switch *s) { assert(s->refs > 1); s->refs--; note(PUT); }
static void tb_remove_dp_resources(struct tb_switch *s) { assert(s == &root && tb.lock); note(DP); }
static void tb_switch_remove_x1_powered_off(struct tb_switch *s) {
 assert(s == &root && !tb.lock && !tb.root_switch && s->is_unplugged); note(REMOVE);
 s->dev.registered = keep_registered;
}
static bool device_is_registered(struct device *d) { return d->registered; }
static int check_stopped(void *c) { assert(c == &tb && tb.lock); note(CHECK); return stop_result; }
int tb_x1_idle_freeze(struct tb *tb);
static void reset(bool frozen) {
 memset(&cm, 0, sizeof(cm)); memset(&root, 0, sizeof(root)); memset(&tb, 0, sizeof(tb));
 tb = (struct tb){ 0, &tb_cm_ops, &nhi, TB_SECURITY_NOPCIE, &ctl, &root, &wq };
 root.dev.registered = true; root.max_port = 3; root.refs = 1;
 for (int i = 0; i < 4; i++) root.ports[i].sw = &root;
 cm.hotplug_active = true; nhi.going_away = false;
 owner_result = keep_registered = stop_result = drain_events = drain_device = 0; retired_ctl = 1; ncalls = 0;
 if (frozen) {
  tb.lock = 1; assert(!tb_x1_idle_freeze(&tb)); tb.lock = 0;
  nhi.going_away = true;
 }
}
'''

TB_CASES = r'''
static int refuse_freeze(void) {
 tb.lock = 1; int ret = tb_x1_idle_freeze(&tb); tb.lock = 0;
 assert(!cm.x1_retained && !cm.x1_idle_frozen && cm.hotplug_active);
 return ret;
}
static int retire(struct tb_x1_cm_retire *s) { memset(s, 0, sizeof(*s)); return tb_x1_idle_retire_cm(&tb, s, check_stopped, &tb); }
int main(void) {
 struct tb_x1_cm_retire s;
 int refusals = 0;
 reset(false); tb.lock = 1;
 assert(!tb_x1_idle_freeze(&tb) && cm.x1_idle_frozen && cm.x1_retained && !cm.hotplug_active);
 assert(tb_x1_idle_freeze(&tb) == -EALREADY);
 assert(tb_x1_idle_freeze(NULL) == -EINVAL);
 reset(false); owner_result = -ENODEV; assert(refuse_freeze() == -ENODEV); refusals++;
 reset(false); cm.x1_pending_events.counter = 1; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); root.ports[2].remote = &other; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); root.ports[1].xdomain = &other; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); cm.tunnel_list.count = 1; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); root.is_unplugged = true; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); tb.root_switch = NULL; assert(refuse_freeze() == -EBUSY); refusals++;
 reset(false); cm.x1_hold_attempted = true; assert(refuse_freeze() == -EALREADY); refusals++;
 reset(false); cm.x1_pcie_attempted = true; assert(refuse_freeze() == -EALREADY); refusals++;
 reset(false); cm.x1_cm_retire_attempted = true; assert(refuse_freeze() == -EALREADY); refusals++;

 /* Success: two stop proofs around the drain, root removed unlocked, ref balanced. */
 reset(true);
 assert(!retire(&s) && !tb.lock);
 int ok[] = { CHECK, CANCEL, CANCEL, CANCEL, FLUSH, CHECK, GET, DP, REMOVE, PUT };
 assert(order(ok, 10));
 assert(s.attempted && s.workers_drained && s.tunnel_released && s.routers_removed && s.finished && !s.error);
 assert(!strcmp(s.stage, "cm-retired-domain-retained") && cm.x1_cm_retired && cm.x1_cm_retire_attempted);
 assert(!tb.root_switch && root.is_unplugged && !root.dev.registered && root.refs == 1);
 assert(retire(&s) == -EALREADY && !s.attempted);

 /* Refusals before the drain change nothing and never call the proof twice. */
 reset(false); nhi.going_away = true; assert(retire(&s) == -EPERM && !s.attempted && !cm.x1_cm_retire_attempted); refusals++;
 reset(true); nhi.going_away = false; assert(retire(&s) == -EPERM && !s.attempted); refusals++;
 reset(true); retired_ctl = 0; assert(retire(&s) == -EPERM && !s.attempted); refusals++;
 reset(true); tb.cm_ops = &other_ops; assert(retire(&s) == -EPERM && !s.attempted); refusals++;
 reset(true); tb.security_level = 0; assert(retire(&s) == -EPERM && !s.attempted); refusals++;
 reset(true); stop_result = -EIO; assert(retire(&s) == -EIO && !s.attempted && ncalls == 1); refusals++;
 reset(true); cm.groups[1].ports.count = 1; assert(retire(&s) == -EBUSY && !s.attempted); refusals++;
 reset(true); cm.dp_resources.count = 1; cm.dp_resources.items[0] = &dp_other;
 assert(retire(&s) == -EXDEV && !s.attempted); refusals++;
 reset(true); assert(tb_x1_idle_retire_cm(&tb, &s, NULL, &tb) == -EINVAL);

 /* Faults after the drain are terminal and keep the root published. */
 int faults = 0;
 reset(true); drain_events = 1;
 assert(retire(&s) == -EBUSY && s.attempted && s.workers_drained && s.error == -EBUSY && !tb.lock);
 assert(tb.root_switch == &root && !cm.x1_cm_retired && root.refs == 1 && !s.routers_removed); faults++;
 reset(true); drain_device = 1;
 assert(retire(&s) == -EBUSY && s.error == -EBUSY && tb.root_switch == &root && root.refs == 1); faults++;
 /* Still registered after removal: keep the extra reference, never finish. */
 reset(true); keep_registered = 1;
 assert(retire(&s) == -EBUSY && s.error == -EBUSY && !s.finished && !s.routers_removed);
 assert(!cm.x1_cm_retired && root.refs == 2 && !tb.lock); faults++;
 printf("PASS tb idle: freeze + %d refusals, ordered root removal, %d faults terminal\n", refusals, faults);
 return 0;
}
'''

FREEZE_PRELUDE = COMMON + r'''
#define HZ 100
#define time_after(a, b) ((long)((b) - (a)) < 0)
struct tb_ctl { int unused; };
struct tb { int lock; struct tb_ctl *ctl; };
struct x1_host { struct tb *tb; };
static unsigned long jiffies;
static struct tb_ctl ctl;
static struct tb tb = { 0, &ctl };
static struct x1_host host = { &tb };
static int pending[4], idle[4], freezes_script[4], npending, nidle, nfreeze;
static int polls_p, polls_i, freezes, sleeps;
static int pick(const int *v, int n, int i) { return i < n ? v[i] : v[n - 1]; }
static unsigned int tb_x1_events_pending(struct tb *t) { assert(t == &tb && tb.lock); return pick(pending, npending, polls_p++); }
static bool tb_ctl_x1_idle(struct tb_ctl *c) { assert(c == &ctl && tb.lock); return pick(idle, nidle, polls_i++); }
static int tb_x1_idle_freeze(struct tb *t) { assert(t == &tb && tb.lock); return pick(freezes_script, nfreeze, freezes++); }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void msleep(unsigned int ms) { assert(!tb.lock); jiffies += ms * HZ / 1000; sleeps++; }
static void setup(const int *p, int np, const int *i, int ni, const int *f, int nf) {
 memcpy(pending, p, np * sizeof(int)); npending = np; memcpy(idle, i, ni * sizeof(int)); nidle = ni;
 memcpy(freezes_script, f, nf * sizeof(int)); nfreeze = nf;
 polls_p = polls_i = freezes = sleeps = 0; jiffies = 5000; tb.lock = 1;
}
'''

FREEZE_CASES = r'''
int main(void) {
 int zero[] = { 0 }, one[] = { 1 }, busy[] = { -EBUSY }, already[] = { -EALREADY };
 setup(zero, 1, one, 1, zero, 1);
 assert(!x1_idle_freeze_cm(&host) && tb.lock && freezes == 1 && !sleeps);
 int drain[] = { 2, 1, 0 };
 setup(drain, 3, one, 1, zero, 1);
 assert(!x1_idle_freeze_cm(&host) && tb.lock && freezes == 1 && sleeps == 2);
 int ctl_busy[] = { 0, 0, 1 };
 setup(zero, 1, ctl_busy, 3, zero, 1);
 assert(!x1_idle_freeze_cm(&host) && tb.lock && freezes == 1 && sleeps == 2);
 setup(one, 1, one, 1, zero, 1);
 assert(x1_idle_freeze_cm(&host) == -EBUSY && tb.lock && !freezes && sleeps >= 150 && sleeps <= 152);
 setup(zero, 1, zero, 1, zero, 1);
 assert(x1_idle_freeze_cm(&host) == -EBUSY && tb.lock && !freezes);
 int late[] = { -EBUSY, 0 };
 setup(zero, 1, one, 1, late, 2);
 assert(!x1_idle_freeze_cm(&host) && freezes == 2 && sleeps == 1);
 setup(zero, 1, one, 1, busy, 1);
 assert(x1_idle_freeze_cm(&host) == -EBUSY && tb.lock && sleeps >= 150 && sleeps <= 152);
 setup(zero, 1, one, 1, already, 1);
 assert(x1_idle_freeze_cm(&host) == -EALREADY && freezes == 1 && !sleeps);
 printf("PASS freeze wait: events and requests drain unlocked, 3 s bound, hard refusals immediate\n");
 return 0;
}
'''

STORE_PRELUDE = COMMON + r'''
#define __free(x)
#define CAP_SYS_ADMIN 21
#define dev_info(d, ...) (infos++)
struct device { int lock; };
struct device_attribute { int unused; };
struct tb_ctl { int unused; };
struct tb { int lock; struct tb_ctl *ctl; };
struct tb_ring { int unused; };
struct tb_nhi { struct tb_ring **tx_rings, **rx_rings; };
struct qcom_usb4_nhi { struct tb_nhi nhi; int ownership_lock; };
struct qcom_usb4_nhi_stop { bool attempted, ctl_closed, irq_disabled, callbacks_drained, finished;
 unsigned int rings_disabled, rx_options, tx_options; int error; const char *stage; };
struct qcom_usb4_nhi_retire { int unused; };
struct x1_pcie_state { bool prepared, inventory_attempted, host_attempted; int error; };
struct x1_host {
 struct device *dev; int lock, bind_lock, lifecycle_lock; struct tb *tb;
 bool activated, cold_owned, ready, stopping, suspended, batch_attempted, batch_waiting, batch_terminal;
 bool command_attempted, tunnel_held, host_stop_attempted, idle_retire_attempted, idle_retired, fully_retired;
 int error; unsigned int connected_command, pending_command; void *batch_switch; const char *batch_step;
 struct { bool attempted; } training, inventory;
 struct x1_pcie_state pcie;
 struct qcom_usb4_nhi_stop host_stop;
 struct qcom_usb4_nhi qnhi;
 struct qcom_usb4_nhi_retire control_retire;
 int idle_retire_error; const char *idle_retire_stage;
};
enum { FREEZE = 1, STOP, POWER, CONTROL, DOMAIN, PLATFORM };
static bool x1_general, x1_managed, x1_host_quiesce, x1_power_quiesce, x1_control_retire, x1_cm_retire, admin, trylock_fail;
static int infos, kfrees, destroyed, freeze_result, stop_result, bad_receipt, power_result;
static int control_result, domain_result, platform_result;
static struct device dev;
static struct tb_ctl ctl;
static struct tb tb;
static struct tb_ring *rings[2];
static struct x1_host host;
static struct x1_host *x1_session_get(struct device *d) { assert(d == &dev); return &host; }
static bool capable(int c) { return c == CAP_SYS_ADMIN && admin; }
static bool sysfs_streq(const char *a, const char *b) {
 size_t n = strlen(a); if (n && a[n - 1] == '\n') n--; return n == strlen(b) && !strncmp(a, b, n);
}
static bool device_trylock(struct device *d) { if (d->lock || trylock_fail) return false; d->lock = 1; return true; }
static void device_unlock(struct device *d) { assert(d->lock); d->lock = 0; }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void mutex_destroy(int *l) { assert(!*l && l == &host.qnhi.ownership_lock); destroyed++; }
static void devm_kfree(struct device *d, void *p) { assert(d == &dev && p == rings); kfrees++; }
static int pm_holds;
static void pm_runtime_get_noresume(struct device *d) { assert(d == &dev && host.idle_retire_attempted && host.lock); pm_holds++; }
static int pm_runtime_put_sync(struct device *d) { assert(d == &dev && pm_holds == 1 && !host.lock); pm_holds--; return 0; }
static bool all_locks(void) { return dev.lock && host.bind_lock && host.lifecycle_lock; }
static int x1_idle_owner_stopped(void *c) { (void)c; return 0; }
static int x1_idle_platform_owner_stopped(void *c) { (void)c; return 0; }
static int x1_idle_freeze_cm(struct x1_host *h) {
 assert(h == &host && all_locks() && h->lock && tb.lock);
 assert(!h->idle_retire_attempted && !h->stopping && h->ready && !h->host_stop_attempted);
 note(FREEZE); return freeze_result;
}
static int tb_ctl_stop_x1_retained(struct tb_ctl *c, struct qcom_usb4_nhi *q, struct qcom_usb4_nhi_stop *s) {
 assert(c == &ctl && q == &host.qnhi && s == &host.host_stop && all_locks() && host.lock && tb.lock);
 assert(host.idle_retire_attempted && host.stopping && !host.ready && host.host_stop_attempted);
 note(STOP);
 if (stop_result) return stop_result;
 s->attempted = s->ctl_closed = s->irq_disabled = s->callbacks_drained = true;
 s->rings_disabled = bad_receipt ? 1 : 2;
 return 0;
}
static int x1_idle_power_stop_run(struct x1_host *h) {
 assert(h == &host && all_locks() && h->lock && tb.lock && h->host_stop.finished && !h->host_stop.error);
 assert(!strcmp(h->host_stop.stage, "control-stopped-idle"));
 note(POWER); return power_result;
}
static int tb_ctl_retire_x1_stopped(struct tb_ctl *c, struct qcom_usb4_nhi *q, const struct qcom_usb4_nhi_stop *st,
  struct qcom_usb4_nhi_retire *r, int (*check)(void *), void *ctx) {
 assert(c == &ctl && q == &host.qnhi && st == &host.host_stop && r == &host.control_retire);
 assert(check == x1_idle_owner_stopped && ctx == &host && all_locks() && !host.lock && !tb.lock);
 note(CONTROL); return control_result;
}
static int x1_idle_retire_domain(struct x1_host *h) {
 assert(h == &host && all_locks() && !h->lock && !tb.lock); note(DOMAIN); return domain_result;
}
static int qcom_usb4_x1_idle_retire_platform(struct device *o, struct x1_pcie_state *s, int (*check)(void *), void *ctx) {
 assert(o == &dev && s == &host.pcie && check == x1_idle_platform_owner_stopped && ctx == &host);
 assert(all_locks() && !host.lock && !tb.lock); note(PLATFORM); return platform_result;
}
static void reset(void) {
 memset(&host, 0, sizeof(host)); memset(&tb, 0, sizeof(tb)); memset(&dev, 0, sizeof(dev));
 tb.ctl = &ctl; host.dev = &dev; host.tb = &tb;
 host.activated = host.cold_owned = host.ready = host.batch_attempted = host.batch_waiting = true;
 host.pcie.prepared = true; host.qnhi.nhi.tx_rings = host.qnhi.nhi.rx_rings = rings;
 x1_general = x1_managed = x1_host_quiesce = x1_power_quiesce = x1_control_retire = x1_cm_retire = admin = true;
 trylock_fail = false;
 infos = kfrees = destroyed = freeze_result = stop_result = bad_receipt = power_result = pm_holds = 0;
 control_result = domain_result = platform_result = 0; ncalls = 0;
}
static bool unlocked(void) { return !dev.lock && !host.bind_lock && !host.lifecycle_lock && !host.lock && !tb.lock; }
static bool kept(void) {
 return !host.idle_retire_attempted && !host.stopping && host.ready && !host.host_stop_attempted && !host.fully_retired;
}
static ssize_t store(const char *token) { return idle_retire_once_store(&dev, NULL, token, strlen(token)); }
'''

STORE_CASES = r'''
static void terminal(int expect, const char *stage, int steps) {
 ssize_t ret = store("retire-idle-v1\n");
 assert(ret == expect && unlocked() && ncalls == steps);
 assert(host.idle_retire_attempted && host.stopping && !host.ready && !host.fully_retired && !host.idle_retired);
 assert(!host.batch_waiting && host.batch_terminal && host.error == -ESHUTDOWN);
 assert(host.idle_retire_error == expect && !strcmp(host.idle_retire_stage, stage) && !kfrees && !destroyed);
 assert(pm_holds == 1); /* A failed power-down keeps the router's runtime vote. */
 assert(store("retire-idle-v1\n") == -EALREADY && unlocked());
}
int main(void) {
 const char *token = "retire-idle-v1\n";
 struct x1_host before;
 int refusals = 0, faults = 0;
 reset();
 assert(store(token) == (ssize_t)strlen(token) && unlocked());
 int ok[] = { FREEZE, STOP, POWER, CONTROL, DOMAIN, PLATFORM };
 assert(order(ok, 6));
 assert(host.idle_retire_attempted && host.idle_retired && host.fully_retired && host.stopping && !host.ready);
 assert(!host.batch_waiting && host.batch_terminal && host.error == -ESHUTDOWN && !strcmp(host.batch_step, "idle-power-down"));
 assert(!host.idle_retire_error && !strcmp(host.idle_retire_stage, "idle-retired"));
 assert(host.host_stop.finished && !host.host_stop.error && kfrees == 2 && destroyed == 1 && !pm_holds);
 assert(!host.qnhi.nhi.tx_rings && !host.qnhi.nhi.rx_rings);
 assert(store(token) == -EALREADY && unlocked() && ncalls == 6);

 /* Refusals before the freeze take no stage and keep the session usable. */
#define REFUSE(setup, err) do { reset(); setup; memcpy(&before, &host, sizeof(host)); \
  assert(store(token) == (err) && unlocked() && !ncalls && !memcmp(&before, &host, sizeof(host))); refusals++; } while (0)
 REFUSE(admin = false, -EPERM);
 REFUSE(x1_general = false, -EPERM);
 REFUSE(x1_managed = false, -EPERM);
 REFUSE(x1_cm_retire = false, -EPERM);
 REFUSE(trylock_fail = true, -EBUSY);
 REFUSE(host.tb = NULL, -EPERM);
 REFUSE(host.ready = false, -EPERM);
 REFUSE(host.stopping = true, -EPERM);
 REFUSE(host.suspended = true, -EPERM);
 REFUSE(host.error = -ESHUTDOWN, -EPERM);
 REFUSE(host.batch_waiting = false, -EPERM);
 REFUSE(host.batch_terminal = true, -EPERM);
 REFUSE(host.command_attempted = true, -EPERM);
 REFUSE(host.connected_command = 1, -EPERM);
 REFUSE(host.pending_command = 1, -EPERM);
 REFUSE(host.batch_switch = &host, -EPERM);
 REFUSE(host.tunnel_held = true, -EPERM);
 REFUSE(host.training.attempted = true, -EPERM);
 REFUSE(host.inventory.attempted = true, -EPERM);
 REFUSE(host.pcie.prepared = false, -EPERM);
 REFUSE(host.pcie.inventory_attempted = true, -EPERM);
 REFUSE(host.pcie.host_attempted = true, -EPERM);
 REFUSE(host.pcie.error = -EIO, -EPERM);
 reset(); assert(store("retire-idle-v2\n") == -EPERM && !ncalls && kept()); refusals++;

 /* A refused freeze keeps the session, reports why, and may be retried. */
 reset(); freeze_result = -EBUSY;
 assert(store(token) == -EBUSY && unlocked() && ncalls == 1 && kept());
 assert(host.idle_retire_error == -EBUSY && !strcmp(host.idle_retire_stage, "refused-session-kept"));
 freeze_result = 0; ncalls = 0;
 assert(store(token) == (ssize_t)strlen(token) && order(ok, 6) && host.fully_retired); refusals++;

 /* Every fault after the freeze is terminal at its own stage. */
 reset(); stop_result = -EBUSY; terminal(-EBUSY, "stop-control", 2);
 assert(host.host_stop.error == -EBUSY && !host.host_stop.finished); faults++;
 reset(); bad_receipt = 1; terminal(-EIO, "stop-control", 2); faults++;
 reset(); power_result = -EIO; terminal(-EIO, "power-stop", 3); faults++;
 reset(); control_result = -ETIMEDOUT; terminal(-ETIMEDOUT, "control-retire", 4); faults++;
 reset(); domain_result = -ETIMEDOUT; terminal(-ETIMEDOUT, "domain-retire", 5); faults++;
 reset(); platform_result = -EPERM; terminal(-EPERM, "platform-retire", 6); faults++;
 printf("PASS idle store: ordered six stages, %d refusals keep the session, %d faults terminal\n", refusals, faults);
 return 0;
}
'''

DOMAIN_PRELUDE = COMMON + r'''
#define msecs_to_jiffies(ms) (ms)
struct completion { int done; };
struct tb { int unused; };
struct tb_x1_cm_retire { int unused; };
struct tb_nhi { struct completion domain_released; };
struct x1_host {
 int lock, lifecycle_lock; struct tb *tb; bool domain_retire_attempted, domain_unpublished;
 bool domain_unregistered, domain_retired; int domain_retire_error;
 struct tb_x1_cm_retire cm_retire; struct { struct tb_nhi nhi; } qnhi;
};
enum { OWNER = 1, CM, RCU, REMOVE, WAIT };
static struct tb tb;
static struct x1_host host;
static int owner_result, cm_result, released, retarget;
static int x1_idle_cm_owner_stopped(void *c) { assert(c == &host && host.lifecycle_lock); note(OWNER); return owner_result; }
static int tb_x1_idle_retire_cm(struct tb *t, struct tb_x1_cm_retire *s, int (*check)(void *), void *c) {
 assert(t == &tb && s == &host.cm_retire && check == x1_idle_cm_owner_stopped && c == &host && !host.lock);
 note(CM); if (retarget) host.tb = NULL; return cm_result;
}
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void synchronize_rcu(void) { assert(!host.lock && !host.tb && host.domain_unpublished); note(RCU); }
static void tb_domain_remove(struct tb *t) { assert(t == &tb && !host.lock); note(REMOVE); }
static unsigned long wait_for_completion_timeout(struct completion *x, unsigned long t) {
 assert(x == &host.qnhi.nhi.domain_released && t == 5000 && host.domain_unregistered); note(WAIT); return released;
}
static void reset(void) {
 memset(&host, 0, sizeof(host)); host.tb = &tb; host.lifecycle_lock = 1;
 owner_result = cm_result = retarget = 0; released = 1; ncalls = 0;
}
'''

DOMAIN_CASES = r'''
int main(void) {
 reset();
 assert(!x1_idle_retire_domain(&host));
 int ok[] = { OWNER, CM, RCU, REMOVE, WAIT };
 assert(order(ok, 5) && host.domain_retired && !host.domain_retire_error && !host.tb && !host.lock);
 assert(x1_idle_retire_domain(&host) == -EALREADY);
 reset(); owner_result = -EPERM;
 assert(x1_idle_retire_domain(&host) == -EPERM && !host.domain_retire_attempted && host.tb == &tb);
 reset(); cm_result = -EBUSY;
 assert(x1_idle_retire_domain(&host) == -EBUSY && host.domain_retire_error == -EBUSY && host.tb == &tb && ncalls == 2);
 reset(); retarget = 1;
 assert(x1_idle_retire_domain(&host) == -ESTALE && !host.domain_unpublished && ncalls == 2 && !host.lock);
 reset(); released = 0;
 assert(x1_idle_retire_domain(&host) == -ETIMEDOUT && host.domain_unregistered && !host.domain_retired);
 assert(host.domain_retire_error == -ETIMEDOUT);
 printf("PASS idle domain: unpublish, RCU, remove, bounded release wait, no hold reference\n");
 return 0;
}
'''

SUSPEND_PRELUDE = COMMON + r'''
#define __free(x)
struct device { int unused; };
struct x1_host { int lifecycle_lock; bool startup_attempted, fully_retired, suspended; };
static bool x1_general;
static struct device dev;
static struct x1_host host;
static struct x1_host *x1_session_get(struct device *d) { assert(d == &dev); return &host; }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static int x1_suspend(struct device *dev);
static int run(bool general, bool started, bool retired) {
 x1_general = general; host = (struct x1_host){ 0, started, retired, false };
 int ret = x1_suspend(&dev);
 assert(!host.lifecycle_lock && host.suspended == !ret);
 return ret;
}
'''

SUSPEND_CASES = r'''
int main(void) {
 assert(!run(true, false, false) && !run(false, false, false));
 assert(run(true, true, false) == -EBUSY && run(false, true, false) == -EBUSY);
 assert(!run(true, true, true));
 assert(run(false, true, true) == -EBUSY);
 printf("PASS suspend gate: fresh sessions sleep, active refuse, only general fully retired admitted\n");
 return 0;
}
'''

PCIE_PRELUDE = COMMON + r'''
struct device { void *driver; int refs; };
struct platform_device { struct device dev; };
struct platform_driver { struct { int unused; } driver; };
struct clk { int on; };
struct icc_path { int bw; };
struct reset_control { int asserted, puts; };
struct x1_pcie_state {
 bool general, prepared, inventory_attempted, host_attempted, bridge_allocated, nvme_bind_attempted;
 bool platform_retire_attempted, platform_reset, platform_icc_released, platform_runtime_released, platform_retired;
 int error, platform_retire_error; unsigned int platform_clocks_released; const char *host_step;
};
struct x1_pcie_context {
 struct device *owner; struct x1_pcie_state *state; struct platform_device *pdev;
 void *bridge, *receiver, *window, *endpoint, *root_port; unsigned long long nvme_session;
 bool receiver_retained, config_open, scan_writes, bind_writes, batch_active, driver_registered;
 bool runtime_enabled, runtime_held, reset_early; unsigned int clocks_enabled;
 struct clk *clocks[3]; struct reset_control *reset; struct icc_path *cfg, *mem;
};
#define READ_ONCE(x) (x)
enum { CHECK = 1, ASSERT, ICC, CLK, ALLOW, PUT_SYNC, BARRIER, DISABLE, UNPUBLISH, UNREGISTER, RESET_PUT, PUT_DEV, FREE };
static struct x1_pcie_driver_type { struct { int unused; } driver; } x1_pcie_driver;
static int x1_pcie_lock, x1_config_lock, check_result, assert_result, icc_result, put_result;
static bool x1_pcie_reset_held;
static struct x1_pcie_context *x1_pcie, ctx_storage;
static struct x1_pcie_state state;
static struct device owner;
static struct platform_device pdev;
static struct clk clks[3];
static struct icc_path cfg, mem;
static struct reset_control rst;
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
#define raw_spin_lock_irqsave(l, f) do { (f) = 0; assert(!*(l)); *(l) = 1; } while (0)
#define raw_spin_unlock_irqrestore(l, f) do { (void)(f); assert(*(l)); *(l) = 0; } while (0)
static int check_stopped(void *c) { assert(c == &state && x1_pcie_lock); note(CHECK); return check_result; }
static int reset_control_assert(struct reset_control *r) { assert(r == &rst); note(ASSERT); if (!assert_result) r->asserted = 1; return assert_result; }
static int icc_set_bw(struct icc_path *p, int a, int b) { assert((p == &cfg || p == &mem) && !a && !b); note(ICC); p->bw = 0; return icc_result; }
static void clk_disable_unprepare(struct clk *c) { assert(c->on); c->on = 0; note(CLK); }
static void pm_runtime_allow(struct device *d) { assert(d == &pdev.dev); note(ALLOW); }
static int pm_runtime_put_sync(struct device *d) { assert(d == &pdev.dev); note(PUT_SYNC); return put_result; }
static void pm_runtime_barrier(struct device *d) { assert(d == &pdev.dev); note(BARRIER); }
static void pm_runtime_disable(struct device *d) { assert(d == &pdev.dev); note(DISABLE); }
static void platform_driver_unregister(void *d) { assert(d == &x1_pcie_driver && !x1_pcie); note(UNREGISTER); }
static void reset_control_put(struct reset_control *r) { assert(r == &rst && r->asserted); r->puts++; note(RESET_PUT); }
static void put_device(struct device *d) { assert(d->refs > 0); d->refs--; note(PUT_DEV); }
static void kfree(void *p) { assert(p == &ctx_storage); note(FREE); }
static void reset(bool early) {
 memset(&state, 0, sizeof(state)); memset(&ctx_storage, 0, sizeof(ctx_storage));
 state.general = state.prepared = true;
 for (int i = 0; i < 3; i++) { clks[i].on = 1; ctx_storage.clocks[i] = &clks[i]; }
 pdev.dev.driver = &x1_pcie_driver.driver; pdev.dev.refs = 1; owner.refs = 1; rst = (struct reset_control){ 0, 0 };
 ctx_storage.owner = &owner; ctx_storage.state = &state; ctx_storage.pdev = &pdev;
 ctx_storage.driver_registered = ctx_storage.runtime_enabled = ctx_storage.runtime_held = true;
 ctx_storage.clocks_enabled = 3; ctx_storage.reset = &rst; ctx_storage.cfg = &cfg; ctx_storage.mem = &mem;
 ctx_storage.reset_early = early; x1_pcie = &ctx_storage; x1_pcie_reset_held = false;
 check_result = assert_result = icc_result = put_result = 0; ncalls = 0;
}
static int retire(void) { return qcom_usb4_x1_idle_retire_platform(&owner, &state, check_stopped, &state); }
'''

PCIE_CASES = r'''
int main(void) {
 int refusals = 0, faults = 0;
 reset(true);
 assert(!retire() && !x1_pcie_lock && !x1_config_lock && !x1_pcie);
 int ok[] = { CHECK, ASSERT, ICC, ICC, CLK, CLK, CLK, ALLOW, PUT_SYNC, BARRIER, DISABLE, UNREGISTER, RESET_PUT, PUT_DEV, PUT_DEV, FREE };
 assert(order(ok, 16));
 assert(state.platform_retired && state.platform_reset && state.platform_icc_released && state.platform_runtime_released);
 assert(state.platform_clocks_released == 3 && x1_pcie_reset_held && rst.asserted && rst.puts == 1);
 assert(!clks[0].on && !clks[1].on && !clks[2].on && !pdev.dev.refs && !owner.refs);
 assert(!strcmp(state.host_step, "idle-platform-provider-context-retired"));
 reset(false);
 assert(!retire() && ncalls == 15 && !rst.puts);

#define REFUSE(setup) do { reset(true); setup; struct x1_pcie_context *was = x1_pcie; \
  assert(retire() == -EPERM && !ncalls && x1_pcie == was && !state.platform_retire_attempted); refusals++; } while (0)
 REFUSE(state.general = false);
 REFUSE(state.prepared = false);
 REFUSE(state.error = -EIO);
 REFUSE(state.inventory_attempted = true);
 REFUSE(state.host_attempted = true);
 REFUSE(state.bridge_allocated = true);
 REFUSE(state.nvme_bind_attempted = true);
 REFUSE(ctx_storage.bridge = &state);
 REFUSE(ctx_storage.receiver = &state);
 REFUSE(ctx_storage.receiver_retained = true);
 REFUSE(ctx_storage.config_open = true);
 REFUSE(ctx_storage.batch_active = true);
 REFUSE(ctx_storage.window = &state);
 REFUSE(ctx_storage.endpoint = &state);
 REFUSE(ctx_storage.root_port = &state);
 REFUSE(ctx_storage.nvme_session = 1);
 REFUSE(ctx_storage.driver_registered = false);
 REFUSE(ctx_storage.runtime_held = false);
 REFUSE(ctx_storage.clocks_enabled = 2);
 REFUSE(pdev.dev.driver = NULL);
 REFUSE(x1_pcie = NULL);
 REFUSE(ctx_storage.owner = &pdev.dev);
 reset(true); check_result = -EBUSY;
 assert(retire() == -EBUSY && ncalls == 1 && !state.platform_retire_attempted && x1_pcie); refusals++;
 reset(true); state.platform_retire_attempted = true;
 assert(retire() == -EALREADY && !ncalls); refusals++;

 reset(true); assert_result = -EIO;
 assert(retire() == -EIO && state.platform_retire_error == -EIO && !state.platform_reset && x1_pcie && ncalls == 2); faults++;
 reset(true); icc_result = -EIO;
 assert(retire() == -EIO && state.platform_reset && !state.platform_icc_released && clks[0].on && x1_pcie); faults++;
 reset(true); put_result = -EIO;
 assert(retire() == -EIO && !ctx_storage.runtime_held && ctx_storage.runtime_enabled && x1_pcie);
 assert(!state.platform_runtime_released && ncalls == 9); faults++;
 printf("PASS idle platform: provider release in retire order, pdev lease only, %d refusals, %d faults\n", refusals, faults);
 return 0;
}
'''


BASE = 'b273a0e'  # omarchy branch before the idle power-down


def base(path):
    return (HERE.parent / 'base' / path.relative_to(SRC)).read_text()


class IdleRetire(unittest.TestCase):
    def test_tb_freeze_and_root_removal(self):
        text = TB.read_text()
        code = TB_PRELUDE + '\n'.join(
            c_function(text, name) for name in (
                'tb_x1_root_empty', 'tb_x1_idle_freeze', 'tb_x1_idle_cm_check',
                'tb_x1_idle_retire_cm')) + TB_CASES
        print(build_and_run(code, 'x1-idle-tb-'), end='')

    def test_freeze_wait(self):
        code = FREEZE_PRELUDE + c_function(HOST.read_text(), 'x1_idle_freeze_cm') + FREEZE_CASES
        print(build_and_run(code, 'x1-idle-freeze-'), end='')

    def test_store_order_and_stages(self):
        code = STORE_PRELUDE.replace('static ssize_t store', 'static ssize_t idle_retire_once_store('
                                     'struct device *, struct device_attribute *, const char *, size_t);\n'
                                     'static ssize_t store')
        code += c_function(HOST.read_text(), 'idle_retire_once_store') + STORE_CASES
        print(build_and_run(code, 'x1-idle-store-'), end='')

    def test_domain_release(self):
        body = c_function(HOST.read_text(), 'x1_idle_retire_domain')
        self.assertNotIn('tb_domain_put', body)
        print(build_and_run(DOMAIN_PRELUDE + body + DOMAIN_CASES, 'x1-idle-domain-'), end='')

    def test_suspend_gate(self):
        code = SUSPEND_PRELUDE + c_function(HOST.read_text(), 'x1_suspend') + SUSPEND_CASES
        print(build_and_run(code, 'x1-idle-suspend-'), end='')

    def test_platform_release(self):
        body = c_function(PCIE.read_text(), 'qcom_usb4_x1_idle_retire_platform')
        self.assertNotIn('module_put', body)
        self.assertNotIn('pm_runtime_allow(owner)', body)
        code = PCIE_PRELUDE.replace('static int retire(void)', 'int qcom_usb4_x1_idle_retire_platform('
                                    'struct device *, struct x1_pcie_state *, int (*)(void *), void *);\n'
                                    'static int retire(void)')
        print(build_and_run(code + body + PCIE_CASES, 'x1-idle-platform-'), end='')

    def test_renewal_skips_detach_only_for_idle(self):
        body = c_function(HOST.read_text(), 'renew_session_store')
        self.assertIn('(old->idle_retired ? x1_idle_platform_owner_stopped(old) :\n'
                      '\t\t\t\t x1_platform_owner_stopped(old))', body)
        self.assertIn('if (port->orientation != TYPEC_ORIENTATION_NONE ||\n'
                      '\t    (!old->idle_retired && port->disconnect_sequence <= old->initial_event_sequence))',
                      body)

    def test_reused_harness_functions_unchanged(self):
        for path, names in ((TB, ('tb_x1_retire_cm', 'tb_x1_cm_retire_check')),
                            (HOST, ('x1_power_stop_run', 'x1_retire_domain', 'x1_retire_session',
                                    'host_quiesce_once_store', 'power_quiesce_once_store')),
                            (PCIE, ('qcom_usb4_x1_retire_platform',)),
                            (SRC / 'drivers/thunderbolt/ctl.c', ('tb_ctl_stop_x1_retained',
                                                                 'tb_ctl_retire_x1_stopped'))):
            old, new = base(path), path.read_text()
            for name in names:
                self.assertEqual(c_function(old, name), c_function(new, name), name)


if __name__ == '__main__':
    unittest.main()
