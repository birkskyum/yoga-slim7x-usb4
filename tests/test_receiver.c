/* Execute the exact v37 header using read-only MMIO callbacks. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
enum { X1_ROOT, X1_PARF, X1_ROUTER };
struct x1_pci0_io { void *ctx; u32 (*read)(void *, unsigned int, u32); };
static u32 x1_pr(struct x1_pci0_io *io, unsigned int r, u32 off)
{ return io->read(io->ctx, r, off); }
#include "kernel/drivers/thunderbolt/x1-pcie-msi-audit.h"
static const u32 expected[][2] = {
 {0,0x820},{0,0x824},{0,0x828},{0,0x82c},{0,0x830},
 {1,0x1a8},{1,0x234},{1,0x24c},{1,0x2c00}
};
struct mock { unsigned int count; u32 fill; };
static u32 read_reg(void *ctx, unsigned int region, u32 off)
{
 struct mock *m = ctx;
 unsigned int i = m->count++;
 assert(i < sizeof(expected)/sizeof(expected[0]));
 assert(region == expected[i][0] && off == expected[i][1]);
 return m->fill ^ i;
}
int main(void)
{
 const u32 patterns[] = {0, 0xffffffff, 0x17050040, 0xa5a5a5a5};
 for (unsigned int p=0; p<sizeof(patterns)/sizeof(patterns[0]); p++) {
  struct mock m={0,patterns[p]};
  struct x1_pci0_io io={&m,read_reg};
  u32 v[9];
  x1_pci0_msi_audit(&io,v);
  assert(m.count==9);
  for(unsigned int i=0; i<9; i++) assert(v[i]==(patterns[p]^i));
 }
 puts("PASS v37 receiver: exact production header, nine ordered reads, four value patterns, no write/delay/reset/mapping callback available.");
}
