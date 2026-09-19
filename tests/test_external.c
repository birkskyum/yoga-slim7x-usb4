/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static void put_unaligned_be32(u32 x,u8 *p) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
static u32 crc32c(u32 x,const u8 *p,unsigned n) { while(n--){x^=*p++;for(unsigned i=0;i<8;i++)x=(x>>1)^((x&1)?0x82f63b78U:0);}return x; }
#include "kernel/drivers/thunderbolt/x1-enum-packet.h"
static unsigned checks;
#define C(x) do { checks++;assert(x); } while(0)
static void crc(u8 *w,unsigned n) { put_unaligned_be32(~crc32c(~0U,w,n-4),w+n-4); }
int main(void) {
    u32 hdr[5]={0x113b059f,0x00014120,0,0,0x2000000a},plan[4],changed[5],a;
    u32 root[5]={0x093805c6,0x0101c11c,0,0,0x2000000a};
    C(x1_root_plan(root,plan)==0);
    C(x1_external_plan(hdr,plan)==0);
    C(plan[0]==0x00114120 && plan[1]==2 && plan[2]==0x80000000U && plan[3]==0x200010ff);
    for(unsigned i=0;i<5;i++)for(unsigned bit=0;bit<32;bit++) {
        memcpy(changed,hdr,sizeof hdr);changed[i]^=1U<<bit;C(x1_external_plan(changed,plan)==-EPERM);
    }
    C(x1_external_plan(NULL,plan)<0);C(x1_external_plan(hdr,NULL)<0);
    C(x1_external_setup_plan(0,NULL)<0);
    for(unsigned bit=0;bit<32;bit++) {
        u32 before=1U<<bit,after;
        int ret=x1_external_setup_plan(before,&after);
        C((ret==0)==!(before&0x8700000fU));
        if(!ret)C(after==(before&~0x00800000U));
    }
    for(unsigned mode=0;mode<2;mode++)for(unsigned seq=0;seq<4;seq++) {
        u32 before[4]={0x00014120,0,0,0x2000000a},after[4]={0x00114120,2,0x80000000U,0x200010ff};
        struct x1_write_spec s={0,2,1,4,seq,before,after,2},t;
        if(mode) { s.offset=5;s.words=1;before[0]=0x00800000;after[0]=0; }
        u8 storage[65]={0},*wire=storage+1,saved[16];
        C(x1_write_request(wire,&s)==0);C(x1_write_address(&s,&a)==0);
        C(get_unaligned_be32(wire)==0 && get_unaligned_be32(wire+4)==2 && get_unaligned_be32(wire+8)==a);
        for(unsigned i=0;i<s.words;i++)C(get_unaligned_be32(wire+12+4*i)==after[i]);
        C(get_unaligned_be32(wire+12+4*s.words)==~crc32c(~0U,wire,12+4*s.words));
        for(unsigned route=0;route<16;route++)for(unsigned port=0;port<9;port++)
        for(unsigned space=0;space<4;space++)for(unsigned off=0;off<32;off++)for(unsigned words=0;words<9;words++) {
            t=s;t.route=route;t.port=port;t.space=space;t.offset=off;t.words=words;
            C((x1_write_address(&t,&a)==0)==(route==2 && !port && space==2 && off==s.offset && words==s.words));
        }
        for(unsigned i=0;i<s.words;i++)for(unsigned bit=0;bit<32;bit++) {
            after[i]^=1U<<bit;C(x1_write_address(&s,&a)<0);after[i]^=1U<<bit;
            if(!mode) { before[i]^=1U<<bit;C(x1_write_address(&s,&a)<0);before[i]^=1U<<bit; }
        }
        t=s;t.seq=4;C(x1_write_request(wire,&t)<0);
        C(x1_write_address(&s,&a)==0);
        put_unaligned_be32(0x80000000U,wire);put_unaligned_be32(2,wire+4);put_unaligned_be32(a|(1U<<19),wire+8);crc(wire,16);
        C(x1_write_reply(wire,16,0,2,&s)==0);memcpy(saved,wire,16);
        for(unsigned i=0;i<16;i++)for(unsigned bit=0;bit<8;bit++) {
            memcpy(wire,saved,16);wire[i]^=1U<<bit;C(x1_write_reply(wire,16,0,2,&s)<0);
        }
        for(unsigned route=0;route<16;route++) {
            memcpy(wire,saved,16);put_unaligned_be32(route,wire+4);crc(wire,16);
            C((x1_write_reply(wire,16,0,2,&s)==0)==(route==2));
        }
        memcpy(wire,saved,16);t=s;t.seq=(seq+1)%4;C(x1_write_reply(wire,16,0,2,&t)<0);
        for(unsigned n=0;n<65;n++)if(n!=16)C(x1_write_reply(wire,n,0,2,&s)<0);
        C(x1_write_reply(wire,16,1,2,&s)<0);C(x1_write_reply(wire,16,0,1,&s)<0);
    }
    printf("EXTERNAL exact plan/WRITE whitelist PASS: %u assertions; route/port/space/offset/length, bit mutations, CRC and replies. Mock only.\n",checks);
}
