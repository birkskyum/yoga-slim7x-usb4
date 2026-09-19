/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static void put_unaligned_be32(u32 x,u8 *p) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
static u32 crc32c(u32 x,const u8 *p,unsigned n) { while(n--) { x^=*p++;for(unsigned i=0;i<8;i++) x=(x>>1)^((x&1)?0x82f63b78U:0); } return x; }
#include "kernel/drivers/thunderbolt/x1-link-packet.h"
struct yoga_pan { u8 port,orientation_raw,mux_raw;u16 vid,svid_header,svid_payload;u8 ext[8]; };
#include "kernel/drivers/soc/qcom/yoga_pan_plan.h"
#include "kernel/drivers/soc/qcom/yoga_link_plan.h"
static unsigned checks;
#define C(x) do { checks++;assert(x); } while(0)
static void crc(u8 *w,unsigned n) { put_unaligned_be32(~crc32c(~0U,w,n-4),w+n-4); }
int main(void) {
    u8 storage[65]={0},*w=storage+1,saved[64];u32 out[8],before[8],address;
    struct x1_read_spec s={0,0,2,0,5,0},t;
    C(~crc32c(~0U,(const u8 *)"123456789",9)==0xe3069283);
    for(unsigned route=0;route<=2;route+=2)for(unsigned seq=0;seq<4;seq++) {
        s.route=route;s.seq=seq;C(x1_read_request(w,&s)==0);
        C(get_unaligned_be32(w+4)==route);
        C(get_unaligned_be32(w+8)==((5U<<13)|(2U<<25)|(seq<<27)));
        put_unaligned_be32(0x80000000U,w);
        put_unaligned_be32(get_unaligned_be32(w+8)|(7U<<19),w+8);
        for(unsigned i=0;i<5;i++)put_unaligned_be32(0xabcdef00+i,w+12+4*i);
        crc(w,36);memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));
        C(x1_read_reply(w,36,0,1,&s,out)==0);C(out[4]==0xabcdef04);
        memcpy(saved,w,36);
        for(unsigned byte=0;byte<36;byte++)for(unsigned bit=0;bit<8;bit++) {
            memcpy(w,saved,36);w[byte]^=1U<<bit;memcpy(out,before,sizeof(out));
            C(x1_read_reply(w,36,0,1,&s,out)<0);C(!memcmp(out,before,sizeof(out)));
        }
        memcpy(w,saved,36);
        for(unsigned n=0;n<65;n++) if(n!=36) C(x1_read_reply(w,n,0,1,&s,out)<0);
        C(x1_read_reply(w,36,1,1,&s,out)<0);C(x1_read_reply(w,36,0,2,&s,out)<0);
        t=s;t.route=(route+1)%8;C(x1_read_reply(w,36,0,1,&t,out)<0);
        t=s;t.seq=(seq+1)%4;C(x1_read_reply(w,36,0,1,&t,out)<0);
    }
    s=(struct x1_read_spec){0,7,1,254,2,3};C(x1_read_request(w,&s)==0);
    put_unaligned_be32(0x80000000U,w);put_unaligned_be32(0x11,w+12);put_unaligned_be32(0x22,w+16);crc(w,24);
    C(x1_read_reply(w,24,0,1,&s,out)==0 && out[1]==0x22);
    for(unsigned f=0;f<6;f++) { t=s;switch(f) {
        case 0:t.route=8;break;case 1:t.port=8;break;case 2:t.space=0;break;
        case 3:t.offset=255;break;case 4:t.words=9;break;case 5:t.seq=4;break;}
        C(x1_read_address(&t,&address)<0);
    }
    t=s;t.route=1;C(x1_read_address(&t,&address)<0);t=s;t.words=0;C(x1_read_address(&t,&address)<0);
    for(unsigned route=0;route<16;route++)for(unsigned port=0;port<9;port++)
    for(unsigned space=0;space<4;space++)for(unsigned off=0;off<258;off++)for(unsigned words=0;words<10;words++) {
        t=(struct x1_read_spec){route,port,space,off,words,0};
        bool allowed=(route==0 || route==2) && port<=7 && words>0 && words<=8 &&
            off<=255 && off+words<=256 &&
            ((space==1 && port>0 && (!route || port<=5)) ||
             (space==2 && !port && ((!off && words==5) || (off==5 && words==4) ||
              (route==2 && (off==5 || off==6) && words==1) || (off>=9 && words<=2))));
        C((x1_read_address(&t,&address)==0)==allowed);
    }
    C(x1_read_address(NULL,&address)<0);C(x1_read_request(NULL,&s)<0);
    memset(w,0,64);
    for(unsigned e=0;e<256;e++) { put_unaligned_be32(e,w+8);crc(w,16);
        C(x1_read_reply(w,16,0,3,&s,out)==-ENOLINK); }
    w[0]^=1;C(x1_read_reply(w,16,0,3,&s,out)==-EBADMSG);
    struct yoga_pan events[32]={0};u32 word=0;
    events[0]=(struct yoga_pan){.port=0,.orientation_raw=0,.mux_raw=4,.vid=0x059f,.svid_header=0xff00,.svid_payload=0xff00,.ext={1}};
    C(yoga_link_word(events,1,1,&word)==0 && word==0x2501);
    events[0].orientation_raw=1;C(yoga_link_word(events,1,1,&word)==0 && word==0x2701);
    C(yoga_link_word(events,1,0,&word)<0);C(yoga_link_word(events,0,0,&word)<0);C(yoga_link_word(events,33,33,&word)<0);
    events[1]=events[0];events[1].mux_raw=0;C(yoga_link_word(events,2,2,&word)<0);
    events[1].port=2;C(yoga_link_word(events,2,2,&word)==0);
    for(unsigned m=0;m<256;m++) { events[0].mux_raw=m;C((yoga_link_word(events,1,1,&word)==0)==(m==4)); }
    events[0].mux_raw=4;
    for(unsigned ext=0;ext<256;ext++) { events[0].ext[0]=ext;C((yoga_link_word(events,1,1,&word)==0)==(ext==1)); }
    events[0].ext[0]=1;events[0].vid=0;C(yoga_link_word(events,1,1,&word)<0);
    printf("LINK production packet/PAN validation PASS: %u checks; no hardware emulation.\n",checks);
}
