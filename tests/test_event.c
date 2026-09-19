/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static void put_unaligned_be32(u32 x,u8 *p) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
static u32 crc32c(u32 x,const u8 *p,unsigned n) { while(n--){x^=*p++;for(unsigned i=0;i<8;i++)x=(x>>1)^((x&1)?0x82f63b78U:0);}return x; }
#include "kernel/drivers/thunderbolt/x1-event-packet.h"
static unsigned checks;
#define C(x) do { checks++;assert(x); } while(0)
static void crc(u8 *w) { put_unaligned_be32(~crc32c(~0U,w,12),w+12); }
int main(void) {
    u8 storage[65]={0},*w=storage+1,ack[16],saved[16];struct x1_plug_event e;
    for(unsigned port=0;port<64;port++)for(unsigned unplug=0;unplug<2;unplug++) {
        put_unaligned_be32(0x80000000U,w);put_unaligned_be32(0,w+4);
        put_unaligned_be32(port|(unplug<<31),w+8);crc(w);
        int ret=x1_event_decode(w,16,0,5,&e);
        C((ret==0)==(port==2 || port==3));
        if(ret)continue;
        C(e.port==port && e.unplug==!!unplug);C(x1_event_ack(ack,&e)==0);
        C(!get_unaligned_be32(ack) && !get_unaligned_be32(ack+4));
        C(get_unaligned_be32(ack+8)==((unplug?0xc0000000U:0x80000000U)|(port<<8)|7));
        C(get_unaligned_be32(ack+12)==~crc32c(~0U,ack,12));
        memcpy(saved,w,16);
        for(unsigned byte=0;byte<16;byte++)for(unsigned bit=0;bit<8;bit++) {
            memcpy(w,saved,16);w[byte]^=1U<<bit;C(x1_event_decode(w,16,0,5,&e)<0);
        }
        memcpy(w,saved,16);
        for(unsigned size=0;size<=64;size++)if(size!=16)C(x1_event_decode(w,size,0,5,&e)<0);
        for(unsigned sof=0;sof<16;sof++)for(unsigned eof=0;eof<16;eof++)
            C((x1_event_decode(w,16,sof,eof,&e)==0)==(!sof && eof==5));
        for(unsigned bit=6;bit<31;bit++) {
            memcpy(w,saved,16);put_unaligned_be32(get_unaligned_be32(w+8)|(1U<<bit),w+8);crc(w);
            C(x1_event_decode(w,16,0,5,&e)==-EPROTO);
        }
        for(unsigned word=0;word<2;word++)for(unsigned bit=0;bit<32;bit++) {
            memcpy(w,saved,16);put_unaligned_be32(get_unaligned_be32(w+4*word)^(1U<<bit),w+4*word);crc(w);
            C(x1_event_decode(w,16,0,5,&e)==-EPROTO);
        }
    }
    C(x1_event_decode(NULL,16,0,5,&e)==-EINVAL);C(x1_event_decode(w,16,0,5,NULL)==-EINVAL);
    C(x1_event_ack(NULL,&e)==-EINVAL);C(x1_event_ack(w,NULL)==-EINVAL);
    for(unsigned port=0;port<256;port++){e.port=port;C((x1_event_ack(w,&e)==0)==(port==2 || port==3));}
    printf("EVENT packet PASS: %u checks; photo header with synthetic CRC, both ports/directions, all bit corruptions and field bounds.\n",checks);
}
