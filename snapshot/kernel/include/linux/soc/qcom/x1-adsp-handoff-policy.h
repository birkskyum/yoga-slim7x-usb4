/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X1_ADSP_HANDOFF_POLICY_H
#define X1_ADSP_HANDOFF_POLICY_H

#ifdef __KERNEL__
/* Narrow built-in bridge: ARM64's boot-mode globals are not module exports. */
bool x1_adsp_el2_available(void);

/* full_name is a local node name after unflattening, not an absolute path. */
static inline bool x1_adsp_exact_node(struct device_node *np)
{
	struct device_node *expected;
	bool match;

	if (!np)
		return false;
	expected = of_find_node_by_path("/soc@0/remoteproc@6800000");
	match = expected && np == expected &&
		of_device_is_compatible(np, "qcom,x1e80100-adsp-pas");
	of_node_put(expected);
	return match;
}
#endif

/* Pure policy helpers shared with the offline tests. No hardware access. */
static inline bool x1_adsp_handoff_allowed(bool selected, bool hyp, bool yoga,
					 bool marker, bool adsp, bool broken_reset,
					 bool iommu)
{
	return selected && hyp && yoga && marker && adsp && broken_reset && !iommu;
}

/* ready, handover, fatal, stop; errors must never look like clear bits. */
static inline bool x1_adsp_signals_healthy(const int err[4], const bool bit[4])
{
	return !err[0] && !err[1] && !err[2] && !err[3] &&
		bit[0] && bit[1] && !bit[2] && !bit[3];
}

static inline bool x1_adsp_smem_valid(unsigned long size, unsigned long required,
		unsigned int magic, unsigned int version, unsigned int total,
		unsigned int valid, unsigned int local, unsigned int remote,
		unsigned int expected_local, unsigned int expected_remote)
{
	return size >= required && magic == 0x504d5324 &&
		(version == 1 || version == 2) && total == 16 && valid <= total &&
		local == expected_local && remote == expected_remote;
}
#endif
