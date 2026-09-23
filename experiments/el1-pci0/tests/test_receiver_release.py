#!/usr/bin/env python3
"""Actual DWC release sequencing; effects mocked, no hardware or real IRQ domain."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from source_helpers import function
from test_receiver_stop import PRELUDE, SRC

STUBS = r'''
struct device { bool registered; void *msi_domain; };
struct pci_host_bridge { struct device dev; void *bus; bool msi_domain; };
struct dw_pcie_rp { void *irq_domain; unsigned num_vectors; unsigned long msi_irq_in_use[4];
 int msi_irq[8]; unsigned long msi_data; bool use_imsi_rx; };
struct x1_imsi_receipt { bool ready,retained,associated,domain_ready,target_reserved;
 bool release_attempted,released; int error,release_error; struct x1_imsi_stop stop; };
struct x1_imsi {
 struct { struct device *dev; struct dw_pcie_rp pp; } pci;
 struct { struct pci_host_bridge *bridge; } request;
 struct x1_imsi_receipt receipt;
 int parents[8]; void *target_group; bool target_group_closed;
};
static struct x1_imsi r;
static struct pci_host_bridge bridge;
static struct device provider;
static int x1_imsi_lock,removed,target_frees,checks,owner_error,group_error,bad_parent,action_parent;
static void mutex_lock(int *p) { assert(p==&x1_imsi_lock && !*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
static bool bitmap_empty(unsigned long *bits,unsigned count) {
 assert(count==256); return !(bits[0]|bits[1]|bits[2]|bits[3]);
}
static bool device_is_registered(struct device *d) { return d->registered; }
static void *dev_get_msi_domain(struct device *d) { return d->msi_domain; }
static void *irq_get_handler_data(int irq) {
 assert(x1_imsi_lock && irq>=40 && irq<48); return irq==bad_parent?&r:NULL;
}
static bool irq_has_action(int irq) { return irq==action_parent; }
static int stopped(void *context) {
 assert(context==&r && x1_imsi_lock && !removed && !target_frees); checks++; return owner_error;
}
static void dev_set_msi_domain(struct device *d,void *domain) {
 assert(d==&bridge.dev && !domain && checks==1 && !owner_error && r.receipt.release_attempted);
 d->msi_domain=NULL;
}
static void dw_pcie_free_msi(struct dw_pcie_rp *pp) {
 assert(pp==&r.pci.pp && !removed && !target_frees && x1_imsi_lock);
 assert(!bridge.bus && !bridge.dev.registered && !bridge.dev.msi_domain && !bridge.msi_domain);
 assert(!r.receipt.associated && r.receipt.stop.finished); removed++;
}
static int devres_release_group(struct device *d,void *group) {
 assert(d==&provider && group==&provider && removed==1 && !target_frees);
 assert(!r.pci.pp.irq_domain && !r.pci.pp.use_imsi_rx && !r.receipt.domain_ready && !r.receipt.ready);
 target_frees++; return group_error;
}
'''
REFUSALS = [
    'r.receipt.ready=false', 'r.receipt.error=-EIO', 'r.receipt.retained=false',
    'r.receipt.associated=false', 'r.receipt.domain_ready=false', 'r.receipt.target_reserved=false',
    'r.receipt.stop.attempted=false', 'r.receipt.stop.finished=false',
    'r.receipt.stop.error=-EIO', 'r.receipt.stop.banks_disabled=7',
    'r.receipt.stop.parents_detached=7', 'r.target_group=NULL', 'r.target_group_closed=false',
    'r.pci.pp.irq_domain=NULL', 'r.pci.pp.num_vectors=32', 'r.pci.pp.msi_irq_in_use[1]=1',
    'r.request.bridge=NULL', 'bridge.bus=&bridge', 'bridge.dev.registered=true',
    'bridge.dev.msi_domain=NULL', 'bridge.msi_domain=false', 'r.parents[7]=0',
    'r.pci.pp.msi_irq[6]=45', 'bad_parent=43', 'action_parent=46', 'owner_error=-EIO',
]


def cases():
    mutations = '\n'.join(f'case {i}: {c}; break;' for i, c in enumerate(REFUSALS))
    return r'''
static void init(void) {
 memset(&r,0,sizeof(r)); memset(&bridge,0,sizeof(bridge));
 x1_imsi_lock=removed=target_frees=checks=owner_error=group_error=bad_parent=action_parent=0;
 /* Real devres_release_group returns a released-resource count, not zero. */
 group_error=1;
 r.receipt.ready=r.receipt.retained=r.receipt.associated=r.receipt.domain_ready=r.receipt.target_reserved=true;
 r.receipt.stop.attempted=r.receipt.stop.finished=true;
 r.receipt.stop.banks_disabled=r.receipt.stop.parents_detached=8;
 r.target_group=&provider; r.target_group_closed=true; r.pci.dev=&provider;
 r.pci.pp.irq_domain=&r; r.pci.pp.num_vectors=256; r.pci.pp.msi_data=0x8000;
 r.pci.pp.use_imsi_rx=true; r.request.bridge=&bridge; bridge.msi_domain=true; bridge.dev.msi_domain=&r;
 for(int i=0;i<8;i++) r.parents[i]=r.pci.pp.msi_irq[i]=40+i;
}
static int run(void) { return x1_imsi_release_stopped(&r,stopped,&r); }
int main(void) {
 init(); assert(!run() && !x1_imsi_lock);
 assert(removed==1 && target_frees==1 && r.receipt.released && !r.receipt.target_reserved);
 assert(!r.target_group && !r.pci.pp.msi_data && !r.pci.pp.irq_domain && !r.receipt.ready);
 assert(run()==-EALREADY && removed==1 && target_frees==1 && !x1_imsi_lock);
 for(int i=0;i<COUNT;i++) {
  init(); switch(i) { MUTATIONS }
  assert(run()<0 && !x1_imsi_lock && !removed && !target_frees && !r.receipt.release_attempted);
 }
 init(); group_error=-ENOENT;
 assert(run()==-ENOENT && !x1_imsi_lock && !r.receipt.released && r.receipt.release_error==-ENOENT);
 assert(removed==1 && target_frees==1 && !r.pci.pp.irq_domain && r.receipt.target_reserved);
 assert(run()==-EALREADY && removed==1 && target_frees==1);
 init(); group_error=0;
 assert(run()==-ENOENT && !r.receipt.released && r.receipt.target_reserved);
 assert(run()==-EALREADY && removed==1 && target_frees==1);
 printf("PASS actual receiver release: %d refusals,target-group failure,domain-before-target,no retry\n",COUNT);
}
'''.replace('COUNT', str(len(REFUSALS))).replace('MUTATIONS', mutations)


class ReceiverReleaseTests(unittest.TestCase):
    def test_actual_release(self):
        declaration = re.search(r'struct x1_imsi_stop \{.*?\n\};',
                                (SRC / 'x1-imsi.h').read_text(), re.S).group()
        code = PRELUDE + declaration + STUBS + function(
            (SRC / 'x1-imsi.c').read_text(), 'x1_imsi_release_stopped') + cases()
        with tempfile.TemporaryDirectory(prefix='x1-receiver-release-') as directory:
            src, binary = Path(directory) / 'test.c', Path(directory) / 'test'
            src.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                            '-fsanitize=address,undefined', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_target_group_is_scoped_and_release_is_software_only(self):
        text = (SRC / 'x1-imsi.c').read_text()
        setup = function(text, 'x1_imsi_setup')
        self.assertLess(setup.index('devres_open_group'), setup.index('dw_pcie_msi_host_init'))
        self.assertLess(setup.index('dw_pcie_msi_host_init'), setup.index('devres_close_group'))
        code = re.sub(r'/\*.*?\*/', '', function(text, 'x1_imsi_release_stopped'), flags=re.S)
        for forbidden in ('readl(', 'writel(', 'dma_free_coherent', 'devres_release_all',
                          'enable_irq', 'dw_pcie_msi_init(', 'x1_imsi_consumed = false'):
            self.assertNotIn(forbidden, code)


if __name__ == '__main__':
    unittest.main()
