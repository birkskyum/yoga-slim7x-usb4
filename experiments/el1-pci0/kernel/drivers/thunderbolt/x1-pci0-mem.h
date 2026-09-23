/* SPDX-License-Identifier: GPL-2.0-only */
/* PRIVATE X1 outbound MEM window. No endpoint access, reset, or inbound DMA.
 * Register sequence derives from dw_pcie_prog_outbound_atu(), pinned baseline.
 * Region 0 remains the already verified fixed CFG0; region 1 must be unused.
 * The caller validates the exact DT range and retained, trained cold owner.
 */
#ifndef LOCAL_X1_PCI0_MEM_H
#define LOCAL_X1_PCI0_MEM_H
#include <linux/errno.h>
#include <linux/types.h>

struct x1_mem_io {
	void *context;
	int (*check)(void *context);
	int (*read)(void *context, u32 offset, u32 *value);
	int (*write)(void *context, u32 offset, u32 value);
	int (*delay)(void *context, unsigned int ms);
};
struct x1_mem_state {
	bool attempted, enable_issued, complete;
	unsigned int writes;
	int error;
	const char *step;
};

static inline bool x1_mem_offset(u32 off)
{
	switch (off) {
	case 0x1200: case 0x1204: case 0x1208: case 0x120c:
	case 0x1210: case 0x1214: case 0x1218: case 0x1220:
		return true;
	default:
		return false;
	}
}

static inline int x1_mem_read(struct x1_mem_io *io, u32 off, u32 *value)
{
	int ret;

	if (!x1_mem_offset(off))
		return -EINVAL;
	ret = io->check(io->context);
	if (!ret)
		ret = io->read(io->context, off, value);
	return ret > 0 ? -EIO : ret;
}

static inline int x1_mem_expect(struct x1_mem_io *io, u32 off, u32 value)
{
	u32 got;
	int ret = x1_mem_read(io, off, &got);

	return ret ?: (got == value ? 0 : -EIO);
}

static inline int x1_mem_set(struct x1_mem_io *io, struct x1_mem_state *s,
			     u32 off, u32 value)
{
	int ret;

	if (!x1_mem_offset(off))
		return -EINVAL;
	ret = io->check(io->context);
	if (ret)
		return ret > 0 ? -EIO : ret;
	s->writes++; /* Issued, not proof of successful arrival. */
	ret = io->write(io->context, off, value);
	return ret ? (ret > 0 ? -EIO : ret) : x1_mem_expect(io, off, value);
}

static inline int x1_mem_prepare(struct x1_mem_io *io, struct x1_mem_state *s)
{
	/* Fixed 256 MiB CPU == PCI aperture; not a DMA/IOMMU aperture. */
	static const u32 words[][2] = {
		{0x1208, 0x40000000}, {0x120c, 0},
		{0x1210, 0x4fffffff}, {0x1220, 0},
		{0x1214, 0x40000000}, {0x1218, 0}, {0x1200, 0},
	};
	u32 value;
	unsigned int i;
	int ret;

	if (!io || !s || !io->check || !io->read || !io->write || !io->delay)
		return -EINVAL;
	if (s->attempted)
		return -EALREADY;
	s->attempted = true;
	s->step = "mem-inactive-ob1";
	ret = x1_mem_expect(io, 0x1204, 0);
	if (ret)
		goto done;
	s->step = "mem-program-ob1";
	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		ret = x1_mem_set(io, s, words[i][0], words[i][1]);
		if (ret)
			goto done;
	}
	/* Detect aliasing before issuing the single enable. */
	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		ret = x1_mem_expect(io, words[i][0], words[i][1]);
		if (ret)
			goto done;
	}
	ret = x1_mem_expect(io, 0x1204, 0);
	if (ret)
		goto done;
	s->step = "mem-enable-ob1";
	ret = io->check(io->context);
	if (ret) {
		ret = ret > 0 ? -EIO : ret;
		goto done;
	}
	s->enable_issued = true;
	s->writes++;
	ret = io->write(io->context, 0x1204, 0x80000000);
	if (ret) {
		ret = ret > 0 ? -EIO : ret;
		goto done;
	}
	for (i = 0; i < 5; i++) {
		ret = x1_mem_read(io, 0x1204, &value);
		if (ret)
			goto done;
		if (value == 0x80000000)
			break;
		if (value || i == 4) {
			ret = value ? -EIO : -ETIMEDOUT;
			goto done;
		}
		ret = io->delay(io->context, 10);
		if (ret) {
			ret = ret > 0 ? -EIO : ret;
			goto done;
		}
	}
	s->step = "mem-final-readback";
	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		ret = x1_mem_expect(io, words[i][0], words[i][1]);
		if (ret)
			goto done;
	}
	ret = x1_mem_expect(io, 0x1204, 0x80000000);
	if (!ret) {
		s->complete = true;
		s->step = "mem-ready-not-endpoint-dma-proof";
	}
done:
	s->error = ret;
	return ret;
}
#endif
