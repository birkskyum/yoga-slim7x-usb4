/* SPDX-License-Identifier: GPL-2.0-only */
/* Private staged operations in the owning PHY, restricted to the RAM image. */
static int x1_diag_phy_report(struct qmp_combo *qmp, const char *stage, int result)
{
	dev_info(qmp->dev, "PHY PREP %s returned %d\n", stage, result);
	return result;
}

int x1_diag_phy_apply(struct phy *phy, enum x1_diag_phy_op op)
{
	struct qmp_combo *qmp;
	struct device_node *expected;
	bool identity;
	u32 mode, clamp;
	int ret;
	if (!phy || !x1_diag_board_allowed())
		return -EPERM;
	qmp = phy_get_drvdata(phy);
	if (!qmp || qmp->cfg != &x1e80100_usb43dpphy_cfg || phy != qmp->usb4_phy)
		return -EINVAL;
	expected = of_find_node_by_path("/soc@0/phy@fd5000");
	identity = expected && expected == qmp->dev->of_node;
	of_node_put(expected);
	if (!identity || qmp->usb4_rx_eq != 0x6f || !qmp->usb4_dp_aux ||
	    !qmp->p2rr2p_pipe_clk || qmp->cfg->num_resets != 2)
		return -EINVAL;
	guard(mutex)(&qmp->phy_mutex);
	if (qmp->usb_init_count || qmp->dp_init_count || qmp->usb4_init_count ||
	    qmp->init_count || qmp->com_powered_on)
		return -EBUSY;
	if (op == X1_DIAG_PHY_DP_ASSERT || op == X1_DIAG_PHY_DP_CLEAR) {
		if (!qmp->x1_diag_prepared)
			return -EPERM;
		/* The PHY already owns the exclusive "common" reset handle. */
		return op == X1_DIAG_PHY_DP_ASSERT ?
			reset_control_assert(qmp->resets[1].rstc) :
			reset_control_deassert(qmp->resets[1].rstc);
	}
	if (op != X1_DIAG_PHY_PREPARE)
		return -EINVAL;
	if (qmp->x1_diag_attempted)
		return -EALREADY;
	qmp->x1_diag_attempted = true;
	/*
	 * Extra references are deliberately retained until full shutdown. Later
	 * phy_init() establishes its own normal references exactly once. A partial
	 * failure must not auto-retry or silently remove power from the router.
	 */
	ret = x1_diag_phy_report(qmp, "1/runtime-pm", pm_runtime_resume_and_get(qmp->dev));
	if (ret < 0)
		return ret;
	ret = x1_diag_phy_report(qmp, "2/regulators",
		regulator_bulk_enable(qmp->cfg->num_vregs, qmp->vregs));
	if (ret)
		return ret;
	ret = x1_diag_phy_report(qmp, "3/clock-bulk",
		clk_bulk_prepare_enable(qmp->num_clks, qmp->clks));
	if (ret)
		return ret;
	ret = x1_diag_phy_report(qmp, "4/pipe-clock", clk_prepare_enable(qmp->pipe_clk));
	if (ret)
		return ret;
	ret = x1_diag_phy_report(qmp, "5/p2rr2p-clock", clk_prepare_enable(qmp->p2rr2p_pipe_clk));
	if (ret)
		return ret;
	mode = readl(qmp->com + QPHY_V3_DP_COM_PHY_MODE_CTRL);
	dev_info(qmp->dev, "PHY PREP 6/mode-control read=%08x\n", mode);
	if (mode & DP_TUNNELING_CLOCK_GEN_EN)
		return -EBUSY;
	/* X1 reference initial PHY +0x104 is PCS_AON USB4 CLAMP_ENABLE. */
	writel(CLAMP_EN, qmp->pcs_aon + QPHY_V6_PCS_AON_USB4_CLAMP_ENABLE);
	clamp = readl(qmp->pcs_aon + QPHY_V6_PCS_AON_USB4_CLAMP_ENABLE);
	dev_info(qmp->dev, "PHY PREP 7/clamp base+0x104 wrote=%08x read=%08x\n",
		 (u32)CLAMP_EN, clamp);
	/* CLAMP_EN is bit 0; upper readback bits are not part of this field. */
	if ((clamp & CLAMP_EN) != CLAMP_EN)
		return -EIO;
	qmp->x1_diag_prepared = true;
	return 0;
}
EXPORT_SYMBOL_GPL(x1_diag_phy_apply);
