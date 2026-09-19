/* Exact production v38 backend. No write/map/command/IRQ-injection providers. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define CONFIG_USB4_X1_NATIVE 1
#define IS_ENABLED(c) (c)
#define BIT(n) (1U<<(n))
#define BIT_ULL(n) (1ULL<<(n))
#define EXPORT_SYMBOL_GPL(s)
struct device { int unused; };
struct pci_dev { struct device dev; bool msi_enabled,msix_enabled; };
struct irq_chip { int unused; };
struct device_node { int refs; };
struct msi_msg { u32 address_lo,address_hi,data; };
struct msi_desc { struct device *dev; unsigned int msi_index;
    struct { struct { bool is_msix,is_virtual; } msi_attrib; } pci; };
struct its_node { void *base,*fwnode_handle; u64 phys_base,typer; };
struct its_device { struct its_node *its; u32 device_id,nr_ites; };
struct irq_data { struct irq_data *parent_data; struct irq_chip *chip;
    struct its_device *idev; unsigned long hwirq; bool forwarded; };
static struct irq_chip its_irq_chip,other_chip;
static struct pci_dev endpoint,foreign;
static struct device_node node;
static struct irq_data irqs[5];
static struct msi_desc desc;
static struct its_node controller;
static struct its_device idev;
static struct msi_msg cached;
static unsigned char base[0x80];
static bool admitted,no_irq,no_desc,no_node;
static unsigned int event_id,reads,puts_count,hang_at;
static u32 statuses[2];
static u64 record_value;
static jmp_buf interrupted;
static char logbuf[32768];
static size_t loglen;
static void record(struct device *d,const char *fmt,...)
{
    assert(d==&endpoint.dev); va_list ap;va_start(ap,fmt);
    int n=vsnprintf(logbuf+loglen,sizeof(logbuf)-loglen,fmt,ap);
    va_end(ap); assert(n>=0 && (size_t)n<sizeof(logbuf)-loglen);loglen+=n;
}
#define dev_emerg record
static bool x1_native_nvme_allowed(struct pci_dev *p) { return admitted && p==&endpoint; }
static struct irq_data *irq_get_irq_data(unsigned int irq) { assert(irq==144+event_id);return no_irq?NULL:&irqs[0]; }
static struct msi_desc *irq_data_get_msi_desc(struct irq_data *d) { assert(d==&irqs[0]);return no_desc?NULL:&desc; }
static struct irq_chip *irq_data_get_irq_chip(struct irq_data *d) { return d->chip; }
static struct its_device *irq_data_get_irq_chip_data(struct irq_data *d) { return d->idev; }
static bool irqd_is_forwarded_to_vcpu(struct irq_data *d) { return d->forwarded; }
static unsigned int its_get_event_id(struct irq_data *d) { assert(d->idev==&idev);return event_id; }
static struct device_node *of_find_node_by_path(const char *p)
{ assert(!strcmp(p,"/soc@0/interrupt-controller@17000000/msi-controller@17040000"));if(no_node)return NULL;node.refs++;return &node; }
static void *of_fwnode_handle(struct device_node *n) { assert(n==&node);return &node; }
static void of_node_put(struct device_node *n) { if(!n)return;assert(n==&node && node.refs==1);node.refs--;puts_count++; }
static void get_cached_msi_msg(unsigned int irq,struct msi_msg *m) { assert(irq==144+event_id);*m=cached; }
static bool test_and_set_bit(unsigned int b,unsigned long *p) { assert(b<4);bool v=!!(*p&(1UL<<b));*p|=1UL<<b;return v; }
static u32 readl_relaxed(void *p)
{
    assert(p==base+0x40 && (controller.typer&BIT_ULL(44)));
    assert(reads==0 || reads==2);
    assert(strstr(logbuf,reads?"RECHECK BEGIN":"STATUS BEGIN"));
    reads++;if(reads==hang_at)longjmp(interrupted,1);
    return statuses[reads==1?0:1];
}
static u64 readq_relaxed(void *p)
{
    assert(p==base+0x48 && reads==1 && (statuses[0]&BIT(4)));
    assert(strstr(logbuf,"UMSIR BEGIN"));reads++;
    if(reads==hang_at)longjmp(interrupted,1);
    return record_value;
}
#include "x1_address_mock.h"
#include "kernel/drivers/irqchip/irq-gic-v3-its-x1-audit.h"
static void reset(unsigned int event)
{
    address_reset();
    assert(!node.refs);event_id=event;puts_count=0;
    endpoint=(struct pci_dev){.msix_enabled=true};admitted=true;
    no_irq=no_desc=no_node=false;hang_at=reads=0;
    memset(irqs,0,sizeof(irqs));irqs[0].chip=&other_chip;irqs[0].parent_data=&irqs[1];
    irqs[1]=(struct irq_data){.chip=&its_irq_chip,.idev=&idev,.hwirq=8224+event};
    idev=(struct its_device){.its=&controller,.device_id=0x80100,.nr_ites=2};
    controller=(struct its_node){.base=base,.fwnode_handle=&node,.phys_base=0x17040000,.typer=BIT_ULL(44)};
    desc=(struct msi_desc){.dev=&endpoint.dev,.msi_index=event,.pci={.msi_attrib={.is_msix=true}}};
    cached=(struct msi_msg){.address_lo=0x17050040,.data=event};
    statuses[0]=statuses[1]=BIT(4)|(3<<6);
    record_value=((u64)0x100<<32)|event;
    x1_its_audit_seen=0;loglen=0;logbuf[0]=0;
}
static void audit(bool timeout) { x1_its_msi_audit(&endpoint,144+event_id,timeout);assert(!node.refs); }
int main(void)
{
    reset(1);mock_el2=mock_domain_present=true;cached.address_lo=mock_iova;
    audit(false);assert(reads==3 && mock_translations==2);
    reset(1);mock_el2=mock_domain_present=true;mock_iova=0x112345040ULL;
    cached.address_lo=(u32)mock_iova;cached.address_hi=mock_iova>>32;
    audit(false);assert(reads==3 && mock_translations==2);
    reset(1);mock_el2=true;audit(false);assert(!reads && !mock_translations);
    reset(1);mock_el2=mock_domain_present=true;mock_physical=0;
    cached.address_lo=mock_iova;audit(false);assert(!reads);
    reset(1);mock_el2=mock_domain_present=true;mock_tail_missing=true;
    cached.address_lo=mock_iova;audit(false);assert(!reads);
    reset(0);controller.typer=0;audit(false);assert(!reads && strstr(logbuf,"unmapped-report-unsupported"));
    reset(0);controller.typer=BIT_ULL(45);audit(false);assert(!reads && strstr(logbuf,"umsi_cap=0"));
    reset(0);statuses[0]=0;audit(false);assert(reads==1 && strstr(logbuf,"latched=0"));
    reset(0);statuses[0]=~0U;audit(false);assert(reads==1 && strstr(logbuf,"invalid-status") && !strstr(logbuf,"UMSIR BEGIN"));
    for(unsigned int event=0;event<2;event++)for(unsigned int syndrome=0;syndrome<16;syndrome++) {
        reset(event);statuses[0]=statuses[1]=BIT(4)|BIT(5)|(syndrome<<6);audit(false);
        assert(reads==3 && strstr(logbuf,"stable=1 latched=1 overflow=1") && strstr(logbuf,"observed_devid=00000100"));
        char expected[32];snprintf(expected,sizeof(expected),"syndrome=%u ",syndrome);assert(strstr(logbuf,expected));
        audit(false);assert(reads==3); /* q0 reallocation cannot repeat snapshot */
        reads=0;audit(true);assert(reads==3);audit(true);assert(reads==3);
    }
    for(unsigned int changed=0;changed<3;changed++) {
        reset(1);statuses[1]=changed==0?0:changed==1?~0U:BIT(4)|(4<<6);audit(true);
        assert(reads==3 && strstr(logbuf,"stable=0"));
    }
    reset(0);x1_its_msi_audit(NULL,144,false);x1_its_msi_audit(&foreign,144,false);assert(!reads && !loglen);
    for(unsigned int f=0;f<24;f++) {
        reset(0);
        switch(f) {
        case 0:admitted=false;break;case 1:endpoint.msi_enabled=true;break;case 2:endpoint.msix_enabled=false;break;
        case 3:no_irq=true;break;case 4:no_desc=true;break;case 5:desc.dev=&foreign.dev;break;
        case 6:desc.pci.msi_attrib.is_msix=false;break;case 7:desc.pci.msi_attrib.is_virtual=true;break;
        case 8:desc.msi_index=2;break;case 9:irqs[0].parent_data=NULL;break;case 10:irqs[1].forwarded=true;break;
        case 11:irqs[1].idev=NULL;break;case 12:idev.its=NULL;break;case 13:controller.base=NULL;break;
        case 14:controller.phys_base++;break;case 15:idev.device_id++;break;case 16:event_id=2;break;
        case 17:desc.msi_index=1;break;case 18:idev.nr_ites=0;break;case 19:no_node=true;break;
        case 20:controller.fwnode_handle=&foreign;break;case 21:cached.address_hi=1;break;
        case 22:cached.address_lo++;break;case 23:cached.data=1;break;
        }
        audit(false);assert(!reads && !loglen && !x1_its_audit_seen);
    }
    for(unsigned int depth=0;depth<5;depth++) {
        reset(0);memset(irqs,0,sizeof(irqs));
        for(unsigned int i=0;i<depth;i++) {irqs[i].chip=&other_chip;irqs[i].parent_data=&irqs[i+1];}
        irqs[depth]=(struct irq_data){.chip=&its_irq_chip,.idev=&idev,.hwirq=8224};
        audit(false);assert(reads==(depth<4?3U:0U));
    }
    /* Interrupted reads cannot be retried in this boot and each has BEGIN. */
    for(unsigned int op=1;op<=3;op++) {
        reset(0);hang_at=op;
        if(!setjmp(interrupted)) {audit(false);assert(0);}
        assert(reads==op && x1_its_audit_seen==1 && !node.refs);
        audit(false);assert(reads==op);
    }
    /* All four event/stage slots distinct, unsupported hardware never touched. */
    reset(0);controller.typer=0;
    for(unsigned int event=0;event<2;event++) {
        event_id=desc.msi_index=cached.data=event;audit(false);audit(true);
    }
    assert(x1_its_audit_seen==15 && reads==0);
    puts("PASS v38 ITS: exact production header; capability gate, 24 rejection guards, 5 domain depths, 32 syndrome cases, sticky/invalid/racing reports, bounded reads and no retries; no MMIO writes or synthetic interrupts.");
}
