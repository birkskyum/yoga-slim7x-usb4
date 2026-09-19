/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static void put_unaligned_be32(u32 x,u8 *p) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
static u32 crc32c(u32 x,const u8 *p,unsigned n) { while(n--) { x^=*p++;for(unsigned i=0;i<8;i++)x=(x>>1)^((x&1)?0x82f63b78U:0); }return x; }
#include "kernel/drivers/thunderbolt/x1-enum-packet.h"
static unsigned checks;
#define C(x) do { checks++;assert(x); } while(0)
static void crc(u8 *w,unsigned n) { put_unaligned_be32(~crc32c(~0U,w,n-4),w+n-4); }
int main(void) {
    u8 storage[65]={0},*w=storage+1,saved[64];u32 h[5]={0x093805c6,0x0101c11c,0,0,0x2000000a},out[4],a;
    C(x1_root_plan(h,out)==0 && out[0]==h[1] && !out[1] && out[2]==0x80000000U && out[3]==0x200010ff);
    for(unsigned i=0;i<5;i++)for(unsigned b=0;b<32;b++) { h[i]^=1U<<b;C(x1_root_plan(h,out)<0);h[i]^=1U<<b; }
    C(x1_root_plan(NULL,out)<0);C(x1_root_plan(h,NULL)<0);
    u32 external[5]={0x113b059f,0x00014120,0,0,0x2000000a};
    C(x1_external_plan(external,out)==0 && out[0]==0x00114120 && out[1]==2);
    for(unsigned mode=0;mode<2;mode++)for(unsigned seq=0;seq<4;seq++) {
        u32 before[4]={0x0101c11c,0,0,0x2000000a},after[4]={0x0101c11c,0,0x80000000U,0x200010ff};
        struct x1_write_spec s={0,2,1,4,seq,before,after,0},t;
        if(mode) { s=(struct x1_write_spec){2,1,4,1,seq,before,after,0};before[0]=0x85600000;after[0]=0x05600000; }
        C(x1_write_request(w,&s)==0);C(x1_write_address(&s,&a)==0);
        C(!get_unaligned_be32(w) && !get_unaligned_be32(w+4) && get_unaligned_be32(w+8)==a);
        for(unsigned i=0;i<s.words;i++)C(get_unaligned_be32(w+12+4*i)==after[i]);
        C(get_unaligned_be32(w+12+4*s.words)==~crc32c(~0U,w,12+4*s.words));
        for(unsigned i=0;i<s.words;i++)for(unsigned bit=0;bit<32;bit++) {
            after[i]^=1U<<bit;C(x1_write_request(w,&s)<0);after[i]^=1U<<bit;
            before[i]^=1U<<bit;C(x1_write_request(w,&s)<0);before[i]^=1U<<bit;
        }
        for(unsigned p=0;p<9;p++)for(unsigned sp=0;sp<4;sp++)for(unsigned off=0;off<8;off++)for(unsigned n=0;n<6;n++) {
            t=s;t.port=p;t.space=sp;t.offset=off;t.words=n;
            C((x1_write_address(&t,&a)==0)==(p==s.port && sp==s.space && off==s.offset && n==s.words));
        }
        t=s;t.seq=4;C(x1_write_request(w,&t)<0);
        t=s;t.before=NULL;C(x1_write_request(w,&t)<0);t=s;t.after=NULL;C(x1_write_request(w,&t)<0);
        C(x1_write_address(NULL,&a)<0);C(x1_write_address(&s,NULL)<0);C(x1_write_request(NULL,&s)<0);
        C(x1_write_address(&s,&a)==0);
        put_unaligned_be32(0x80000000U,w);put_unaligned_be32(0,w+4);put_unaligned_be32(a|(7U<<19),w+8);crc(w,16);
        C(x1_write_reply(w,16,0,2,&s)==0);memcpy(saved,w,16);
        for(unsigned i=0;i<16;i++)for(unsigned b=0;b<8;b++) {
            memcpy(w,saved,16);w[i]^=1U<<b;C(x1_write_reply(w,16,0,2,&s)<0);
        }
        memcpy(w,saved,16);
        for(unsigned n=0;n<65;n++)if(n!=16)C(x1_write_reply(w,n,0,2,&s)<0);
        C(x1_write_reply(w,16,1,2,&s)<0);C(x1_write_reply(w,16,0,1,&s)<0);
        t=s;t.seq=(seq+1)%4;C(x1_write_reply(w,16,0,2,&t)<0);
        for(unsigned i=0;i<3;i++) {
            memcpy(w,saved,16);put_unaligned_be32(get_unaligned_be32(w+4*i)^1,w+4*i);crc(w,16);
            C(x1_write_reply(w,16,0,2,&s)<0);
        }
        for(unsigned e=0;e<256;e++) { memset(w,0,16);put_unaligned_be32(e,w+8);crc(w,16);
            C(x1_write_reply(w,16,0,3,&s)==-ENOLINK); }
    }
    printf("ENUM production root-plan / bounded WRITE packet PASS: %u checks; no hardware emulation.\n",checks);
}
