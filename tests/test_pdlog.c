#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
static uint32_t get_unaligned_le32(const void *v) {
 const u8 *p=v; return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void put_unaligned_le32(uint32_t v,void *x) {
 u8 *p=x;for(int i=0;i<4;i++)p[i]=v>>(i*8);
}
#include "kernel/drivers/soc/qcom/yoga_pdlog_policy.h"
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while(0)
int main(void) {
 u8 req[16], reply[PDLOG_SIZE+12]; char text[PDLOG_SIZE+2];
 pdlog_request(req);
 const u8 expected[]={10,128,0,0,1,0,0,0,24,0,0,0,0,32,0,0};
 CHECK(!memcmp(req,expected,16));
 memset(reply,0,sizeof(reply));memcpy(reply,req,12);
 for (size_t len=0;len<sizeof(reply);len++) {
  u8 *short_packet=malloc(len?len:1); memset(short_packet,0,len);
  CHECK(pdlog_decode(short_packet,len,text)==-EMSGSIZE);free(short_packet);
 }
 CHECK(pdlog_decode(reply,sizeof(reply)+1,text)==-EMSGSIZE);
 CHECK(pdlog_decode(reply,sizeof(reply),text)==0 && text[0]==0);
 for(unsigned i=0;i<12;i++)for(unsigned bit=0;bit<8;bit++) {
  reply[i]^=1u<<bit; CHECK(pdlog_decode(reply,sizeof(reply),text)==-EBADMSG);reply[i]^=1u<<bit;
 }
 for(unsigned c=1;c<256;c++) {
  reply[12]=c;reply[13]=0;
  CHECK(pdlog_decode(reply,sizeof(reply),text)==0);
  CHECK(text[0]==((c>=32&&c<=126)||c=='\n'||c=='\t'?(char)c:'.'));
 }
 memset(reply+12,'x',PDLOG_SIZE);text[PDLOG_SIZE+1]='!';
 CHECK(pdlog_decode(reply,sizeof(reply),text)==0);
 CHECK(strlen(text)==PDLOG_SIZE && text[PDLOG_SIZE+1]=='!');
 for(unsigned stop_at=0;stop_at<=PDLOG_MAX_REQUESTS;stop_at++) {
  struct pdlog_state s={0};
  CHECK(pdlog_start(&s,false)==-EPERM && !s.attempted);
  CHECK(pdlog_start(&s,true)==0);
  CHECK(pdlog_start(&s,true)==-EPERM);
  for(unsigned i=0;i<stop_at;i++) {
   CHECK(pdlog_next(&s,false));CHECK(!pdlog_next(&s,false));
   CHECK(pdlog_reply(&s,0));CHECK(!pdlog_reply(&s,0));
  }
  CHECK(s.requests==stop_at && s.replies==stop_at);
  pdlog_stop(&s,0);
  CHECK(!pdlog_next(&s,false) && !pdlog_reply(&s,0));
  CHECK(pdlog_start(&s,true)==-EPERM);
 }
 struct pdlog_state s={0}; CHECK(pdlog_start(&s,true)==0);
 for(unsigned i=0;i<128;i++){CHECK(pdlog_next(&s,false));CHECK(pdlog_reply(&s,0));}
 CHECK(!pdlog_next(&s,false) && s.limited && s.requests==128 && !s.active);
 s=(struct pdlog_state){0};CHECK(pdlog_start(&s,true)==0);
 CHECK(!pdlog_next(&s,true) && s.limited && s.requests==0);
 const int errors[]={-ETIMEDOUT,-ENOTCONN,-EBADMSG,-EMSGSIZE,-EAGAIN};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  s=(struct pdlog_state){0};CHECK(pdlog_start(&s,true)==0);CHECK(pdlog_next(&s,false));
  CHECK(pdlog_reply(&s,errors[i]));CHECK(s.error==errors[i]&&!s.active&&!s.awaiting);
  CHECK(!pdlog_reply(&s,0) && !pdlog_next(&s,false));
  CHECK(pdlog_start(&s,true)==-EPERM);
  pdlog_stop(&s,-EIO);CHECK(s.error==errors[i]);
 }
 s=(struct pdlog_state){0};CHECK(pdlog_start(&s,true)==0);CHECK(pdlog_next(&s,false));
 pdlog_stop(&s,0);CHECK(!pdlog_reply(&s,0)&&!s.replies&&!s.error);
 printf("PASS: %u exact-production PDLOG wire/parser/bounds/state checks (not hardware or kernel-race validation)\n",checks);
}
