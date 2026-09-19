/* Exact production headers, external providers mocked. No write, synthetic
 * interrupt or sleep provider is defined: accidental use fails the build. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include "kernel/include/uapi/linux/pci_regs.h"
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define IS_ENABLED(c) (c)
#define PCI_IRQ_MSIX 4U
#define PCI_IRQ_MSI 2U
#define PCI_IRQ_INTX 1U
#define PCI_IRQ_AFFINITY 8U
#define IORESOURCE_MEM 0x200
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define __iomem
#define READ_ONCE(a) (a)
#define le16_to_cpu(a) (a)
struct device { int unused; };
struct pci_bus { int domain; };
struct pci_dev { struct device dev; bool msi_enabled, msix_enabled; int msix_cap, aer_cap;
    struct pci_bus *bus; u16 vendor, device; unsigned int rid, type; };
struct device_node { const char *full_name; int refs; };
typedef struct { int n; } atomic_t;
#define ATOMIC_INIT(x) {x}
static void atomic_inc(atomic_t *a) { a->n++; }
static int atomic_read(atomic_t *a) { return a->n; }
static int atomic_cmpxchg(atomic_t *a,int old,int n) { int v=a->n; if(v==old)a->n=n; return v; }
struct irq_chip { const char *name; void *irq_set_irqchip_state; };
struct msi_msg { u32 address_lo, address_hi, data; };
struct msi_desc { struct device *dev; unsigned int msi_index;
    struct { struct { bool is_msix, is_virtual; } msi_attrib; void *mask_base; } pci; };
struct irq_domain { const char *name; };
struct irq_data { struct irq_domain *domain; unsigned long hwirq; struct irq_data *parent_data; };
static struct pci_dev endpoint, root;
static struct pci_bus bus;
static struct device wrongdev;
static struct device_node controller, decoy;
static struct irq_chip chip;
static struct irq_domain domains[2] = {{"ITS-PCI-MSIX"}, {"ITS"}};
static struct irq_data irqdata[2];
static struct msi_desc desc;
static struct msi_msg cached;
static u32 entries[8], table_config, pba_config, pba_bits, unc_status;
static u16 control;
static bool admitted, no_irq, no_desc, missing_path, null_route, wrong_controller, map_fail, no_root;
static int config_reads, mmio_reads, node_puts, fail_at, ops, arg_ops, map_calls, unmaps;
static int root_reads, header_reads, maps_live, hang_at;
static unsigned long bar_flags, bar_start, bar_len;
static u32 translated_id;
static jmp_buf interrupted;
static char logbuf[65536];
static size_t loglen;
static void record(struct device *dev, const char *fmt, ...)
{
    va_list ap; (void)dev; va_start(ap,fmt);
    int n=vsnprintf(logbuf+loglen,sizeof(logbuf)-loglen,fmt,ap);
    va_end(ap); assert(n>=0 && (size_t)n<sizeof(logbuf)-loglen); loglen+=n;
}
#define dev_emerg record
/* Backend has its own exact-production test. This suite keeps its existing
 * NVMe MMIO/read counts and tests the caller independently. */
static void __attribute__((unused)) x1_its_msi_audit(struct pci_dev *p, unsigned int irq, bool timeout)
{ assert(p==&endpoint && (irq==144 || irq==145)); (void)timeout; }
static bool x1_native_nvme_allowed(struct pci_dev *p) { return admitted && p==&endpoint; }
static int pci_irq_vector(struct pci_dev *p, unsigned int v) { assert(p==&endpoint && v<2); return 144+v; }
static unsigned int pci_dev_id(struct pci_dev *p) { assert(p==&endpoint || p==&root); return p->rid; }
static void get_cached_msi_msg(int irq, struct msi_msg *msg) { assert(!no_irq && !no_desc); assert(irq==144 || irq==145); *msg=cached; }
static u32 of_msi_xlate(struct device *d, struct device_node **np, u32 rid)
{ assert(d==&endpoint.dev && rid==0x100); *np=null_route?NULL:(wrong_controller?&decoy:&controller); if(*np)(*np)->refs++; return translated_id; }
static struct device_node *of_find_node_by_path(const char *path)
{ assert(!strcmp(path,"/soc@0/interrupt-controller@17000000/msi-controller@17040000")); if(missing_path)return NULL; controller.refs++; return &controller; }
static void of_node_put(struct device_node *np)
{ if(!np)return; assert((np==&controller || np==&decoy) && np->refs>0); np->refs--; node_puts++; }
static struct irq_data *irq_get_irq_data(int irq) { assert(irq>=144 && irq<=145); return no_irq?NULL:&irqdata[0]; }
static struct msi_desc *irq_data_get_msi_desc(struct irq_data *d) { assert(d==&irqdata[0]); return no_desc?NULL:&desc; }
static int config_access(struct pci_dev *p)
{ assert(p==&endpoint || p==&root); config_reads++; if(p==&root)root_reads++; return config_reads==fail_at?0x86:0; }
static int pci_read_config_word(struct pci_dev *p, int off, u16 *out)
{
    int ret=config_access(p); if(ret)return ret;
    switch(off) { case 0x82: assert(p==&endpoint); *out=control;break;
    case PCI_COMMAND:*out=6;break; case PCI_STATUS:*out=0x10;break; default:assert(0); } return 0;
}
static int pci_read_config_dword(struct pci_dev *p, int off, u32 *out)
{
    int ret=config_access(p); if(ret)return ret;
    if(off==0x84) { assert(p==&endpoint); *out=table_config; }
    else if(off==0x88) { assert(p==&endpoint); *out=pba_config; }
    else {
        assert(p->aer_cap>=0x100 && p->aer_cap<=0xfc0 && !(p->aer_cap&3));
        switch(off-p->aer_cap) {
        case PCI_ERR_UNCOR_STATUS:*out=unc_status;break;
        case PCI_ERR_UNCOR_MASK:case PCI_ERR_COR_STATUS:case PCI_ERR_COR_MASK:case PCI_ERR_CAP:*out=0;break;
        case PCI_ERR_HEADER_LOG:case PCI_ERR_HEADER_LOG+4:case PCI_ERR_HEADER_LOG+8:case PCI_ERR_HEADER_LOG+12:
            header_reads++;*out=0x12340000+off;break; default:assert(0);
        }
    } return 0;
}
static int pcie_capability_read_word(struct pci_dev *p, int off, u16 *out)
{ assert(off==PCI_EXP_DEVSTA); int ret=config_access(p); if(!ret)*out=0; return ret; }
static unsigned long pci_resource_flags(struct pci_dev *p,int b) { assert(p==&endpoint && !b); return bar_flags; }
static unsigned long pci_resource_start(struct pci_dev *p,int b) { assert(p==&endpoint && !b); return bar_start; }
static unsigned long pci_resource_len(struct pci_dev *p,int b) { assert(p==&endpoint && !b); return bar_len; }
static void *pci_iomap_range(struct pci_dev *p,int b,unsigned long off,unsigned long size)
{ assert(p==&endpoint && !b && off==0x3000 && size==4 && bar_len>=off+size && bar_start && (bar_flags&IORESOURCE_MEM)); map_calls++; if(map_fail)return NULL; maps_live++;return &pba_bits; }
static void pci_iounmap(struct pci_dev *p,void *a) { assert(p==&endpoint && a==&pba_bits && maps_live==1); maps_live--;unmaps++; }
static struct pci_dev *pci_upstream_bridge(struct pci_dev *p) { assert(p==&endpoint); return no_root?NULL:&root; }
static int pci_domain_nr(struct pci_bus *b) { assert(b==&bus); return b->domain; }
static int pci_pcie_type(struct pci_dev *p) { assert(p==&root); return p->type; }
static u32 readl(void *p)
{ assert(p==&pba_bits || ((uintptr_t)p>=(uintptr_t)entries && (uintptr_t)p<(uintptr_t)(entries+8))); if(p==&pba_bits)assert(maps_live==1); mmio_reads++; if(mmio_reads==hang_at)longjmp(interrupted,1); return *(u32*)p; }
static struct irq_chip *irq_data_get_irq_chip(struct irq_data *d) { (void)d; return &chip; }
#include "kernel/drivers/nvme/host/x1-nvme-irq.h"
struct mutex { bool held, busy; };
static int lock_calls, unlock_calls;
static bool mutex_trylock(struct mutex *m) { lock_calls++; if(m->busy)return false; assert(!m->held); return m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; unlock_calls++; }
#define NVMEQ_ENABLED 0
#define NVMEQ_POLLED 1
static bool test_bit(unsigned int n, unsigned long *p) { return !!(*p & (1UL<<n)); }
struct nvme_completion { u16 status; };
struct nvme_dev { struct device *dev; struct mutex shutdown_lock; unsigned int bar_mapped_size; };
struct nvme_queue { struct nvme_dev *dev; u16 qid, cq_vector, cq_head, q_depth; u8 cq_phase; unsigned long flags; struct nvme_completion *cqes; };
static struct nvme_dev nvdev;
static struct nvme_queue queue;
static struct nvme_completion cq[2];
static struct pci_dev *to_pci_dev(struct device *d) { assert(d==&endpoint.dev); return &endpoint; }
#include "kernel/drivers/nvme/host/x1-nvme-irq-timeout.h"
static int operation(int ret) { ops++;return ret; }
static unsigned int arg(void) { arg_ops++;return 0; }
static void reset(void)
{
    endpoint=(struct pci_dev){.msix_enabled=true,.msix_cap=0x80,.aer_cap=0x100,.bus=&bus,.rid=0x100};
    root=(struct pci_dev){.aer_cap=0x100,.bus=&bus,.vendor=0x17cb,.device=0x0111,.type=PCI_EXP_TYPE_ROOT_PORT};
    bus.domain=0;admitted=true;
    desc=(struct msi_desc){.dev=&endpoint.dev,.msi_index=1,.pci={.msi_attrib={.is_msix=true},.mask_base=entries}};
    irqdata[0]=(struct irq_data){.domain=&domains[0],.hwirq=1,.parent_data=&irqdata[1]};
    irqdata[1]=(struct irq_data){.domain=&domains[1],.hwirq=8193};
    cached=(struct msi_msg){.address_lo=0x17050040,.data=1};
    memset(entries,0,sizeof(entries));entries[4]=cached.address_lo;entries[6]=cached.data;
    control=PCI_MSIX_FLAGS_ENABLE|32;table_config=0x2000;pba_config=0x3000;
    no_irq=no_desc=map_fail=no_root=false;hang_at=0;
    config_reads=mmio_reads=node_puts=fail_at=ops=arg_ops=map_calls=unmaps=0;
    root_reads=header_reads=maps_live=0;pba_bits=unc_status=0;
    bar_flags=IORESOURCE_MEM;bar_start=0x40000000;bar_len=0x4000;
    assert(!controller.refs && !decoy.refs);controller.full_name=decoy.full_name="msi-controller@17040000";
    missing_path=null_route=wrong_controller=false;translated_id=0x80100;chip=(struct irq_chip){"ITS",(void *)1};
    memset(x1_irq_entries,0,sizeof(x1_irq_entries));memset(x1_irq_timeouts,0,sizeof(x1_irq_timeouts));
    nvdev=(struct nvme_dev){.dev=&endpoint.dev,.bar_mapped_size=8192};
    queue=(struct nvme_queue){.dev=&nvdev,.qid=1,.cq_vector=1,.q_depth=2,.cq_phase=1,.flags=1,.cqes=cq};
    cq[0].status=1;cq[1].status=0;lock_calls=unlock_calls=0;loglen=0;logbuf[0]=0;
}
static void state(void) { x1_nvme_irq_state(&endpoint,1,1,"timeout"); }
int main(void)
{
    for(unsigned int f=0;f<16;f++) { reset();assert(x1_nvme_irq_flags(&endpoint,f)==(IS_ENABLED(CONFIG_USB4_X1_NATIVE)?PCI_IRQ_MSIX|(f&PCI_IRQ_AFFINITY):f));admitted=false;assert(x1_nvme_irq_flags(&endpoint,f)==f); }
    reset();for(unsigned int n=0;n<32;n++)assert(x1_nvme_irq_count(&endpoint,n)==(IS_ENABLED(CONFIG_USB4_X1_NATIVE)?min(n,2U):n));
    for(int err=-110;err<=0;err++) { reset();assert(X1_NVME_IRQ_REQUEST(&endpoint,arg(),arg(),operation(err))==err);assert(ops==1 && arg_ops==2);if(err || !IS_ENABLED(CONFIG_USB4_X1_NATIVE))assert(!mmio_reads && !config_reads); }
    reset();state();x1_nvme_irq_timeout(&queue,1);
    if(!IS_ENABLED(CONFIG_USB4_X1_NATIVE)) { assert(!loglen && !config_reads && !mmio_reads && !lock_calls);puts("PASS MSI-X production policy: disabled configuration untouched.");return 0; }
    reset();x1_nvme_irq_snapshot(&endpoint,1,1,0);assert(config_reads==3 && mmio_reads==4 && node_puts==2);
    assert(strstr(logbuf,"node=1 devid=1 address=1 event=1 its=1 queue=1 index=1 function_unmasked=1 vector_unmasked=1 cache=1"));assert(!strstr(logbuf,"SOFTINT"));
    reset();state();assert(map_calls==1 && unmaps==1 && !maps_live && mmio_reads==5 && root_reads==8);assert(strstr(logbuf,"pending=0 valid=1") && strstr(logbuf,"role=endpoint") && strstr(logbuf,"role=root"));
    reset();control|=PCI_MSIX_FLAGS_MASKALL;entries[7]=1;pba_bits=2;state();assert(strstr(logbuf,"ctrl=c020") && strstr(logbuf,"mask=00000001") && strstr(logbuf,"pending=1 valid=1"));
    reset();pba_bits=0xffffffff;state();assert(strstr(logbuf,"valid=0"));reset();entries[6]=99;state();assert(strstr(logbuf,"cache_match=0"));
    for(int f=0;f<6;f++) { reset();switch(f) { case 0:table_config=0x2001;break;case 1:pba_config=0x3001;break;case 2:bar_flags=0;break;case 3:bar_start=0;break;case 4:bar_len=0x3003;break;case 5:map_fail=true;break; }state();assert(mmio_reads==4 && !unmaps && !maps_live && map_calls==(f==5) && strstr(logbuf,"PBA SKIP")); }
    for(int f=0;f<6;f++) { reset();switch(f) { case 0:no_root=true;break;case 1:bus.domain=1;break;case 2:root.rid=8;break;case 3:root.vendor=0xffff;break;case 4:root.device=0xffff;break;case 5:root.type=PCI_EXP_TYPE_ENDPOINT;break; }state();assert(!root_reads && strstr(logbuf,"role=root reason=identity")); }
    reset();unc_status=PCI_ERR_UNC_UNSUP;state();assert(header_reads==8 && strstr(logbuf,"sticky evidence"));reset();unc_status=0xffffffff;state();assert(!header_reads);
    for(int off=0;off<0x1100;off++) { reset();endpoint.aer_cap=root.aer_cap=off;unc_status=1;state();assert(header_reads==((off>=0x100 && off<=0xfc0 && !(off&3))?8:0)); }
    reset();state();int reads=config_reads;
    for(int fail=1;fail<=reads;fail++) { reset();fail_at=fail;state();assert(!maps_live && map_calls==unmaps && strstr(logbuf,"ret=134"));if(fail<=3)assert(!mmio_reads && !map_calls); }
    for(int f=0;f<12;f++) { reset();switch(f) { case 0:admitted=false;break;case 1:no_irq=true;break;case 2:no_desc=true;break;case 3:endpoint.msix_enabled=false;break;case 4:endpoint.msi_enabled=true;break;case 5:desc.dev=&wrongdev;break;case 6:desc.pci.msi_attrib.is_virtual=true;break;case 7:desc.pci.msi_attrib.is_msix=false;break;case 8:desc.pci.mask_base=NULL;break;case 9:desc.msi_index=33;break;case 10:endpoint.msix_cap=0;break;case 11:control&=~PCI_MSIX_FLAGS_ENABLE;break; }state();assert(!mmio_reads && !map_calls); }
    reset();x1_nvme_irq_state(&endpoint,2,2,"timeout");x1_nvme_irq_state(&endpoint,0,1,"timeout");assert(!config_reads);
    for(int f=0;f<4;f++) { reset();switch(f) { case 0:wrong_controller=true;break;case 1:missing_path=true;break;case 2:null_route=true;break;case 3:translated_id=0x90100;break; }x1_nvme_irq_snapshot(&endpoint,1,1,0);assert(!controller.refs && !decoy.refs && strstr(logbuf,"route=0")); }
    reset();x1_nvme_irq_timeout(&queue,1);int first_reads=config_reads;assert(lock_calls==1 && unlock_calls==1 && !nvdev.shutdown_lock.held && strstr(logbuf,"pending_hint=1"));x1_nvme_irq_timeout(&queue,1);assert(config_reads==first_reads && lock_calls==1 && queue.cq_head==0 && queue.cq_phase==1 && cq[0].status==1);
    reset();queue.qid=queue.cq_vector=desc.msi_index=0;cached.data=0;entries[0]=cached.address_lo;x1_nvme_irq_timeout(&queue,1);assert(strstr(logbuf,"TIMEOUT qid=0") && map_calls==1);
    for(int f=0;f<9;f++) { reset();switch(f) { case 0:nvdev.shutdown_lock.busy=true;break;case 1:queue.flags=0;break;case 2:queue.flags|=2;break;case 3:queue.cqes=NULL;break;case 4:queue.cq_vector=0;break;case 5:nvdev.bar_mapped_size=0;break;case 6:queue.cq_head=2;break;case 7:queue.qid=2;break;case 8:admitted=false;break; }x1_nvme_irq_timeout(&queue,1);assert(!config_reads && !mmio_reads && !nvdev.shutdown_lock.held && unlock_calls==((f>0 && f<7)?1:0));x1_nvme_irq_timeout(&queue,1);assert(lock_calls==(f<7?1:0)); }
    reset();hang_at=1;if(!setjmp(interrupted)) { state();assert(0); }assert(mmio_reads==1 && strstr(logbuf,"ENTRY BEGIN") && !strstr(logbuf,"ENTRY END"));
    reset();hang_at=5;if(!setjmp(interrupted)) { state();assert(0); }assert(strstr(logbuf,"PBA BEGIN") && !strstr(logbuf,"PBA END"));
    reset();admitted=false;x1_nvme_irq_enter(&endpoint,0);assert(!atomic_read(&x1_irq_entries[0]));admitted=true;x1_nvme_irq_enter(&endpoint,2);assert(!atomic_read(&x1_irq_entries[1]));x1_nvme_irq_enter(&endpoint,1);assert(atomic_read(&x1_irq_entries[1])==1);
    puts("PASS v33 late evidence: setup/timeout masks, cache mismatch, PBA 0/1/all-ones, BAR bounds, map failures/cleanup, AER offsets/error/header evidence, exact root guard.");
    puts("PASS v33 timeout lifetime: 1 snapshot per queue, trylock/teardown/refused queue/bounds, no CQ mutation, no synthetic IRQs or writes; interrupted MMIO marked.");
    puts("PASS OF path regression: valid local name accepted, wrong/missing/NULL refused, balanced references.");
    puts("PASS MSI-X production policy: no MSI/INTx fallback; <=2 vectors; config-off/admission/descriptor guards; original request result and argument evaluation preserved.");
    return 0;
}
