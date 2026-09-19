/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
static uint32_t get_unaligned_le32(const u8 *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t get_unaligned_le16(const u8 *p) { return p[0] | (uint16_t)p[1]<<8; }
#include "kernel/drivers/soc/qcom/yoga_pan_packet.h"
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
int main(void)
{
    u8 storage[65] = {0}, *p = storage + 1; /* Intentionally unaligned */
    struct yoga_pan out, before;
    p[0]=0x0c; p[1]=0x80; p[4]=2; p[8]=0x16;
    memset(&before, 0xa5, sizeof(before));
    for (unsigned n=0; n<=64; n++) {
        out=before;
        int ret=yoga_pan_decode(p,n,&out);
        CHECK(n==32 ? ret==0 : ret==-EINVAL);
        if(n!=32) CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
    CHECK(yoga_pan_decode(NULL,32,&out)==-EINVAL);
    CHECK(yoga_pan_decode(p,32,NULL)==-EINVAL);
    for (unsigned port=0; port<256; port++) {
        p[12]=port; out=before;
        int ret=yoga_pan_decode(p,32,&out);
        CHECK(port<3 ? ret==0 : ret==-EBADMSG);
        if(port>=3) CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
    p[12]=0;
    for(unsigned o=0;o<256;o++) for(unsigned m=0;m<256;m++) {
        p[13]=o;p[14]=m;
        CHECK(yoga_pan_decode(p,32,&out)==0);
        CHECK(out.orientation_raw==o && out.mux_raw==m);
    }
    p[13]=0;p[14]=4;
    p[10]=0x87;p[11]=0x80;p[16]=0x5c;p[17]=0x0b;p[18]=0xff;p[19]=0x00;
    for(unsigned i=0;i<8;i++) p[20+i]=i*31;
    CHECK(yoga_pan_decode(p,32,&out)==0);
    CHECK(out.svid_header==0x8087 && out.svid_payload==0xff && out.vid==0x0b5c);
    CHECK(memcmp(out.ext,p+20,8)==0);
    for(unsigned byte=0;byte<10;byte++) for(unsigned bit=0;bit<8;bit++) {
        p[byte]^=1u<<bit;out=before;
        CHECK(yoga_pan_decode(p,32,&out)==-EBADMSG);
        CHECK(memcmp(&out,&before,sizeof(out))==0);
        p[byte]^=1u<<bit;
    }
    printf("PAN packet PASS: %u exact-production checks; no hardware emulation.\n", checks);
}
