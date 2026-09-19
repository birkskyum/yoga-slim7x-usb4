/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
static u32 get_unaligned_le32(const u8 *p) { return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static u16 get_unaligned_le16(const u8 *p) { return p[0]|(u16)p[1]<<8; }
#include "kernel/drivers/soc/qcom/yoga_pan_packet.h"
#include "kernel/drivers/soc/qcom/yoga_pan_plan.h"
#include "kernel/drivers/soc/qcom/yoga_ucsi_packet.h"
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
int main(void) {
    struct yoga_pan e={.svid_header=0xff00,.svid_payload=0xff00};
    struct yoga_port_plan p, before;
    memset(&before,0xa5,sizeof(before));
    for(unsigned o=0;o<256;o++) for(unsigned m=0;m<256;m++) {
        e.orientation_raw=o;e.mux_raw=m;p=before;
        bool supported=(m==0 && o<=2)||((m==1||m==4)&&o<=1);
        int ret=yoga_plan(&e,&p);
        CHECK(supported ? ret==0 : ret<0);
        if(ret) CHECK(!memcmp(&p,&before,sizeof(p)));
        else CHECK(p.connect_word == (m==4 ? (0x501U | o<<9) : 0));
    }
    e.orientation_raw=0;e.mux_raw=4;
    for(unsigned sid=0;sid<2;sid++) {
        e.svid_header=e.svid_payload=sid?0x8087:0xff00;
        for(unsigned ext=0;ext<256;ext++) {
            e.ext[0]=ext;p=before;
            bool supported=!(ext&0x80) && (ext&7)<=1 && ((ext>>3)&7)<=1;
            int ret=yoga_plan(&e,&p);
            CHECK(supported ? ret==0 : ret<0);
            if(ret) CHECK(!memcmp(&p,&before,sizeof(p)));
        }
    }
    e.ext[0]=0;e.svid_header=e.svid_payload=0xff00;
    CHECK(yoga_plan(&e,&p)==0 && p.connect_word==0x501);
    e.orientation_raw=1;e.ext[0]=9;
    CHECK(yoga_plan(&e,&p)==0 && p.connect_word==0x2f01);
    e.svid_header=e.svid_payload=0x8087;
    CHECK(yoga_plan(&e,&p)==0 && p.connect_word==0x3e01);
    e.ext[0]|=0x40;
    CHECK(yoga_plan(&e,&p)==0 && p.connect_word==0x2e01);
    for(unsigned i=1;i<8;i++) {
        e.ext[i]=1;CHECK(yoga_plan(&e,&p)==-EOPNOTSUPP);e.ext[i]=0;
    }
    e.svid_header=0xff00;CHECK(yoga_plan(&e,&p)<0);
    e.port=1;CHECK(yoga_plan(&e,&p)<0);
    CHECK(yoga_plan(NULL,&p)<0);CHECK(yoga_plan(&e,NULL)<0);
    u8 bytes[547]={0},*b=bytes+1;
    b[0]=0x0b;b[1]=0x80;b[4]=1;b[8]=0x11;b[13]=1;
    struct yoga_ucsi_sample sample, old;
    memset(&old,0xa5,sizeof(old));
    for(unsigned len=0;len<546;len++) {
        sample=old;int ret=yoga_ucsi_decode(b,len,&sample);
        CHECK(len==64?ret==0:ret<0);
        if(ret) CHECK(!memcmp(&sample,&old,sizeof(sample)));
    }
    b[13]=2;CHECK(yoga_ucsi_decode(b,544,&sample)==0);
    b[16]=0x42;CHECK(yoga_ucsi_decode(b,544,&sample)==0 && sample.cci==0x42);
    b[543]=1;CHECK(yoga_ucsi_decode(b,544,&sample)==-EREMOTEIO);b[543]=0;
    b[8]=0x12;CHECK(yoga_ucsi_decode(b,544,&sample)==-EBADMSG);
    printf("Policy/parser PASS: %u checks, exact production functions under sanitizers. No hardware.\n",checks);
}
