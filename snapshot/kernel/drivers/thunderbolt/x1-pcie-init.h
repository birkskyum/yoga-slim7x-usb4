/* SPDX-License-Identifier: GPL-2.0-only */
/* Private Hamoa PCI0 initialization. Hardware facts from the pinned offline
 * PPX/ACPI audit; see PCIE-INIT-EVIDENCE.md. Not a generic Qualcomm recipe.
 * Callbacks keep the bounded sequence independently testable without MMIO.
 */
#ifndef X1_PCIE_INIT_H
#define X1_PCIE_INIT_H
enum x1_pci0_region { X1_ROOT, X1_PARF, X1_ROUTER };
struct x1_pci0_io {
	void *ctx;
	u32 (*read)(void *, unsigned int, u32);
	void (*write)(void *, unsigned int, u32, u32);
	int (*reset)(void *, bool);
	void (*delay)(void *, unsigned int);
};
static u32 x1_pr(struct x1_pci0_io *io, unsigned int r, u32 off)
{
	return io->read(io->ctx, r, off);
}
static void x1_pw(struct x1_pci0_io *io, unsigned int r, u32 off, u32 v)
{
	io->write(io->ctx, r, off, v);
}
static int x1_pm(struct x1_pci0_io *io, unsigned int r, u32 off, u32 mask, u32 v)
{
	u32 old = x1_pr(io, r, off);
	if (old == 0xffffffff) return -EIO;
	x1_pw(io, r, off, (old & ~mask) | v);
	return (x1_pr(io, r, off) & mask) == v ? 0 : -EIO;
}
static int x1_pv(struct x1_pci0_io *io, unsigned int r, u32 off, u32 v)
{
	x1_pw(io, r, off, v);
	return x1_pr(io, r, off) == v ? 0 : -EIO;
}
static int x1_pp(struct x1_pci0_io *io, unsigned int r, u32 off,
		 u32 mask, u32 expected, unsigned int ms)
{
	unsigned int i;
	for (i = 0; i <= ms; i++) {
		u32 v = x1_pr(io, r, off);
		if (v == 0xffffffff) return -EIO;
		if ((v & mask) == expected) return 0;
		if (i != ms) io->delay(io->ctx, 1);
	}
	return -ETIMEDOUT;
}

/* Refuse active/inherited state. A changed class/header is not an excuse to
 * reset some other owner. The photo supplied this exact quiescent signature.
 */
static int x1_pci0_mode(struct x1_pci0_io *io)
{
	u32 gate;
	int ret;
	if (x1_pr(io, X1_ROOT, 0) != 0x011117cb ||
	    x1_pr(io, X1_ROOT, 4) != 0x00100000 ||
	    x1_pr(io, X1_ROOT, 8) != 0xff000001 ||
	    x1_pr(io, X1_ROOT, 12) != 0)
		return -ENODEV;
	gate = x1_pr(io, X1_ROUTER, 0x82000);
	/* The reset-request mailbox is a later training-phase resource, not
	 * a pre-RC-mode liveness probe. v24 stopped at this premature read.
	 * Preserve its busy/ack checks in x1_pci0_reset_request(), reached only
	 * after validated RC setup and exact-device tunnel approval.
	 */
	if (gate > 1) return -EBUSY;
	ret = x1_pm(io, X1_PARF, 0x1b0, 0x100, 0);
	if (ret) return ret;
	x1_pw(io, X1_ROUTER, 0x82000, 1);
	if (x1_pr(io, X1_ROUTER, 0x82000) != 1) return -EIO;
	ret = io->reset(io->ctx, true);
	if (ret) return ret;
	io->delay(io->ctx, 5);
	ret = io->reset(io->ctx, false);
	if (ret) return ret;
	io->delay(io->ctx, 5);
	ret = x1_pm(io, X1_PARF, 0x1000, 0xf, 4);
	if (ret) return ret;
	x1_pw(io, X1_ROUTER, 0x82000, 0);
	if (x1_pr(io, X1_ROUTER, 0x82000)) return -EIO;
	io->delay(io->ctx, 5);
	/* Require the physical header type to change, never write/fake it. */
	if (x1_pr(io, X1_ROOT, 0) != 0x011117cb ||
	    ((x1_pr(io, X1_ROOT, 12) >> 16) & 0xff) != 1)
		return -ENODEV;
	return 0;
}

/* Run only AFTER independent Type 1 / PCIe Root Port capability validation.
 * Fixed board windows: no arbitrary table interpreter, address or payload.
 * No 0x9a-platform PCI ID write; PCI0 remains 17cb:0111.
 */
static int x1_pci0_setup(struct x1_pci0_io *io)
{
	static const u32 parf[][2] = {
		{0x350, 0}, {0x354, 4}, {0x634, 0x1000}, {0x638, 4},
		{0x64c, 0xffffffff}, {0x650, 0xffffffff}, {0x358, 0}, {0x35c, 0x200},
	};
	static const u32 dbi[][3] = {
		{0xb90, 0x100, 0x100}, {0x71c, 0xffff0000, 0x04040000},
		{0x644, 1, 0}, {0x718, 0x7c000, 0x10000},
		{0x20, 0xfff0fff0, 0x4ff04000},
	};
	unsigned int i;
	int ret;
	ret = x1_pm(io, X1_PARF, 0x1b4, 0xfff, 0xf40);
	if (ret) return ret;
	ret = x1_pm(io, X1_PARF, 0x20, 0x20, 0);
	if (ret) return ret;
	for (i = 0; i < ARRAY_SIZE(parf); i++) {
		ret = x1_pv(io, X1_PARF, parf[i][0], parf[i][1]);
		if (ret) return ret;
	}
	(void)x1_pr(io, X1_PARF, 0x35c);
	for (i = 0; i < 2; i++) {
		ret = x1_pm(io, X1_ROOT, dbi[i][0], dbi[i][1], dbi[i][2]);
		if (ret) return ret;
	}
	ret = x1_pm(io, X1_ROOT, 0x8bc, 1, 1);
	if (ret) return ret;
	ret = x1_pm(io, X1_ROOT, 8, 0xffff0000, 0x06040000);
	if (!ret) ret = x1_pm(io, X1_ROOT, 0x84, 0x60, 0x60);
	/* Close the DBI write gate even if a protected-field check fails. */
	if (x1_pm(io, X1_ROOT, 0x8bc, 1, 0)) return -EIO;
	if (ret) return ret;
	for (i = 2; i < ARRAY_SIZE(dbi); i++) {
		ret = x1_pm(io, X1_ROOT, dbi[i][0], dbi[i][1], dbi[i][2]);
		if (ret) return ret;
	}
	ret = x1_pm(io, X1_ROOT, 0xfe4, 1, 1);
	if (ret) return ret;
	x1_pw(io, X1_ROOT, 0x10, 0);
	x1_pw(io, X1_ROOT, 0x14, 0);
	(void)x1_pr(io, X1_ROOT, 0x14);
	ret = x1_pm(io, X1_ROOT, 0xfe4, 1, 0);
	if (ret) return ret;
	/* Keep endpoint config blocked; only the root command register here. */
	return x1_pm(io, X1_ROOT, 4, 6, 6);
}

static int x1_pci0_reset_request(struct x1_pci0_io *io, u32 bit)
{
	u32 old = x1_pr(io, X1_ROUTER, 0xe008);
    /* Only bits 0/1 are reset requests; preserve all other bits (v26: 0x30).
     * Reject either outstanding request, including an all-ones read.
     * Pinned PPX 0x14000f130 ORs the old word with the selected request. */
    if ((old & 3) || (bit != 1 && bit != 2)) return -EBUSY;
    x1_pw(io, X1_ROUTER, 0xe008, old | bit);
	return x1_pp(io, X1_ROUTER, 0xe008, bit, 0, 10);
}

/* Called once only after the exact LaCie PCIe tunnel was approved. Bounded
 * replacement for PPX's unbounded LTSSM-enable loop. No link retries.
 */
static int x1_pci0_train(struct x1_pci0_io *io)
{
	int ret = x1_pci0_reset_request(io, 1);
	if (ret) return ret;
	io->delay(io->ctx, 1);
	ret = x1_pci0_reset_request(io, 2);
	if (ret) return ret;
	io->delay(io->ctx, 20);
	x1_pw(io, X1_PARF, 0x494, 0);
	x1_pw(io, X1_PARF, 0x494, 2);
	x1_pw(io, X1_PARF, 0x494, 0);
	x1_pw(io, X1_PARF, 0x494, 5);
	ret = x1_pm(io, X1_PARF, 0x1b0, 0x100, 0x100);
	if (!ret) ret = x1_pp(io, X1_ROOT, 0xf48, 0x400, 0x400, 150);
	x1_pw(io, X1_PARF, 0x494, 0);
	if (!ret) ret = x1_pp(io, X1_PARF, 0x1b0, 0x3f, 0x11, 150);
	return ret;
}

/* ECAM flag 1 configuration from PPX 0x140010178..0x1400106ac. Use
 * link-up path (flag=1), with a 1-MiB local DBI decode window. Physical regions
 * match this board's ACPI and DT. Translation never covers the internal SSD.
 */
static int x1_pci0_windows(struct x1_pci0_io *io)
{
	static const u32 root[][2] = {
		{0x1008, 0x00100000}, {0x100c, 4}, {0x1010, 0x001fffff},
		{0x1014, 0x01000000}, {0x1018, 0}, {0x1000, 4}, {0x1004, 0x90000000},
		{0x1208, 0x00200000}, {0x120c, 4}, {0x1210, 0x0fffffff},
		{0x1200, 5}, {0x1204, 0x90000000},
	};
	static const u32 parf[][2] = {
		{0x380, 0}, {0x360, 0x1000}, {0x364, 4},
		{0x370, 0x1000}, {0x374, 4},
		{0x388, 0x000fffff}, {0x390, 0x008fefff},
		{0x368, 0x000fffff}, {0x36c, 4}, {0x378, 0x000fffff}, {0x37c, 4},
		{0x398, 0x101000}, {0x39c, 4}, {0x3a8, 0x101000}, {0x3ac, 4},
		{0x3a0, 0x1fffff}, {0x3a4, 4}, {0x3b0, 0x1fffff}, {0x3b4, 4},
	};
	unsigned int i;
	int ret;
	for (i = 0; i < ARRAY_SIZE(root); i++) {
		ret = x1_pv(io, X1_ROOT, root[i][0], root[i][1]);
		if (ret) return ret;
	}
	for (i = 0; i < ARRAY_SIZE(parf); i++) {
		ret = x1_pv(io, X1_PARF, parf[i][0], parf[i][1]);
		if (ret) return ret;
	}
	/* The enable bit must latch on both config regions. */
	if ((x1_pr(io, X1_ROOT, 0x1004) & 0x90000000) != 0x90000000 ||
	    (x1_pr(io, X1_ROOT, 0x1204) & 0x90000000) != 0x90000000)
		return -EIO;
	return x1_pm(io, X1_PARF, 0, 0x44000000, 0x44000000);
}
#endif
