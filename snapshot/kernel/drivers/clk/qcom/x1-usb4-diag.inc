/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by the owning X1 GCC driver. No separate MMIO mapping. */
static DEFINE_MUTEX(x1_diag_gcc_lock);
static struct regmap *x1_diag_gcc_map;
static struct device *x1_diag_gcc_owner;

static int x1_diag_gcc_write(u32 offset, u32 mask, u32 value)
{
	u32 actual = 0;
	int ret;
	/* Pre-access markers matter: a fabric fault need not return an errno. */
	dev_info(x1_diag_gcc_owner, "ACCESS GCC update BEGIN off=%05x mask=%08x want=%08x\n",
		 offset, mask, value);
	ret = regmap_update_bits(x1_diag_gcc_map, offset, mask, value);
	dev_info(x1_diag_gcc_owner, "ACCESS GCC update END off=%05x ret=%d\n", offset, ret);
	if (ret) {
		dev_err(x1_diag_gcc_owner, "DIAG GCC write off=%05x mask=%08x want=%08x ret=%d\n",
			offset, mask, value, ret);
		return ret;
	}
	dev_info(x1_diag_gcc_owner, "ACCESS GCC readback BEGIN off=%05x\n", offset);
	ret = regmap_read(x1_diag_gcc_map, offset, &actual);
	dev_info(x1_diag_gcc_owner, "ACCESS GCC readback END off=%05x ret=%d got=%08x\n",
		 offset, ret, actual);
	if (ret) {
		dev_err(x1_diag_gcc_owner, "DIAG GCC read off=%05x ret=%d\n", offset, ret);
		return ret;
	}
	if ((actual & mask) != value)
		dev_err(x1_diag_gcc_owner, "DIAG GCC mismatch off=%05x mask=%08x want=%08x got=%08x\n",
			offset, mask, value, actual);
	return (actual & mask) == value ? 0 : -EIO;
}

static int x1_diag_gcc_rx(bool phy)
{
	static const u32 gates[] = { 0x9f0b0, 0x9f0c0 };
	static const u32 muxes[] = { 0x9f0ac, 0x9f0bc };
	u32 want = phy ? 0 : 2;
	int i, ret;
	/* Establish both gates, even when the selectors already match. */
	for (i = 0; i < 2; ++i) {
		ret = x1_diag_gcc_write(gates[i], BIT(0), 0);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; ++i) {
		ret = x1_diag_gcc_write(muxes[i], GENMASK(1, 0), want);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; ++i) {
		ret = x1_diag_gcc_write(gates[i], BIT(0), BIT(0));
		if (ret)
			return ret;
	}
	dev_info(x1_diag_gcc_owner, "ACCESS GCC RX switch COMPLETE source=%s\n", phy ? "PHY" : "REFERENCE");
	return 0;
}

int x1_diag_gcc_claim(struct device *owner)
{
	guard(mutex)(&x1_diag_gcc_lock);
	if (!x1_diag_board_allowed() || !owner ||
	    !of_device_is_compatible(owner->of_node, "birk,yoga-x1-usb4-mcu-test"))
		return -EPERM;
	if (!x1_diag_gcc_map)
		return -ENODEV;
	if (x1_diag_gcc_owner)
		return -EBUSY;
	/* One RAM-only attempt. Retain owner until shutdown, not a reusable API. */
	x1_diag_gcc_owner = get_device(owner);
	return 0;
}
EXPORT_SYMBOL_GPL(x1_diag_gcc_claim);

int x1_diag_gcc_apply(struct device *owner, enum x1_diag_gcc_op op)
{
	u32 value = 0;
	int ret;
	guard(mutex)(&x1_diag_gcc_lock);
	if (!x1_diag_board_allowed() || !x1_diag_gcc_map ||
	    !owner || owner != x1_diag_gcc_owner)
		return -EPERM;
	switch (op) {
	case X1_GCC_RX_REFERENCE: return x1_diag_gcc_rx(false);
	case X1_GCC_RX_PHY: return x1_diag_gcc_rx(true);
	case X1_GCC_MISC_ASSERT: return x1_diag_gcc_write(0xad0f8, 0x7ff, 0x7ff);
	case X1_GCC_MISC_CLEAR: return x1_diag_gcc_write(0xad0f8, 0x7ff, 0);
	/* Exact X1 reference offset. Do not substitute PHY_PRIM_BCR 0x5000c. */
	case X1_GCC_EXTRA_ASSERT: return x1_diag_gcc_write(0x5001c, BIT(0), BIT(0));
	case X1_GCC_EXTRA_CLEAR: return x1_diag_gcc_write(0x5001c, BIT(0), 0);
	case X1_GCC_PIPE_ASSERT: return x1_diag_gcc_write(0xad0f8, BIT(3), BIT(3));
	case X1_GCC_PIPE_CLEAR: return x1_diag_gcc_write(0xad0f8, BIT(3), 0);
	case X1_GCC_SYS_READY:
		ret = regmap_read_poll_timeout(x1_diag_gcc_map, 0x9f054, value,
					      !(value & BIT(31)), 10, 100000);
		dev_info(owner, "DIAG GCC SYS_READY off=9f054 last=%08x ret=%d bound=100ms\n", value, ret);
		return ret;
	case X1_GCC_MEMORY_FORCE: return x1_diag_gcc_write(0x9f054, BIT(14), BIT(14));
	case X1_GCC_HWCG_OFF: return x1_diag_gcc_write(0x9f0a4, BIT(1), 0);
	default: return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(x1_diag_gcc_apply);

static void x1_diag_gcc_forget(void *unused)
{
	guard(mutex)(&x1_diag_gcc_lock);
	x1_diag_gcc_map = NULL;
	put_device(x1_diag_gcc_owner);
	x1_diag_gcc_owner = NULL;
}
