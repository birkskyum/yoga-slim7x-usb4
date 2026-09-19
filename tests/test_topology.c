/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static void put_unaligned_be32(u32 x,u8 *p) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
static u32 crc32c(u32 x,const u8 *p,unsigned n) { while(n--) { x^=*p++;for(unsigned i=0;i<8;i++) x=(x>>1)^((x&1)?0x82f63b78U:0); } return x; }
#include "kernel/drivers/thunderbolt/x1-link-packet.h"
#include "kernel/drivers/thunderbolt/x1-topology.h"
static unsigned checks, mode, calls, fault, adapters, reports;
static u32 hdr[]={0x113b059f,0x00114120,2,0x80000000U,0x200010ff};
static u32 host[]={0x093805c6,0x0101c11c,0,0x80000000U,0x200010ff};
#define C(x) do { checks++;assert(x); } while(0)
static int read_mock(void *ctx,u32 route,unsigned port,unsigned space,unsigned off,unsigned words,u32 *out) {
    struct x1_read_spec s={route,port,space,off,words,0}; u8 wire[48]={0};u32 address;
    C(x1_read_address(&s,&address)==0);C(x1_read_request(wire,&s)==0);
    C(route==0 || route==2);C(space==1 || space==2);C(!route || port<=5);
    calls++;if(calls==fault)return -EIO;
    memset(out,0,words*4);
    if(space==2) {
        if(off==5) { C(words==4);return 0; }
        C(words==2);
        out[0]=0x300;
        if(mode==3) { out[0]=0x500;out[1]=0x100; } /* long VSE outside bound */
        if(mode==4) { out[0]=0x7700; } /* unknown router cap */
        if(mode==5 && off!=0x30) { out[0]=0x500;out[1]=0x30; } /* valid long VSE */
        if(mode==6 && off!=0x30) { out[0]=0x02000530; } /* valid short VSE */
        return 0;
    }
    if(!off) {
        C(words==8);out[1]=8;out[2]=port<=2?1:(route?0x100102:0x100101);
        if(!route && port==1)out[2]=2;
        if(mode==7)out[2]=0; /* inactive adapter: never walk */
        return 0;
    }
    C(adapters==(route?12:7)); /* All headers of this router precede its capabilities. */
    C(words==1);
    if(mode==11) { out[0]=0x500 | (off<23?off+1:0);return 0; }
    if(off==9) { out[0]=0x4814001c;return 0; }
    if(mode==1)out[0]=0x0508; /* immediate cycle */
    else if(mode==2)out[0]=0x0500 | (off+1); /* depth overflow */
    else if(mode==8)out[0]=0x0507; /* pointer into header */
    else if(mode==9)out[0]=0x05ff; /* out of window */
    else if(mode==10)out[0]=0x05fe; /* repeat last valid slot */
    else out[0]=(port<=2?1:4)<<8;
    return 0;
}
static void report_mock(void *ctx,const char *kind,u32 route,unsigned port,unsigned off,const u32 *data,unsigned words) {
    reports++;C(words>0 && words<=8);
    if(!strcmp(kind,"ADAPTER"))adapters++;
    if(!strcmp(kind,"PHY"))C(data[1]==0x4814001c);
}
static struct x1_topology fresh(void) {
    calls=adapters=reports=fault=mode=0;
    return (struct x1_topology){.read=read_mock,.report=report_mock};
}
static int run(struct x1_topology *t,const u32 *h) {
    int ret=x1_topology_router(t,0,host);
    return ret?ret:x1_topology_router(t,2,h);
}
int main(void) {
    struct x1_topology t=fresh();u32 h[5],out[8];
    C(run(&t,hdr)==0);unsigned total=calls;
    C(t.adapters==12 && t.pcie_up==3 && t.pcie_down==5 && t.lanes==3);
    for(unsigned i=1;i<=total;i++) { t=fresh();fault=i;C(run(&t,hdr)==-EIO);C(calls==i && t.reads==i); }
    for(unsigned field=0;field<5;field++)for(unsigned bit=0;bit<32;bit++) {
        t=fresh();memcpy(h,hdr,sizeof h);h[field]^=1U<<bit;
        C(x1_topology_router(&t,2,h)<0);C(!calls);
        memcpy(h,host,sizeof h);h[field]^=1U<<bit;
        C(x1_topology_router(&t,0,h)<0);C(!calls);
    }
    for(unsigned m=1;m<=10;m++) {
        t=fresh();mode=m;int ret=run(&t,hdr);
        C((ret==0)==(m==3 || m==4 || m==5 || m==6 || m==7 || m==9));C(calls<=128);
        if(m==3 || m==4 || m==9)C(t.skipped>0);
    }
    t=fresh();t.reads=128;C(run(&t,hdr)==-ENOSPC);C(!calls);
    t=fresh();mode=11;C(run(&t,hdr)==-ENOSPC);C(calls==128 && t.reads==128);
    t=fresh();C(x1_topo_read(&t,3,0,2,0,5,out)==-EINVAL);C(!calls);
    C(x1_topology_router(NULL,2,hdr)==-EINVAL);t=fresh();t.read=NULL;C(x1_topology_router(&t,2,hdr)==-EINVAL);
    t=fresh();t.report=NULL;C(x1_topology_router(&t,2,hdr)==-EINVAL);
    t=fresh();C(x1_topology_router(&t,2,NULL)==-EINVAL);
    t=fresh();C(x1_topology_router(&t,1,hdr)==-EPERM && !calls);
    /* Exercise the production reply path for new external PORT reads. */
    for(unsigned port=1;port<=5;port++)for(unsigned words=1;words<=8;words++) {
        u8 wire[48]={0};struct x1_read_spec s={2,port,1,8,words,3};
        C(x1_read_request(wire,&s)==0);put_unaligned_be32(0x80000000,wire);
        put_unaligned_be32(~crc32c(~0U,wire,12+4*words),wire+12+4*words);
        C(x1_read_reply(wire,16+4*words,0,1,&s,out)==0);
        wire[4]^=1;C(x1_read_reply(wire,16+4*words,0,1,&s,out)==-EBADMSG);
    }
    printf("TOPO exact mapper PASS: %u assertions; all %u read fault positions, identity, bounds/cycles, VSE forms and external replies. Mock only.\n",checks,total);
}
