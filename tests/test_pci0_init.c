/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact production sequence; simulated registers, NOT hardware evidence. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#include "kernel/drivers/thunderbolt/x1-pcie-init.h"
static u32 root[0x1300/4],parf[0x8000/4],gate,request;
static unsigned writes,reads,delays,resets,fail_write,fail_reset,stuck;
static bool transition,training;
static bool trace, trace_reads, mailbox_permitted;
static u32 *reg(unsigned r,u32 o) {
 assert(!(o&3));
 if(r==X1_ROOT){assert(o<sizeof(root));return &root[o/4];}
 if(r==X1_PARF){assert(o<sizeof(parf));return &parf[o/4];}
 assert(r==X1_ROUTER&&(o==0x82000||o==0xe008));
 return o==0x82000?&gate:&request;
}
static u32 rd(void *c,unsigned r,u32 o){
 if(trace_reads)printf("Q %u %x\n",r,o);
 /* A mock access fault before the caller enters the training phase. */
 if(r==X1_ROUTER&&o==0xe008)assert(mailbox_permitted);
 reads++;return *reg(r,o);
}
static void wr(void *c,unsigned r,u32 o,u32 v){
 if(trace)printf("W %u %x %x\n",r,o,v);
 writes++;if(writes==fail_write)return;
 *reg(r,o)=v;
 if(r==X1_ROUTER&&o==0x82000&&!v&&transition&&parf[0x1000/4]==4&&resets==2)
  root[3]=0x00010000;
 if(r==X1_ROUTER&&o==0xe008&&!stuck)request=v&~3U;
 if(r==X1_PARF&&o==0x1b0&&(v&0x100)&&training){
  root[0xf48/4]=0x400;parf[o/4]=v|0x11;
 }
}
static int rst(void *c,bool a){if(trace)printf("R %u\n",a);resets++;assert(a==(resets==1));return resets==fail_reset?-EIO:0;}
static void delay(void *c,unsigned ms){if(trace)printf("D %u\n",ms);assert(ms<=20);delays+=ms;}
static struct x1_pci0_io io={0,rd,wr,rst,delay};
static void reset(void){
 memset(root,0,sizeof(root));memset(parf,0,sizeof(parf));gate=request=0;
 writes=reads=delays=resets=fail_write=fail_reset=stuck=0;transition=training=true;
 mailbox_permitted=false;
 root[0]=0x011117cb;root[1]=0x00100000;root[2]=0xff000001;
}
/* Isolated train failures begin with the caller's preconditions satisfied. */
static int train_fixture(void){mailbox_permitted=true;return x1_pci0_train(&io);}
int main(int argc,char **argv){
 trace_reads=argc==2&&!strcmp(argv[1],"--trace-reads");
 bool photo=argc==2&&!strcmp(argv[1],"--trace-mailbox30");
 trace=trace_reads||photo||(argc==2&&!strcmp(argv[1],"--trace"));
 if(trace_reads)puts("P mode");
 unsigned n=0;reset();request=photo?0x30:0;assert(!x1_pci0_mode(&io));
 assert(root[3]==0x10000&&resets==2&&delays==15&&!gate);n++;
 if(trace_reads)puts("P setup");
 assert(!x1_pci0_setup(&io));assert(root[0]==0x011117cb&&root[2]==0x06040001);
 assert(!(root[0x8bc/4]&1)&&!(root[0xfe4/4]&1));n++;
 if(trace_reads)puts("P train");
 assert(!train_fixture());assert(request==(photo?0x30:0)&&(parf[0x1b0/4]&0x13f)==0x111);n++;
 if(trace_reads)puts("P windows");
 assert(!x1_pci0_windows(&io));assert(root[0x1004/4]==0x90000000&&root[0x1204/4]==0x90000000);
 assert(parf[0x368/4]==0xfffff&&parf[0x390/4]==0x8fefff&&parf[0]==0x44000000);n++;
 if(trace)return 0;
 /* Each photo identity word is independently mandatory before writes. */
 for(unsigned i=0;i<4;i++){reset();root[i]^=1;assert(x1_pci0_mode(&io)==-ENODEV&&!writes&&!resets);n++;}
 reset();gate=2;assert(x1_pci0_mode(&io)==-EBUSY&&!writes&&!resets);n++;
 /* Mailbox values cannot be inspected early; busy checks remain at training.
  * This also catches an accidental early read introduced by a new guard. */
 const u32 busy[]={1,2,3,0x31,0x32,0x33,0xffffffff};
 for(unsigned i=0;i<ARRAY_SIZE(busy);i++){
  reset();request=busy[i];assert(!x1_pci0_mode(&io));assert(!x1_pci0_setup(&io));
  unsigned before=writes;
  assert(train_fixture()==-EBUSY&&writes==before&&request==busy[i]);n++;
 }
 /* Exact v26 photo plus other non-request bits survive both requests. */
 const u32 idle[]={0x30,4,0xfffffffc,0x80000000};
 for(unsigned i=0;i<ARRAY_SIZE(idle);i++){
  reset();request=idle[i];mailbox_permitted=true;
  assert(!x1_pci0_reset_request(&io,1)&&request==idle[i]&&writes==1);n++;
  assert(!x1_pci0_reset_request(&io,2)&&request==idle[i]&&writes==2);n++;
 }
 const u32 invalid[]={0,3,4,0xffffffff};
 for(unsigned i=0;i<ARRAY_SIZE(invalid);i++){
  reset();request=0x30;mailbox_permitted=true;
  assert(x1_pci0_reset_request(&io,invalid[i])==-EBUSY&&!writes&&request==0x30);n++;
 }
 /* Nonzero non-request bits do not hide a request that fails to clear. */
 for(u32 bit=1;bit<=2;bit++){
  reset();request=0x30;stuck=1;mailbox_permitted=true;
  assert(x1_pci0_reset_request(&io,bit)==-ETIMEDOUT&&delays==10&&writes==1);
  assert(request==(0x30|bit));n++;
 }
 reset();transition=false;assert(x1_pci0_mode(&io)==-ENODEV);n++;
 for(unsigned i=1;i<=2;i++){reset();fail_reset=i;assert(x1_pci0_mode(&io)==-EIO&&resets==i);n++;}
 for(unsigned i=1;i<=4;i++){reset();fail_write=i;parf[0x1b0/4]=0x100;
  assert(x1_pci0_mode(&io)<0);n++;}
 /* No self-clearing reset acknowledgement => bounded refusal. */
 reset();stuck=1;assert(train_fixture()==-ETIMEDOUT&&delays==10&&writes==1);n++;
 reset();training=false;assert(train_fixture()==-ETIMEDOUT&&delays==171);n++;
 reset();root[0xf48/4]=0x400;training=false;
 assert(train_fixture()==-ETIMEDOUT&&delays==171);n++;
 reset();root[0xf48/4]=0xffffffff;training=false;assert(train_fixture()==-EIO);n++;
 reset();request=2;assert(train_fixture()==-EBUSY&&!writes);n++;
 reset();fail_write=7;assert(x1_pci0_windows(&io)==-EIO);n++;
 reset();fail_write=12;assert(x1_pci0_windows(&io)==-EIO);n++;
 /* Protected class readback failure still closes the DBI gate. */
 reset();fail_write=14;assert(x1_pci0_setup(&io)==-EIO&&!(root[0x8bc/4]&1));n++;
 printf("PASS PCI0 init: %u simulated identity/reset/mode/setup/window/training cases; no hardware executed.\n",n);
}
