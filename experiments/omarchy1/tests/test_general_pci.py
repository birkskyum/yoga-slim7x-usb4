#!/usr/bin/env python3
"""Run the actual omarchy-branch PCI functions with mocked cores; no hardware.

Covers the x1_general paths (normal endpoint removal, fence, namespace and PCI
retirement) and reruns the harness PCI retirement scenario unchanged, so the
harness flavor is shown to behave as before when general is false.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'kernel/drivers/thunderbolt/qcom-usb4-x1-pcie.c'


def function(text, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|bool|void)\s*' + name +
                      r'\([^;{]*\)\s*\{', text, re.M)
    if not match:
        raise ValueError(name)
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def build_and_run(code, prefix):
    with tempfile.TemporaryDirectory(prefix=prefix) as directory:
        src, binary = Path(directory) / 'test.c', Path(directory) / 'test'
        src.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined', str(src),
                        '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=True, timeout=20,
                              capture_output=True, text=True).stdout


RETIRE_PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
struct device { void *driver; bool registered; struct { void *data; } msi; };
struct list_head { int count; void *first; };
struct pci_bus { void *sysdata; int number,domain; struct pci_bus *parent; struct list_head devices; };
struct pci_dev { struct device dev; struct pci_bus *bus,*subordinate; int devfn,vendor,device; void *data; };
struct pci_host_bridge { struct device dev; struct pci_bus *bus; };
struct x1_pcie_state {
 bool nvme_bound,host_prepared,host_published,namespace_retire_attempted,namespace_retired;
 bool config_fenced,pci_retire_attempted,nvme_resources_retired,pci_retired;
 bool general,endpoint_removed;
 int error,host_error,cfg_error,namespace_retire_error,pci_retire_error;
 struct { bool released; struct { bool finished; int error,banks_disabled,parents_detached; } stop; } receiver;
 const char *host_step;
};
struct x1_pcie_context {
 struct device *owner; struct x1_pcie_state *state; struct pci_host_bridge *bridge;
 struct pci_dev *endpoint,*root_port; bool receiver_retained,batch_active;
 bool config_open,scan_writes,bind_writes; void *receiver; unsigned long long nvme_session;
};
struct x1_pcie_fenced_action { struct x1_pcie_context *pcie; int (*action)(void *); void *context; };
static struct x1_pcie_context storage;
static struct x1_pcie_context *x1_pcie = &storage;
static struct x1_pcie_state state;
static struct device owner;
static struct pci_host_bridge bridge;
static struct pci_dev endpoint,root;
static struct pci_bus rootbus,childbus;
static int x1_pcie_lock,bus_lock,checks,resources,stops,removes,ref_puts,fault;
#define lockdep_assert_held(p) assert(*(p))
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
static bool pci_is_root_bus(struct pci_bus *b) { return !b->parent; }
static int pci_domain_nr(struct pci_bus *b) { return b->domain; }
static bool list_is_singular(struct list_head *l) { return l->count==1; }
static bool list_empty(struct list_head *l) { return l->count==0; }
#define list_first_entry(l,t,m) ((t *)(l)->first)
static void pci_lock_rescan_remove(void) { assert(x1_pcie_lock && !bus_lock); bus_lock=1; }
static void pci_unlock_rescan_remove(void) { assert(bus_lock); bus_lock=0; }
static int stopped(void *c) {
 assert(c==&state && x1_pcie_lock && bus_lock); checks++;
 if(removes) assert(ref_puts==2 && state.pci_retired);
 return fault==1 || (fault==11 && removes)?-EIO:0;
}
static int nvme_x1_retire_resources(struct pci_dev *p,int (*check)(void *),void *c) {
 assert(!state.general);
 assert(p==&endpoint && check==stopped && c==&state);
 assert(x1_pcie_lock && bus_lock && state.pci_retire_attempted && !resources && checks==1);
 resources++; return fault==2?-EIO:0;
}
static void pci_stop_root_bus(struct pci_bus *b) {
 assert(b==&rootbus && resources==!state.general && !stops && !removes && bus_lock);
 assert(state.nvme_resources_retired && state.config_fenced && !x1_pcie->config_open);
 stops++; endpoint.dev.driver=fault==3?&endpoint:NULL;
 endpoint.data=fault==4?&endpoint:NULL; endpoint.dev.msi.data=fault==5?&endpoint:NULL;
 root.dev.driver=fault==6?&root:NULL;
}
static void *pci_get_drvdata(struct pci_dev *p) { return p->data; }
static void pci_remove_root_bus(struct pci_bus *b) {
 assert(b==&rootbus && resources==!state.general && stops==1 && !removes && bus_lock);
 removes++; bridge.bus=fault==7?&rootbus:NULL;
 endpoint.dev.registered=fault==8; root.dev.registered=fault==9; bridge.dev.registered=fault==10;
}
static bool device_is_registered(struct device *d) { return d->registered; }
static int nvme_x1_close_session(struct pci_dev *p,unsigned long long id) {
 assert(!state.general);
 assert(p==&endpoint && id==1 && removes==1 && !ref_puts && !p->dev.registered);
 return 0;
}
static void pci_dev_put(struct pci_dev *p) {
 assert(p==(ref_puts?&root:&endpoint) && ref_puts<2 && removes==1 && (!fault || fault>=11) && bus_lock); ref_puts++;
}
static int x1_imsi_release_stopped(void *receiver,int (*check)(void *),void *context) {
 assert(receiver==x1_pcie->receiver && state.pci_retired && ref_puts==2);
 int ret=check(context); return ret?ret:fault==12?-EIO:0;
}
static void x1_imsi_cached(void *receiver,void *out) {
 assert(receiver==x1_pcie->receiver && out==&state.receiver);
 state.receiver.released=!fault;
}
'''

HARNESS_REFUSALS = [
    'x1_pcie->owner=NULL', 'x1_pcie->state=NULL', 'x1_pcie->bridge=NULL',
    'x1_pcie->endpoint=NULL', 'x1_pcie->root_port=NULL', 'x1_pcie->receiver_retained=false',
    'x1_pcie->batch_active=true', 'state.nvme_bound=false', 'state.host_prepared=false',
    'state.host_published=false', 'state.error=-EIO', 'state.host_error=-EIO',
    'state.cfg_error=-EIO', 'state.namespace_retire_attempted=false',
    'state.namespace_retired=false', 'state.namespace_retire_error=-EIO',
    'state.config_fenced=false', 'x1_pcie->config_open=true', 'x1_pcie->scan_writes=true',
    'x1_pcie->bind_writes=true', 'state.receiver.stop.finished=false',
    'state.receiver.stop.error=-EIO', 'state.receiver.stop.banks_disabled=7',
    'state.receiver.stop.parents_detached=7', 'bridge.bus=NULL', 'rootbus.sysdata=NULL',
    'rootbus.parent=&childbus', 'rootbus.domain=1', 'rootbus.number=1',
    'root.bus=&childbus', 'root.devfn=1', 'root.vendor=1', 'root.device=1',
    'root.dev.driver=&root', 'root.subordinate=NULL', 'childbus.parent=NULL',
    'endpoint.bus=&rootbus', 'childbus.number=2', 'endpoint.devfn=1',
    'endpoint.vendor=1', 'endpoint.device=1', 'endpoint.subordinate=&childbus',
    'rootbus.devices.count=2', 'childbus.devices.count=2',
    'rootbus.devices.first=&endpoint', 'childbus.devices.first=&root',
]
GENERAL_REFUSALS = [
    'state.endpoint_removed=false', 'childbus.devices=(struct list_head){1,&endpoint}',
    'endpoint.dev.registered=true', 'endpoint.dev.driver=&endpoint', 'root.vendor=1',
    'root.dev.driver=&root', 'rootbus.devices.count=2', 'x1_pcie->endpoint=NULL',
    'state.namespace_retired=false', 'state.config_fenced=false',
]


def retire_cases():
    harness = '\n'.join(f'case {i}: {c}; break;' for i, c in enumerate(HARNESS_REFUSALS))
    general = '\n'.join(f'case {i}: {c}; break;' for i, c in enumerate(GENERAL_REFUSALS))
    return r'''
static void init(void) {
 memset(x1_pcie,0,sizeof(*x1_pcie)); memset(&state,0,sizeof(state));
 memset(&endpoint,0,sizeof(endpoint)); memset(&root,0,sizeof(root));
 memset(&bridge,0,sizeof(bridge)); memset(&rootbus,0,sizeof(rootbus)); memset(&childbus,0,sizeof(childbus));
 x1_pcie_lock=bus_lock=checks=resources=stops=removes=ref_puts=fault=0;
 x1_pcie->owner=&owner; x1_pcie->state=&state; x1_pcie->bridge=&bridge;
 x1_pcie->endpoint=&endpoint; x1_pcie->root_port=&root; x1_pcie->receiver_retained=true;
 x1_pcie->nvme_session=1;
 state.nvme_bound=state.host_prepared=state.host_published=true;
 state.namespace_retire_attempted=state.namespace_retired=state.config_fenced=true;
 state.receiver.stop.finished=true; state.receiver.stop.banks_disabled=state.receiver.stop.parents_detached=8;
 bridge.bus=&rootbus; rootbus.sysdata=x1_pcie;
 root.bus=&rootbus; root.subordinate=&childbus; root.vendor=0x17cb; root.device=0x111;
 endpoint.bus=&childbus; endpoint.vendor=0x1c19; endpoint.device=0x102b;
 endpoint.dev.driver=endpoint.data=endpoint.dev.msi.data=&endpoint;
 childbus.parent=&rootbus; childbus.number=1;
 rootbus.devices=(struct list_head){1,&root}; childbus.devices=(struct list_head){1,&endpoint};
 bridge.dev.registered=root.dev.registered=endpoint.dev.registered=true;
}
/* After normal PCI removal: any vendor, no driver, not registered, bus 1 empty. */
static void init_general(void) {
 init(); state.general=state.endpoint_removed=true; x1_pcie->nvme_session=0;
 endpoint.vendor=0x144d; endpoint.device=0xa80a;
 endpoint.dev.driver=endpoint.data=endpoint.dev.msi.data=NULL; endpoint.dev.registered=false;
 childbus.devices=(struct list_head){0,NULL};
}
static int run(void) { return qcom_usb4_x1_retire_pci(&owner,&state,stopped,&state); }
static void unlocked(void) { assert(!x1_pcie_lock && !bus_lock); }
int main(void) {
 init(); assert(!run()); unlocked();
 assert(state.nvme_resources_retired && state.pci_retired && !state.pci_retire_error);
 assert(!bridge.bus && !x1_pcie->endpoint && !x1_pcie->root_port && ref_puts==2);
 assert(!state.nvme_bound && !state.host_prepared && !state.host_published);
 assert(run()<0 && resources==1 && removes==1 && ref_puts==2); unlocked();
 for(int i=0;i<HCOUNT;i++) {
  init(); switch(i) { HMUTATIONS }
  assert(run()<0); unlocked(); assert(!resources && !stops && !removes && !ref_puts);
 }
 for(int i=1;i<=12;i++) {
  init(); fault=i; assert(run()<0); unlocked();
  assert(state.pci_retired==(i>=11) && ref_puts==(i>=11?2:0));
  assert(resources==(i>=2) && stops==(i>=3) && removes==(i>=7));
 }
 init_general(); assert(!run()); unlocked();
 assert(state.nvme_resources_retired && state.pci_retired && !state.pci_retire_error);
 assert(!resources && stops==1 && removes==1 && ref_puts==2 && !bridge.bus);
 assert(!x1_pcie->endpoint && !x1_pcie->root_port && state.receiver.released);
 for(int i=0;i<GCOUNT;i++) {
  init_general(); switch(i) { GMUTATIONS }
  assert(run()<0); unlocked(); assert(!resources && !stops && !removes && !ref_puts);
 }
 for(int i=3;i<=12;i++) {
  if(i==2) continue;
  init_general(); fault=i; assert(run()<0); unlocked(); assert(!resources);
  assert(stops==1 && removes==(i>=7) && state.pci_retired==(i>=11));
 }
 printf("PASS harness %d refusals + 12 faults; general %d refusals + 10 faults\n",HCOUNT,GCOUNT);
 return 0;
}
'''.replace('HCOUNT', str(len(HARNESS_REFUSALS))).replace('HMUTATIONS', harness) \
   .replace('GCOUNT', str(len(GENERAL_REFUSALS))).replace('GMUTATIONS', general)


REMOVE_PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define __iomem
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x4
#define PCI_MSIX_FLAGS 2
#define PCI_MSIX_FLAGS_ENABLE 0x8000
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
typedef uint16_t u16;
typedef uint32_t u32;
struct device { void *driver; bool registered; struct { void *data; } msi; };
struct pci_dev { struct device dev; void *data; };
struct receipt { bool ready; struct { bool finished; int error,banks_disabled,parents_detached; } stop; };
struct x1_pcie_state {
 bool general,nvme_requested,host_prepared,host_published,nvme_bound,nvme_bind_attempted;
 bool config_fenced,endpoint_remove_attempted,endpoint_removed;
 bool namespace_retire_attempted,namespace_retired;
 int error,host_error,cfg_error,endpoint_remove_error,namespace_retire_error;
 struct receipt receiver; const char *host_step;
};
struct x1_pcie_inventory { u16 msix_offset; };
struct x1_pcie_context {
 struct device *owner; struct x1_pcie_state *state; struct pci_dev *endpoint,*root_port;
 bool receiver_retained,batch_active,config_open,scan_writes,bind_writes;
 void *receiver; char *config; struct x1_pcie_inventory *inventory;
};
struct x1_pcie_fenced_action { struct x1_pcie_context *pcie; int (*action)(void *); void *context; };
static struct x1_pcie_context storage;
static struct x1_pcie_context *x1_pcie = &storage;
static struct x1_pcie_state state;
static struct x1_pcie_inventory inventory = { .msix_offset = 0xb0 };
static struct device owner;
static struct pci_dev endpoint,root;
static char config[4096];
static int x1_pcie_lock,bus_lock,removals,imsi_stops,actions,private_calls,fault;
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
#define raw_spin_lock_irqsave(l,f) do { (void)(f); assert(!*(l)); *(l)=1; } while (0)
#define raw_spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while (0)
static int x1_config_lock;
static u16 readw(const char *p) { u16 v; memcpy(&v,p,2); assert(x1_config_lock); return v; }
static void put16(int off,u16 v) { memcpy(config+off,&v,2); }
static int x1_pcie_host_local(struct x1_pcie_context *c) { assert(x1_config_lock); return fault==5?-EIO:0; }
static void pci_lock_rescan_remove(void) { assert(x1_pcie_lock && !bus_lock); bus_lock=1; }
static void pci_unlock_rescan_remove(void) { assert(bus_lock); bus_lock=0; }
static void pci_stop_and_remove_bus_device(struct pci_dev *p) {
 assert(p==&endpoint && bus_lock && x1_pcie_lock && !removals && !state.config_fenced);
 assert(x1_pcie->config_open); removals++;
 /* nvme_remove shut the controller down and freed its vectors. */
 p->dev.driver=fault==1?p:NULL; p->data=fault==2?p:NULL; p->dev.msi.data=NULL;
 p->dev.registered=fault==3;
 put16(PCI_COMMAND,fault==4?PCI_COMMAND_MASTER:0);
 put16(inventory.msix_offset+PCI_MSIX_FLAGS,fault==6?PCI_MSIX_FLAGS_ENABLE:0x1f);
}
static void *pci_get_drvdata(struct pci_dev *p) { return p->data; }
static bool device_is_registered(struct device *d) { return d->registered; }
static int x1_imsi_stop_retained(void *r) {
 assert(r==x1_pcie->receiver && state.config_fenced && !x1_pcie->config_open && !x1_config_lock);
 imsi_stops++; return 0;
}
static void x1_imsi_cached(void *r,struct receipt *out) {
 assert(r==x1_pcie->receiver && out==&state.receiver);
 out->stop.finished=true; out->stop.banks_disabled=out->stop.parents_detached=8;
}
static int tunnel_stop(void *c) {
 assert(c==&state && state.config_fenced && imsi_stops==1 && x1_pcie_lock);
 actions++; return 0;
}
static int nvme_x1_retire_irqs_and_action(struct pci_dev *p,int (*a)(void *),void *c) {
 assert(!state.general); private_calls++; return a(c);
}
static int nvme_x1_retire_namespaces(struct pci_dev *p,int (*check)(void *),void *c) {
 assert(!state.general); private_calls++; return check(c);
}
static int check_stopped(void *c) { assert(c==&state && x1_pcie_lock); return 0; }
'''


def remove_cases():
    return r'''
static void init(bool general) {
 memset(x1_pcie,0,sizeof(*x1_pcie)); memset(&state,0,sizeof(state)); memset(config,0,sizeof(config));
 memset(&endpoint,0,sizeof(endpoint)); memset(&root,0,sizeof(root));
 x1_pcie_lock=bus_lock=x1_config_lock=removals=imsi_stops=actions=private_calls=fault=0;
 x1_pcie->owner=&owner; x1_pcie->state=&state; x1_pcie->endpoint=&endpoint; x1_pcie->root_port=&root;
 x1_pcie->receiver_retained=x1_pcie->config_open=true; x1_pcie->receiver=&owner;
 x1_pcie->config=config; x1_pcie->inventory=&inventory;
 state.general=general; state.nvme_requested=state.host_prepared=state.host_published=true;
 state.nvme_bound=state.nvme_bind_attempted=state.receiver.ready=true;
 endpoint.dev.driver=endpoint.data=endpoint.dev.msi.data=&endpoint; endpoint.dev.registered=true;
 put16(PCI_COMMAND,PCI_COMMAND_MASTER|2); put16(inventory.msix_offset+PCI_MSIX_FLAGS,PCI_MSIX_FLAGS_ENABLE|0x1f);
}
static void unlocked(void) { assert(!x1_pcie_lock && !bus_lock && !x1_config_lock); }
static int eject(void) {
 int ret=qcom_usb4_x1_general_remove(&owner,&state); unlocked();
 if(!ret) { ret=qcom_usb4_x1_fence_and_quiesce(&owner,&state,tunnel_stop,&state); unlocked(); }
 if(!ret) { ret=qcom_usb4_x1_retire_namespaces(&owner,&state,check_stopped,&state); unlocked(); }
 return ret;
}
int main(void) {
 init(true); assert(!eject());
 assert(removals==1 && state.endpoint_removed && !state.endpoint_remove_error);
 assert(state.config_fenced && !x1_pcie->config_open && imsi_stops==1 && actions==1);
 assert(state.namespace_retired && !private_calls && !state.error);
 assert(qcom_usb4_x1_general_remove(&owner,&state)<0); unlocked(); assert(removals==1);
 for(int f=1;f<=6;f++) {
  init(true); fault=f; assert(eject()<0);
  assert(removals==1 && !state.endpoint_removed && state.endpoint_remove_error && state.error);
  assert(!imsi_stops && !actions && !state.config_fenced && !state.namespace_retired);
 }
 /* The fence refuses a general session whose endpoint was never removed. */
 init(true); assert(qcom_usb4_x1_fence_and_quiesce(&owner,&state,tunnel_stop,&state)<0); unlocked();
 assert(!actions && !state.config_fenced);
 /* Refusals before any removal. */
 const char *names[]={"general","bound","fenced","batch","endpoint","receiver"};
 for(int i=0;i<6;i++) {
  init(true);
  switch(i) { case 0: state.general=false; break; case 1: state.nvme_bound=false; break;
   case 2: state.config_fenced=true; break; case 3: x1_pcie->batch_active=true; break;
   case 4: x1_pcie->endpoint=NULL; break; case 5: state.receiver.ready=false; break; }
  if(qcom_usb4_x1_general_remove(&owner,&state)>=0) { printf("accepted %s\n",names[i]); return 1; }
  unlocked(); assert(!removals);
 }
 /* Harness flavor still goes through the private NVMe helpers. */
 init(false); state.endpoint_removed=false;
 assert(!qcom_usb4_x1_fence_and_quiesce(&owner,&state,tunnel_stop,&state)); unlocked();
 assert(private_calls==1 && actions==1 && !removals);
 assert(!qcom_usb4_x1_retire_namespaces(&owner,&state,check_stopped,&state)); unlocked();
 assert(private_calls==2);
 printf("PASS general removal, fence, namespaces: 6 faults, 6 refusals, harness path unchanged\n");
 return 0;
}
'''


class GeneralPci(unittest.TestCase):
    def test_retire_pci_harness_and_general(self):
        text = SOURCE.read_text()
        code = RETIRE_PRELUDE + '\n'.join(function(text, name) for name in (
            'x1_pcie_retire_shape', 'x1_pcie_receiver_release_proof',
            'qcom_usb4_x1_retire_pci')) + retire_cases()
        print(build_and_run(code, 'x1-general-retire-'), end='')

    def test_general_removal_fence_and_namespaces(self):
        text = SOURCE.read_text()
        code = REMOVE_PRELUDE + '\n'.join(function(text, name) for name in (
            'qcom_usb4_x1_general_remove', 'x1_pcie_fence_action',
            'qcom_usb4_x1_fence_and_quiesce', 'qcom_usb4_x1_retire_namespaces')) + remove_cases()
        print(build_and_run(code, 'x1-general-remove-'), end='')

    def test_admission_requires_opt_in_absent_only_for_general(self):
        text = SOURCE.read_text()
        body = function(text, 'x1_pcie_admit')
        self.assertIn('of_property_read_bool(np, "qcom,x1-private-nvme-read-test") == '
                      'x1_pcie->state->general', body)

    def test_private_session_skipped_only_for_general(self):
        body = function(SOURCE.read_text(), 'qcom_usb4_x1_nvme_bind')
        self.assertIn('if (!ret && !state->general)\n\t\tret = nvme_x1_open_session', body)

    def test_general_endpoint_keeps_its_dma_configuration(self):
        # of_dma_configure() replaces bus_dma_limit at probe; 64-bit DMA is proven.
        self.assertNotIn('bus_dma_limit', SOURCE.read_text())


if __name__ == '__main__':
    unittest.main()
