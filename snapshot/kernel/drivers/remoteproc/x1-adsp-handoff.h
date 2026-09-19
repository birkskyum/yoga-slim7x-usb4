/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Experimental Yoga-only adaptation of Stephan Gerhold's q6v5 attach and
 * broken-reset work (2025). See docs/EL2-HANDOFF.md for the original patches.
 * Included by qcom_q6v5_pas.c. Intentionally not a generic PAS replacement.
 */
#if IS_ENABLED(CONFIG_USB4_X1_ADSP_HANDOFF)
#include <linux/soc/qcom/x1-adsp-handoff-policy.h>

static int x1_adsp_check_signals(struct qcom_q6v5 *q6v5)
{
	int irq[4] = { q6v5->ready_irq, q6v5->handover_irq,
		       q6v5->fatal_irq, q6v5->stop_irq };
	bool state[4] = {};
	int ret[4], i;

	for (i = 0; i < 4; i++)
		ret[i] = irq_get_irqchip_state(irq[i], IRQCHIP_STATE_LINE_LEVEL, &state[i]);
	dev_info(q6v5->dev, "X1 ADSP HANDOFF signals ready=%d handover=%d fatal=%d stop=%d errors=%d/%d/%d/%d\n",
		 state[0], state[1], state[2], state[3], ret[0], ret[1], ret[2], ret[3]);
	return x1_adsp_signals_healthy(ret, state) ? 0 : -ENODEV;
}

static int x1_adsp_no_restart(struct rproc *rproc)
{
	dev_err(&rproc->dev, "X1 ADSP HANDOFF restart/stop/detach refused; cold power-off required\n");
	return -EOPNOTSUPP;
}

static int x1_adsp_no_load(struct rproc *rproc, const struct firmware *fw)
{
	return x1_adsp_no_restart(rproc);
}

static int x1_adsp_attach(struct rproc *rproc)
{
	struct qcom_pas *pas = rproc->priv;
	struct qcom_q6v5 *q6v5 = &pas->q6v5;
	int ret;

	if (rproc->state != RPROC_DETACHED || q6v5->running)
		return -EPERM;
	ret = x1_adsp_check_signals(q6v5);
	if (ret)
		return ret; /* Never set OFFLINE, never fall back to firmware boot. */
	if (q6v5->qmp) {
		ret = qmp_send(q6v5->qmp,
			"{class: image, res: load_state, name: adsp, val: on}");
		if (ret)
			return ret;
	}
	q6v5->handover_issued = true;
	q6v5->running = true;
	complete(&q6v5->start_done);
	/* Handover already happened. We hold no proxy votes to release and keep
	 * its IRQ disabled. Fatal/watchdog IRQs retain normal crash reporting.
	 */
	dev_info(pas->dev, "X1 ADSP HANDOFF attached without PAS; recovery disabled; not USB4 proof\n");
	return 0;
}

static const struct rproc_ops x1_adsp_handoff_ops = {
	.attach = x1_adsp_attach,
	.start = x1_adsp_no_restart,
	.stop = x1_adsp_no_restart,
	.detach = x1_adsp_no_restart,
	.load = x1_adsp_no_load,
};

static int x1_adsp_handoff_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct qcom_pas *pas;
	struct rproc *rproc;
	int ret;

	if (!x1_adsp_handoff_allowed(IS_ENABLED(CONFIG_USB4_X1_ADSP_HANDOFF),
		x1_adsp_el2_available(), of_machine_is_compatible("lenovo,yoga-slim7x"),
		of_property_read_bool(of_root, "birk,adsp-el2-service-test"),
		x1_adsp_exact_node(np),
		of_property_read_bool(np, "qcom,broken-reset"),
		of_property_present(np, "iommus")))
		return dev_err_probe(&pdev->dev, -EPERM, "X1 ADSP HANDOFF environment refused\n");

	rproc = devm_rproc_alloc(&pdev->dev, "adsp", &x1_adsp_handoff_ops,
				"disabled-no-linux-firmware-load", sizeof(*pas));
	if (!rproc)
		return -ENOMEM;
	pas = rproc->priv;
	pas->dev = &pdev->dev;
	pas->rproc = rproc;
	platform_set_drvdata(pdev, pas);
	rproc->has_iommu = false; /* Keep the proven firmware-owned ADSP mapping. */
	rproc->auto_boot = true;
	rproc->recovery_disabled = true;
	rproc->sysfs_read_only = true;
	rproc->state = RPROC_DETACHED;
	ret = qcom_q6v5_init(&pas->q6v5, pdev, rproc, 423, "adsp", NULL);
	if (ret)
		return ret;
	ret = x1_adsp_check_signals(&pas->q6v5);
	if (ret)
		goto deinit;

	qcom_add_glink_subdev(rproc, &pas->glink_subdev, "lpass");
	qcom_add_smd_subdev(rproc, &pas->smd_subdev);
	qcom_add_pdm_subdev(rproc, &pas->pdm_subdev);
	pas->sysmon = qcom_add_sysmon_subdev(rproc, "adsp", 0x14);
	if (IS_ERR(pas->sysmon)) {
		ret = PTR_ERR(pas->sysmon);
		goto remove_subdevs;
	}
	qcom_add_ssr_subdev(rproc, &pas->ssr_subdev, "lpass");
	ret = rproc_add(rproc);
	if (ret)
		goto remove_sysmon;
	/* A successful one-shot attachment is retained until cold power-off.
	 * Also block module unload after an asynchronous attach failure.
	 */
	__module_get(THIS_MODULE);
	return 0;

remove_sysmon:
	qcom_remove_ssr_subdev(rproc, &pas->ssr_subdev);
	qcom_remove_sysmon_subdev(pas->sysmon);
remove_subdevs:
	qcom_remove_pdm_subdev(rproc, &pas->pdm_subdev);
	qcom_remove_smd_subdev(rproc, &pas->smd_subdev);
	qcom_remove_glink_subdev(rproc, &pas->glink_subdev);
deinit:
	qcom_q6v5_deinit(&pas->q6v5);
	return ret;
}
#else
static int x1_adsp_handoff_probe(struct platform_device *pdev)
{
	return -EPERM;
}
#endif
