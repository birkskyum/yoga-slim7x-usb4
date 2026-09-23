#!/usr/bin/env python3
"""Execute the actual iMSI receiver stop with mocked IRQ/MMIO effects."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from source_helpers import function

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / 'kernel/drivers/thunderbolt'
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define MSI_REG_CTRL_BLOCK_SIZE 12
#define PCIE_MSI_INTR0_MASK 0x82c
#define PCIE_MSI_INTR0_ENABLE 0x828
'''
STUBS = r'''
struct dw_pcie_rp {
 int lock; void *irq_domain; bool use_imsi_rx;
 unsigned num_vectors; int msi_irq[8]; u32 irq_mask[8];
 unsigned long msi_irq_in_use[4];
};
struct x1_imsi {
 struct { struct dw_pcie_rp pp; void *dbi_base; } pci;
 struct { void *dbi; } request;
 struct { bool ready,retained,associated,domain_ready,target_reserved;
  int error,banks; struct x1_imsi_stop stop; } receipt;
 int parents[8];
};
static struct x1_imsi r;
static int x1_imsi_lock,depth,writes,fail_write,disabled,detached;
static int wrong_parent,stuck_parent;
static void *handler[8];
static int memory;
static void mutex_lock(int *p) { assert(p==&x1_imsi_lock && !*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p && !depth); *p=0; }
#define raw_spin_lock_irqsave(p,f) do { assert(x1_imsi_lock && !(p)[0] && !depth); (p)[0]=1; depth=1; (f)=0; } while(0)
#define raw_spin_unlock_irqrestore(p,f) do { assert((p)[0] && depth && !(f)); (p)[0]=0; depth=0; } while(0)
static bool bitmap_empty(unsigned long *bits,unsigned count) {
 assert(depth && count==256); return !(bits[0]|bits[1]|bits[2]|bits[3]);
}
static void *irq_get_handler_data(int irq) {
 assert(x1_imsi_lock && irq>=40 && irq<48);
 if(wrong_parent==irq) return NULL;
 return handler[irq-40];
}
static int x1_imsi_write_checked(void *dbi,u32 offset,u32 value) {
 assert(depth && dbi==&memory && r.receipt.stop.attempted);
 int bank=writes/2;
 assert(offset==(writes%2?PCIE_MSI_INTR0_ENABLE:PCIE_MSI_INTR0_MASK)+bank*12);
 assert(value==(writes%2?0:U32_MAX) && r.pci.pp.irq_mask[bank]==U32_MAX);
 writes++; return writes==fail_write?-EIO:0;
}
static void disable_irq(int irq) {
 assert(!depth && x1_imsi_lock && r.receipt.stop.attempted && irq==40+disabled);
 disabled++;
}
static void irq_set_chained_handler_and_data(int irq,void *fn,void *data) {
 assert(!depth && disabled==detached+1 && !fn && !data && irq==40+detached);
 if(irq!=stuck_parent) handler[irq-40]=NULL;
 detached++;
}
'''
CASES = r'''
static void init(void) {
 memset(&r,0,sizeof(r)); x1_imsi_lock=depth=writes=fail_write=disabled=detached=0;
 wrong_parent=stuck_parent=0;
 r.receipt.ready=r.receipt.retained=r.receipt.associated=true;
 r.receipt.domain_ready=r.receipt.target_reserved=r.pci.pp.use_imsi_rx=true;
 r.pci.pp.irq_domain=&r; r.pci.pp.num_vectors=256; r.receipt.banks=8;
 r.pci.dbi_base=r.request.dbi=&memory;
 for(int i=0;i<8;i++) r.parents[i]=r.pci.pp.msi_irq[i]=40+i,handler[i]=&r.pci.pp;
}
static void retained(void) {
 assert(r.pci.pp.irq_domain==&r && r.receipt.target_reserved && r.receipt.associated);
 assert(!depth && !x1_imsi_lock);
}
int main(void) {
 init(); assert(!x1_imsi_stop_retained(&r)); retained();
 assert(writes==16 && disabled==8 && detached==8 && r.receipt.stop.finished);
 assert(r.receipt.stop.banks_disabled==8 && r.receipt.stop.parents_detached==8);
 assert(x1_imsi_stop_retained(&r)==-EALREADY && writes==16 && disabled==8);
 for(int bad=0;bad<16;bad++) {
  init();
  switch(bad) {
   case 0:r.receipt.ready=false;break; case 1:r.receipt.error=-EIO;break;
   case 2:r.receipt.retained=false;break; case 3:r.receipt.associated=false;break;
   case 4:r.receipt.domain_ready=false;break; case 5:r.receipt.target_reserved=false;break;
   case 6:r.pci.pp.irq_domain=NULL;break; case 7:r.pci.pp.use_imsi_rx=false;break;
   case 8:r.pci.pp.num_vectors=32;break; case 9:r.receipt.banks=1;break;
   case 10:r.pci.dbi_base=NULL;break; case 11:r.parents[7]=0;break;
   case 12:r.pci.pp.msi_irq[6]=49;break; case 13:wrong_parent=44;break;
   case 14:r.pci.pp.msi_irq_in_use[3]=1;break; case 15:r.receipt.stop.attempted=true;break;
  }
  assert(x1_imsi_stop_retained(&r)<0 && !writes && !disabled && !detached);
  assert(!depth && !x1_imsi_lock && !r.receipt.stop.finished);
 }
 for(int fault=1;fault<=16;fault++) {
  init(); fail_write=fault;
  assert(x1_imsi_stop_retained(&r)==-EIO); retained();
  assert(writes==fault && disabled==8 && detached==8 && !r.receipt.stop.finished);
  assert(r.receipt.stop.error==-EIO && r.receipt.stop.banks_disabled==(fault-1)/2);
  assert(r.receipt.stop.parents_detached==8);
  assert(x1_imsi_stop_retained(&r)==-EALREADY && writes==fault && disabled==8);
 }
 init(); stuck_parent=45;
 assert(x1_imsi_stop_retained(&r)==-EIO); retained();
 assert(!r.receipt.stop.finished && r.receipt.stop.parents_detached==7);
 puts("PASS actual receiver stop:16 admission refusals,16 MMIO failures,parent detach failure,no lock held over IRQ sync");
}
'''


class ReceiverStopTests(unittest.TestCase):
    def test_actual_receiver_stop(self):
        declaration = re.search(r'struct x1_imsi_stop \{.*?\n\};',
                                (SRC / 'x1-imsi.h').read_text(), re.S).group()
        code = PRELUDE + declaration + STUBS + function(
            (SRC / 'x1-imsi.c').read_text(), 'x1_imsi_stop_retained') + CASES
        with tempfile.TemporaryDirectory(prefix='x1-receiver-stop-') as directory:
            source, binary = Path(directory) / 'test.c', Path(directory) / 'test'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                            '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_keeps_domain_and_target_until_pci_children_are_removed(self):
        code = function((SRC / 'x1-imsi.c').read_text(), 'x1_imsi_stop_retained')
        for forbidden in ('irq_domain_remove', 'dw_pcie_free_msi', 'dma_free',
                          'dev_set_msi_domain', 'enable_irq', 'x1_imsi_setup'):
            self.assertNotIn(forbidden, code)
        self.assertLess(code.index('bitmap_empty'), code.index('x1_imsi_write_checked'))
        self.assertLess(code.rindex('raw_spin_unlock_irqrestore'), code.index('disable_irq('))


if __name__ == '__main__':
    unittest.main()
