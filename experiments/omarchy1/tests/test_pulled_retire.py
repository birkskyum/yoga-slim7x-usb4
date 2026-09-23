#!/usr/bin/env python3
"""Run the actual omarchy-branch pulled-drive functions with mocks; no hardware.

Covers the Type-C detach mark and the config callbacks that then answer
absent without touching the tunnel (pcie.c), removal of the pulled endpoint,
the software-only tunnel stop (tb.c), the shared tunnel receipt predicate and
the store's order, locks and failure stages (host.c).
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_general_pci import build_and_run  # noqa: E402
from test_idle_retire import COMMON, c_function, TB, HOST, PCIE  # noqa: E402

CONFIG_PRELUDE = COMMON + r'''
#define __iomem
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define PCIBIOS_SUCCESSFUL 0x00
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
#define PCIBIOS_SET_FAILED 0x88
#define xchg(p, v) ({ __typeof__(*(p)) _old = *(p); *(p) = (v); _old; })
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
enum { pci_channel_io_normal = 1, pci_channel_io_frozen, pci_channel_io_perm_failure };
struct device { void *driver; bool registered; struct { void *data; } msi; };
struct pci_bus { int number; };
struct pci_dev { struct device dev; int error_state; void *drvdata; };
struct pci_host_bridge { int unused; };
struct x1_pcie_state {
 bool general, pulled, pci_retire_attempted, nvme_requested, host_prepared, host_published;
 bool nvme_bound, nvme_bind_attempted, config_fenced, endpoint_remove_attempted, endpoint_removed;
 bool cfg_error_write;
 int error, host_error, cfg_error, endpoint_remove_error;
 u32 cfg_error_offset, cfg_error_value; u8 cfg_error_bus, cfg_error_size;
 const char *host_step; struct { bool ready; } receiver;
};
struct x1_pcie_context {
 struct device *owner; struct x1_pcie_state *state; struct pci_dev *endpoint, *root_port;
 struct pci_host_bridge *bridge; bool pulled, batch_active, receiver_retained, bind_writes;
};
static int x1_config_lock, x1_pcie_lock, rescan_lock;
static struct x1_pcie_context ctx_storage, *x1_pcie;
static struct x1_pcie_state state;
static struct device owner;
static struct pci_dev ep, rp;
static struct pci_bus bus = { 1 };
static struct pci_host_bridge bridge;
static char window[4096];
static int base_present, host_local_result, host_local_calls, mmio_reads, mmio_writes, removals;
#define raw_spin_lock_irqsave(l, f) do { (f) = 0; assert(!*(l)); *(l) = 1; } while (0)
#define raw_spin_unlock_irqrestore(l, f) do { (void)(f); assert(*(l)); *(l) = 0; } while (0)
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void pci_lock_rescan_remove(void) { mutex_lock(&rescan_lock); }
static void pci_unlock_rescan_remove(void) { mutex_unlock(&rescan_lock); }
static bool pci_dev_is_disconnected(const struct pci_dev *d) { return d->error_state == pci_channel_io_perm_failure; }
static void *pci_get_drvdata(struct pci_dev *d) { return d->drvdata; }
static bool device_is_registered(struct device *d) { return d->registered; }
static void __iomem *x1_pcie_config_base(struct pci_bus *b, unsigned int devfn) {
 assert(x1_config_lock && b == &bus && !devfn); return base_present ? window : NULL;
}
static int x1_pcie_host_local(struct x1_pcie_context *c) { assert(c == x1_pcie && x1_config_lock); host_local_calls++; return host_local_result; }
static u32 x1_pcie_config_read_width(void __iomem *addr, int size) { (void)addr; (void)size; mmio_reads++; return 0x1234; }
static int x1_pcie_config_write_allowed(struct x1_pcie_context *c, void __iomem *b, int w, int s, u32 v) {
 (void)c; (void)b; (void)w; (void)s; (void)v; return 0;
}
static void writeb(u32 v, void *a) { (void)v; (void)a; mmio_writes++; }
static void writew(u32 v, void *a) { (void)v; (void)a; mmio_writes++; }
static void writel(u32 v, void *a) { (void)v; (void)a; mmio_writes++; }
static void pci_stop_and_remove_bus_device(struct pci_dev *d) {
 assert(d == &ep && rescan_lock && x1_pcie_lock && pci_dev_is_disconnected(d));
 d->dev.driver = NULL; d->drvdata = NULL; d->dev.msi.data = NULL; d->dev.registered = false; removals++;
}
static void reset(void) {
 memset(&ctx_storage, 0, sizeof(ctx_storage)); memset(&state, 0, sizeof(state));
 memset(&ep, 0, sizeof(ep)); memset(&rp, 0, sizeof(rp));
 ep.error_state = pci_channel_io_normal; ep.dev.driver = &ep; ep.drvdata = &ep; ep.dev.registered = true;
 state.general = state.nvme_requested = state.host_prepared = state.host_published = true;
 state.nvme_bound = state.nvme_bind_attempted = state.receiver.ready = true;
 ctx_storage = (struct x1_pcie_context){ &owner, &state, &ep, &rp, &bridge };
 ctx_storage.receiver_retained = ctx_storage.bind_writes = true;
 x1_pcie = &ctx_storage; base_present = 1; host_local_result = 0;
 host_local_calls = mmio_reads = mmio_writes = removals = 0;
}
int qcom_usb4_x1_pulled_remove(struct device *owner, struct x1_pcie_state *state);
void qcom_usb4_x1_mark_pulled(struct device *owner, struct x1_pcie_state *state);
'''

CONFIG_CASES = r'''
int main(void) {
 u32 v;
 /* Before the pull every access still goes through the live checks. */
 reset();
 assert(x1_pcie_config_read(&bus, 0, 0, 4, &v) == PCIBIOS_SUCCESSFUL && v == 0x1234 && host_local_calls == 1);
 assert(x1_pcie_config_write(&bus, 0, 4, 2, 6) == PCIBIOS_SUCCESSFUL && mmio_writes == 1);
 /* The detach mark: disconnected for the PCI core and NVMe, absent for config. */
 qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(ctx_storage.pulled && state.pulled && pci_dev_is_disconnected(&ep) && !x1_config_lock);
 host_local_calls = mmio_reads = mmio_writes = 0;
 assert(x1_pcie_config_read(&bus, 0, 0, 4, &v) == PCIBIOS_DEVICE_NOT_FOUND && v == ~0U);
 assert(x1_pcie_config_write(&bus, 0, 4, 2, 6) == PCIBIOS_DEVICE_NOT_FOUND);
 assert(!host_local_calls && !mmio_reads && !mmio_writes && !state.cfg_error && !x1_config_lock);
 assert(x1_pcie_enable_device(&bridge, &ep) == -EPERM && !host_local_calls);
 qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(ctx_storage.pulled && pci_dev_is_disconnected(&ep));
 /* No published endpoint, the wrong owner, the harness flavor or a retired PCI: no-op. */
 reset(); ctx_storage.endpoint = NULL; qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(!ctx_storage.pulled && !state.pulled);
 reset(); qcom_usb4_x1_mark_pulled(NULL, &state); assert(!ctx_storage.pulled);
 reset(); state.general = false; qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(!ctx_storage.pulled && !pci_dev_is_disconnected(&ep));
 reset(); state.pci_retire_attempted = true; qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(!ctx_storage.pulled);
 reset(); x1_pcie = NULL; qcom_usb4_x1_mark_pulled(&owner, &state); assert(!state.pulled);

 /* Removal of the pulled endpoint: no config proof, receipts, one attempt. */
 int refusals = 0;
#define REFUSE(setup) do { reset(); qcom_usb4_x1_mark_pulled(&owner, &state); setup; \
  assert(qcom_usb4_x1_pulled_remove(&owner, &state) == -EPERM && !removals && !state.endpoint_remove_attempted); refusals++; } while (0)
 reset();
 assert(qcom_usb4_x1_pulled_remove(&owner, &state) == -EPERM && !removals); refusals++; /* Never marked. */
 REFUSE(ep.error_state = pci_channel_io_normal);
 REFUSE(state.general = false);
 REFUSE(state.cfg_error = -ENODEV);
 REFUSE(state.error = -EIO);
 REFUSE(state.config_fenced = true);
 REFUSE(state.nvme_bound = false);
 REFUSE(ctx_storage.batch_active = true);
 REFUSE(ctx_storage.owner = NULL);
 reset(); qcom_usb4_x1_mark_pulled(&owner, &state);
 assert(!qcom_usb4_x1_pulled_remove(&owner, &state) && removals == 1 && !mmio_reads && !host_local_calls);
 assert(state.endpoint_removed && !state.endpoint_remove_error && !strcmp(state.host_step, "pulled-endpoint-removed"));
 assert(qcom_usb4_x1_pulled_remove(&owner, &state) == -EALREADY && removals == 1 && !x1_pcie_lock && !rescan_lock);
 printf("PASS pulled mark: config answers absent without MMIO, no error latched; removal %d refusals\n", refusals);
 return 0;
}
'''

TB_PULLED_PRELUDE = COMMON + r'''
typedef unsigned long long u64;
enum tb_tunnel_state { TB_TUNNEL_INACTIVE, TB_TUNNEL_ACTIVATING, TB_TUNNEL_ACTIVE };
struct tb_port { int unused; };
struct tb_switch { u64 uid; int authorized; };
struct tb_path { bool activated; };
struct tb_tunnel { enum tb_tunnel_state state; struct tb_port *src_port, *dst_port; int npaths; struct tb_path *paths[2]; bool pci; };
struct tb { struct tb_switch *root_switch; };
struct tb_cm {
 bool retained, x1_stop_attempted, x1_stop_complete, x1_hold_attempted, x1_hold_ready;
 u64 x1_pcie_uid; struct tb_tunnel *x1_held_tunnel, *x1_pcie_tunnel;
 struct tb_switch *x1_held_switch, *x1_pcie_switch, *x1_held_root;
 struct tb_path *x1_held_paths[2]; struct tb_port *x1_held_up, *x1_held_down;
};
struct tb_x1_tunnel_stop { bool attempted, finished; int error; const char *stage; unsigned int adapters, hops;
 int (*check_live)(void *); void *context; };
static struct tb tb;
static struct tb_cm cm;
#define tb_priv(t) (&cm)
static struct tb_switch root, sw;
static struct tb_port up, down, other;
static struct tb_path p0, p1;
static struct tb_tunnel tunnel;
static int owner_result, topology_ok, adapters_result, topology_calls;
static int tb_x1_pcie_owner(struct tb *t) { assert(t == &tb); return owner_result; }
static bool tb_x1_retained_mode(struct tb *t) { assert(t == &tb); return cm.retained; }
static bool tb_x1_pcie_topology(struct tb *t, struct tb_switch *s, bool authorized) {
 assert(t == &tb && s == &sw && authorized); topology_calls++; return topology_ok;
}
static int tb_x1_pcie_adapters(struct tb *t, struct tb_switch *s, struct tb_port **u, struct tb_port **d) {
 assert(t == &tb && s == &sw); *u = &up; *d = &down; return adapters_result;
}
static bool tb_tunnel_is_pci(struct tb_tunnel *t) { return t->pci; }
static void reset(void) {
 memset(&cm, 0, sizeof(cm));
 root = (struct tb_switch){ 0, 1 }; sw = (struct tb_switch){ 0x1234, 1 };
 p0.activated = p1.activated = true;
 tunnel = (struct tb_tunnel){ TB_TUNNEL_ACTIVE, &down, &up, 2, { &p0, &p1 }, true };
 tb.root_switch = &root;
 cm.retained = cm.x1_hold_attempted = cm.x1_hold_ready = true; cm.x1_pcie_uid = 0x1234;
 cm.x1_held_tunnel = cm.x1_pcie_tunnel = &tunnel; cm.x1_held_switch = cm.x1_pcie_switch = &sw;
 cm.x1_held_root = &root; cm.x1_held_paths[0] = &p0; cm.x1_held_paths[1] = &p1;
 cm.x1_held_up = &up; cm.x1_held_down = &down;
 owner_result = adapters_result = topology_calls = 0; topology_ok = 1;
}
int tb_x1_stop_pcie_pulled(struct tb *tb, u64 expected_uid, struct tb_x1_tunnel_stop *receipt);
'''

TB_PULLED_CASES = r'''
static bool untouched(void) {
 return tunnel.state == TB_TUNNEL_ACTIVE && p0.activated && p1.activated && sw.authorized && !cm.x1_stop_complete;
}
int main(void) {
 struct tb_x1_tunnel_stop r;
 reset(); memset(&r, 0, sizeof(r));
 assert(!tb_x1_stop_pcie_pulled(&tb, 0x1234, &r));
 assert(tunnel.state == TB_TUNNEL_INACTIVE && !p0.activated && !p1.activated && !sw.authorized);
 assert(cm.x1_stop_attempted && cm.x1_stop_complete && r.attempted && r.finished && !r.error);
 assert(!r.adapters && !r.hops && !strcmp(r.stage, "pulled-inactive-host-reset-pending") && topology_calls == 1);
 memset(&r, 0, sizeof(r));
 assert(tb_x1_stop_pcie_pulled(&tb, 0x1234, &r) == -EALREADY && !r.attempted);
 /* Refusals before consuming the stop. */
 reset(); memset(&r, 0, sizeof(r)); owner_result = -EPERM;
 assert(tb_x1_stop_pcie_pulled(&tb, 0x1234, &r) == -EPERM && !cm.x1_stop_attempted && untouched());
 reset(); r = (struct tb_x1_tunnel_stop){ .attempted = true };
 assert(tb_x1_stop_pcie_pulled(&tb, 0x1234, &r) == -EINVAL && !cm.x1_stop_attempted);
 assert(tb_x1_stop_pcie_pulled(&tb, 0, &r) == -EINVAL);
 /* Identity faults consume the one stop and leave the tunnel untouched. */
 int faults = 0;
#define FAULT(setup, err) do { reset(); memset(&r, 0, sizeof(r)); setup; \
  struct tb_tunnel t0 = tunnel; struct tb_path a0 = p0, a1 = p1; struct tb_switch s0 = sw; \
  assert(tb_x1_stop_pcie_pulled(&tb, 0x1234, &r) == (err) && r.error == (err) && r.attempted && !r.finished); \
  assert(cm.x1_stop_attempted && !cm.x1_stop_complete && !memcmp(&t0, &tunnel, sizeof(t0))); \
  assert(a0.activated == p0.activated && a1.activated == p1.activated && s0.authorized == sw.authorized); \
  faults++; } while (0)
 FAULT(cm.retained = false, -ENODEV);
 FAULT(cm.x1_hold_ready = false, -ENODEV);
 FAULT(cm.x1_pcie_uid = 0x9999, -ENODEV);
 FAULT(sw.uid = 0x9999, -ENODEV);
 FAULT(tb.root_switch = &sw, -ENODEV);
 FAULT(cm.x1_pcie_tunnel = NULL, -ENODEV);
 FAULT(topology_ok = 0, -ENODEV);
 FAULT(adapters_result = -ENODEV, -ENODEV);
 FAULT(cm.x1_held_up = &other, -EIO);
 FAULT(tunnel.state = TB_TUNNEL_INACTIVE, -EIO);
 FAULT(p1.activated = false, -EIO);
 FAULT(tunnel.paths[0] = &p1, -EIO);
 FAULT(tunnel.src_port = &up, -EIO);
 FAULT(tunnel.pci = false, -EIO);
 printf("PASS pulled CM stop: software inactive like a retained stop, %d identity faults terminal\n", faults);
 return 0;
}
'''

PREDICATE_PRELUDE = COMMON + r'''
struct tb_x1_tunnel_stop { bool finished; int error; unsigned int adapters, hops; };
struct x1_host {
 bool tunnel_held, tunnel_stop_complete, pulled; int tunnel_stop_error;
 struct tb_x1_tunnel_stop tunnel_stop; struct { bool pulled; } pcie;
};
static bool x1_general;
static struct x1_host h;
static void reset(bool pulled) {
 memset(&h, 0, sizeof(h));
 h.tunnel_held = h.tunnel_stop_complete = h.tunnel_stop.finished = true;
 if (pulled) { h.pulled = h.pcie.pulled = true; } else { h.tunnel_stop.adapters = 2; h.tunnel_stop.hops = 4; }
 x1_general = true;
}
'''

PREDICATE_CASES = r'''
int main(void) {
 reset(false); assert(x1_tunnel_stopped(&h));
 x1_general = false; assert(x1_tunnel_stopped(&h)); /* Harness flavor unchanged. */
 reset(false); h.tunnel_stop.hops = 2; assert(!x1_tunnel_stopped(&h));
 reset(false); h.tunnel_stop.adapters = 0; h.tunnel_stop.hops = 0; assert(!x1_tunnel_stopped(&h));
 reset(false); h.tunnel_stop_error = -EIO; assert(!x1_tunnel_stopped(&h));
 reset(true); assert(x1_tunnel_stopped(&h));
 reset(true); x1_general = false; assert(!x1_tunnel_stopped(&h));
 reset(true); h.pcie.pulled = false; assert(!x1_tunnel_stopped(&h));
 reset(true); h.tunnel_stop.adapters = 2; h.tunnel_stop.hops = 4; assert(!x1_tunnel_stopped(&h));
 reset(true); h.tunnel_stop.finished = false; assert(!x1_tunnel_stopped(&h));
 reset(true); h.tunnel_held = false; assert(!x1_tunnel_stopped(&h));
 printf("PASS tunnel receipt: retained 2/4 unchanged, pulled 0/0 only with the pulled mark\n");
 return 0;
}
'''

SWITCH_PRELUDE = COMMON + r'''
enum typec_orientation { TYPEC_ORIENTATION_NONE, TYPEC_ORIENTATION_NORMAL, TYPEC_ORIENTATION_REVERSE };
#define __free(x)
struct device { int unused; };
struct typec_switch_dev { int unused; };
struct x1_pcie_state { int unused; };
struct x1_port { int unused; };
struct x1_host { struct device *dev; int lock; bool stopping, command_attempted; int error;
 enum typec_orientation orientation; unsigned int pending_command; struct x1_pcie_state pcie; };
enum { INVALIDATE = 1, MARK, BATCH };
static bool x1_general;
static struct device dev;
static struct typec_switch_dev swdev;
static struct x1_port port;
static struct x1_host host;
static int batch_result;
static void *typec_switch_get_drvdata(struct typec_switch_dev *s) { assert(s == &swdev); return &port; }
static struct x1_host *x1_orientation_session(struct x1_port *p, enum typec_orientation o) { (void)o; assert(p == &port); return &host; }
static void x1_invalidate_retained(struct x1_host *h) { assert(h == &host); note(INVALIDATE); }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void qcom_usb4_x1_mark_pulled(struct device *o, struct x1_pcie_state *s) {
 assert(o == &dev && s == &host.pcie && host.lock); note(MARK);
}
static int x1_batch_orientation(struct x1_host *h, enum typec_orientation o) { (void)o; assert(h == &host && h->lock); note(BATCH); return batch_result; }
static void reset(bool general, bool stopping) {
 host = (struct x1_host){ .dev = &dev, .stopping = stopping }; x1_general = general; batch_result = -ESHUTDOWN; ncalls = 0;
}
'''

SWITCH_CASES = r'''
int main(void) {
 int mark_first[] = { INVALIDATE, INVALIDATE, MARK, BATCH };
 reset(true, false);
 assert(x1_switch_set(&swdev, TYPEC_ORIENTATION_NONE) == -ESHUTDOWN && order(mark_first, 4) && !host.lock);
 int plain[] = { INVALIDATE, INVALIDATE, BATCH };
 reset(true, false);
 x1_switch_set(&swdev, TYPEC_ORIENTATION_NORMAL); assert(order(plain, 3));
 reset(false, false);
 x1_switch_set(&swdev, TYPEC_ORIENTATION_NONE); assert(order(plain, 3));
 int stopped[] = { INVALIDATE, INVALIDATE };
 reset(true, true);
 assert(!x1_switch_set(&swdev, TYPEC_ORIENTATION_NONE) && order(stopped, 2));
 printf("PASS detach: the pulled mark precedes batch handling, general flavor only, not once stopping\n");
 return 0;
}
'''

STORE_PRELUDE = COMMON + r'''
#define __free(x)
#define CAP_SYS_ADMIN 21
#define dev_info(d, ...) (infos++)
#define rcu_access_pointer(p) (p)
#define device_lock_assert(d) assert((d)->lock)
struct device { int lock; };
struct device_attribute { int unused; };
struct tb_ctl { int unused; };
struct tb { int lock; struct tb_ctl *ctl; };
struct qcom_usb4_nhi { int unused; };
struct qcom_usb4_nhi_stop { bool attempted, ctl_closed, irq_disabled, callbacks_drained, finished;
 unsigned int rings_disabled, rx_options, tx_options; int error; const char *stage; };
struct qcom_usb4_nhi_retire { int unused; };
struct tb_x1_tunnel_stop { int unused; };
struct x1_pcie_state { bool nvme_bound, endpoint_removed, pulled; };
struct x1_power_stop { bool attempted; };
struct x1_host {
 struct device *dev; int lock, bind_lock, lifecycle_lock; struct tb *tb, *retained_tb;
 bool tunnel_held, cold_owned, ready, stopping, suspended, batch_attempted, batch_terminal, batch_complete;
 bool batch_waiting, command_attempted, tunnel_stop_attempted, tunnel_stop_complete, host_stop_attempted;
 bool pulled, pulled_retire_attempted, pulled_retired;
 int error, batch_error, tunnel_stop_error, pulled_retire_error; unsigned int connected_command;
 unsigned long long batch_uid; const char *pulled_retire_stage;
 struct x1_pcie_state pcie; struct x1_power_stop power_stop; struct qcom_usb4_nhi_stop host_stop;
 struct qcom_usb4_nhi qnhi; struct qcom_usb4_nhi_retire control_retire; struct tb_x1_tunnel_stop tunnel_stop;
};
enum { MARK = 1, REMOVE, FENCE, CM, STOP, POWER, NAMESPACES, PCI, CONTROL, DOMAIN, SESSION };
static bool x1_general, x1_managed, x1_tunnel_quiesce, x1_host_quiesce, x1_power_quiesce;
static bool x1_namespace_retire, x1_pci_retire, x1_control_retire, x1_cm_retire, admin, trylock_fail, pulled_port;
static int infos, fail_at, bad_receipt;
static struct device dev;
static struct tb_ctl ctl;
static struct tb tb;
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
static bool outer(void) { return dev.lock && host.bind_lock && host.lifecycle_lock; }
static int step(int code) { note(code); return fail_at == code ? -EIO : 0; }
static bool x1_port_pulled(struct x1_host *h) { assert(h == &host && h->lock); return pulled_port; }
static int x1_namespace_owner_stopped(void *c) { (void)c; return 0; }
static int x1_control_owner_stopped(void *c) { (void)c; return 0; }
static void qcom_usb4_x1_mark_pulled(struct device *o, struct x1_pcie_state *s) {
 assert(o == &dev && s == &host.pcie && outer() && host.lock && host.pulled_retire_attempted && host.pulled);
 note(MARK); s->pulled = true;
}
static int qcom_usb4_x1_pulled_remove(struct device *o, struct x1_pcie_state *s) {
 assert(o == &dev && s == &host.pcie && s->pulled && outer() && !host.lock && !tb.lock);
 int ret = step(REMOVE); if (!ret) s->endpoint_removed = true; return ret;
}
static int x1_pulled_tunnel_action(void *c) {
 assert(c == &host && outer() && host.lock && tb.lock && host.tunnel_stop_attempted); return step(CM);
}
static int qcom_usb4_x1_fence_and_quiesce(struct device *o, struct x1_pcie_state *s, int (*action)(void *), void *c) {
 assert(o == &dev && s == &host.pcie && s->endpoint_removed && action == x1_pulled_tunnel_action && c == &host);
 int ret = step(FENCE); return ret ?: action(c);
}
static int tb_ctl_stop_x1_retained(struct tb_ctl *c, struct qcom_usb4_nhi *q, struct qcom_usb4_nhi_stop *s) {
 assert(c == &ctl && q == &host.qnhi && s == &host.host_stop && host.lock && tb.lock);
 assert(host.tunnel_stop_complete && !host.tunnel_stop_error && host.host_stop_attempted && host.ready);
 int ret = step(STOP); if (ret) return ret;
 s->attempted = s->ctl_closed = s->irq_disabled = s->callbacks_drained = true;
 s->rings_disabled = bad_receipt ? 1 : 2; return 0;
}
static int x1_power_stop_run(struct x1_host *h) {
 assert(h == &host && h->lock && tb.lock && !h->ready && h->host_stop.finished);
 assert(!strcmp(h->host_stop.stage, "control-stopped-pulled")); return step(POWER);
}
static int qcom_usb4_x1_retire_namespaces(struct device *o, struct x1_pcie_state *s, int (*check)(void *), void *c) {
 assert(o == &dev && s == &host.pcie && check == x1_namespace_owner_stopped && c == &host && outer() && !host.lock && !tb.lock);
 return step(NAMESPACES);
}
static int qcom_usb4_x1_retire_pci(struct device *o, struct x1_pcie_state *s, int (*check)(void *), void *c) {
 assert(o == &dev && s == &host.pcie && check == x1_namespace_owner_stopped && c == &host && outer() && !host.lock);
 return step(PCI);
}
static int tb_ctl_retire_x1_stopped(struct tb_ctl *c, struct qcom_usb4_nhi *q, const struct qcom_usb4_nhi_stop *st,
  struct qcom_usb4_nhi_retire *r, int (*check)(void *), void *ctx) {
 assert(c == &ctl && q == &host.qnhi && st == &host.host_stop && r == &host.control_retire);
 assert(check == x1_control_owner_stopped && ctx == &host && outer() && !host.lock && !tb.lock);
 return step(CONTROL);
}
static int x1_retire_domain(struct x1_host *h) { assert(h == &host && outer() && !h->lock && !tb.lock); return step(DOMAIN); }
static int x1_retire_session(struct x1_host *h) { assert(h == &host && outer() && !h->lock); return step(SESSION); }
static void reset(void) {
 memset(&host, 0, sizeof(host)); memset(&tb, 0, sizeof(tb)); memset(&dev, 0, sizeof(dev));
 tb.ctl = &ctl; host.dev = &dev; host.tb = host.retained_tb = &tb; host.batch_uid = 0x1234;
 host.tunnel_held = host.cold_owned = host.ready = host.batch_attempted = host.batch_terminal = true;
 host.batch_complete = host.command_attempted = true; host.connected_command = 1; host.error = -ESHUTDOWN;
 host.pcie.nvme_bound = true;
 x1_general = x1_managed = x1_tunnel_quiesce = x1_host_quiesce = x1_power_quiesce = true;
 x1_namespace_retire = x1_pci_retire = x1_control_retire = x1_cm_retire = admin = pulled_port = true;
 trylock_fail = false; infos = fail_at = bad_receipt = 0; ncalls = 0;
}
static bool unlocked(void) { return !dev.lock && !host.bind_lock && !host.lifecycle_lock && !host.lock && !tb.lock; }
static ssize_t pulled_retire_once_store(struct device *, struct device_attribute *, const char *, size_t);
static ssize_t store(const char *token) { return pulled_retire_once_store(&dev, NULL, token, strlen(token)); }
'''

STORE_CASES = r'''
int main(void) {
 const char *token = "retire-pulled-v1\n";
 struct x1_host before;
 int refusals = 0, faults = 0;
 reset();
 assert(store(token) == (ssize_t)strlen(token) && unlocked());
 int ok[] = { MARK, REMOVE, FENCE, CM, STOP, POWER, NAMESPACES, PCI, CONTROL, DOMAIN, SESSION };
 assert(order(ok, 11));
 assert(host.pulled && host.pulled_retire_attempted && host.pulled_retired && !host.pulled_retire_error);
 assert(!strcmp(host.pulled_retire_stage, "pulled-retired") && host.tunnel_stop_attempted && host.tunnel_stop_complete);
 assert(host.host_stop.finished && !host.ready);
 assert(store(token) == -EALREADY && unlocked() && ncalls == 11);
 /* Already removed by an Eject that the pull interrupted: no second removal. */
 reset(); host.pcie.endpoint_removed = true;
 assert(store(token) == (ssize_t)strlen(token) && ncalls == 10 && calls[1] == FENCE);

#define REFUSE(setup, err) do { reset(); setup; memcpy(&before, &host, sizeof(host)); \
  assert(store(token) == (err) && unlocked() && !ncalls && !memcmp(&before, &host, sizeof(host))); refusals++; } while (0)
 REFUSE(admin = false, -EPERM);
 REFUSE(x1_general = false, -EPERM);
 REFUSE(x1_cm_retire = false, -EPERM);
 REFUSE(trylock_fail = true, -EBUSY);
 REFUSE(pulled_port = false, -EPERM);
 REFUSE(host.tunnel_held = false, -EPERM);
 REFUSE(host.retained_tb = NULL, -EPERM);
 REFUSE(host.ready = false, -EPERM);
 REFUSE(host.stopping = true, -EPERM);
 REFUSE(host.suspended = true, -EPERM);
 REFUSE(host.batch_complete = false, -EPERM);
 REFUSE(host.batch_error = -EIO, -EPERM);
 REFUSE(host.error = 0, -EPERM);
 REFUSE(host.tunnel_stop_attempted = true, -EPERM);
 REFUSE(host.host_stop_attempted = true, -EPERM);
 REFUSE(host.power_stop.attempted = true, -EPERM);
 REFUSE(host.pcie.nvme_bound = false, -EPERM);
 reset(); assert(store("retire-idle-v1\n") == -EPERM && !ncalls); refusals++;

#define FAULT(at, n, stage) do { reset(); fail_at = (at); \
  assert(store(token) == -EIO && unlocked() && ncalls == (n) && host.pulled_retire_attempted); \
  assert(!host.pulled_retired && host.pulled_retire_error == -EIO && !strcmp(host.pulled_retire_stage, stage)); \
  assert(store(token) == -EALREADY); faults++; } while (0)
 FAULT(REMOVE, 2, "remove-endpoint");
 FAULT(FENCE, 3, "stop-tunnel");
 FAULT(CM, 4, "stop-tunnel");
 FAULT(STOP, 5, "stop-control");
 FAULT(POWER, 6, "power-stop");
 FAULT(NAMESPACES, 7, "retire-namespaces");
 FAULT(PCI, 8, "retire-pci");
 FAULT(CONTROL, 9, "retire-control");
 FAULT(DOMAIN, 10, "retire-domain");
 FAULT(SESSION, 11, "retire-platform");
 reset(); fail_at = FENCE; store(token); assert(host.tunnel_stop_error == -EIO && !host.tunnel_stop_complete);
 reset(); bad_receipt = 1;
 assert(store(token) == -EIO && ncalls == 5 && !strcmp(host.pulled_retire_stage, "stop-control") && host.ready);
 faults++;
 printf("PASS pulled store: eleven ordered stages, %d refusals change nothing, %d faults terminal\n", refusals, faults);
 return 0;
}
'''


class Pulled(unittest.TestCase):
    def test_mark_config_and_removal(self):
        text = PCIE.read_text()
        code = CONFIG_PRELUDE + '\n'.join(c_function(text, name) for name in (
            'x1_pcie_config_read', 'x1_pcie_config_write', 'x1_pcie_enable_device',
            'qcom_usb4_x1_mark_pulled', 'qcom_usb4_x1_pulled_remove')) + CONFIG_CASES
        print(build_and_run(code, 'x1-pulled-config-'), end='')

    def test_cm_software_stop(self):
        code = TB_PULLED_PRELUDE + c_function(TB.read_text(), 'tb_x1_stop_pcie_pulled') + TB_PULLED_CASES
        print(build_and_run(code, 'x1-pulled-cm-'), end='')

    def test_tunnel_receipt_predicate(self):
        code = PREDICATE_PRELUDE + c_function(HOST.read_text(), 'x1_tunnel_stopped') + PREDICATE_CASES
        print(build_and_run(code, 'x1-pulled-predicate-'), end='')

    def test_detach_marks_before_batch(self):
        code = SWITCH_PRELUDE + c_function(HOST.read_text(), 'x1_switch_set') + SWITCH_CASES
        print(build_and_run(code, 'x1-pulled-switch-'), end='')

    def test_store_order_and_stages(self):
        code = STORE_PRELUDE + c_function(HOST.read_text(), 'pulled_retire_once_store') + STORE_CASES
        print(build_and_run(code, 'x1-pulled-store-'), end='')

    def test_namespace_owner_uses_the_shared_receipt(self):
        body = c_function(HOST.read_text(), 'x1_namespace_owner_stopped')
        self.assertIn('!x1_tunnel_stopped(host)', body)
        self.assertNotIn('tunnel_stop.adapters', body)


if __name__ == '__main__':
    unittest.main()
