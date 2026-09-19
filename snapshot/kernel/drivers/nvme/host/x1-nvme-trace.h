/* SPDX-License-Identifier: GPL-2.0 */
/* Private Yoga diagnostic: observe existing operations, never add MMIO. */
#ifndef X1_NVME_TRACE_H
#define X1_NVME_TRACE_H

static atomic_t x1_nvme_trace_budget = ATOMIC_INIT(128);

static inline void x1_nvme_stage(const char *phase, const char *fn,
				 const char *operation)
{
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE))
		pr_emerg("V33 NVME %s %s: %s\n", phase, fn, operation);
}

static inline bool x1_nvme_trace_begin(const char *fn, const char *operation)
{
	int left;

	if (!IS_ENABLED(CONFIG_USB4_X1_NATIVE))
		return false;
	left = atomic_dec_if_positive(&x1_nvme_trace_budget);
	if (left < 0)
		return false;
	if (!left)
		pr_emerg("V33 NVME last traced MMIO; later MMIO unchanged, stage markers continue\n");
	x1_nvme_stage("MMIO BEGIN", fn, operation);
	return true;
}

/* typeof does not evaluate these non-VLA expressions. Each op runs once. */
#define X1_NVME_STEP(op) ({ \
	__typeof__(op) __x1_result; \
	x1_nvme_stage("BEGIN", __func__, #op); \
	__x1_result = (op); \
	x1_nvme_stage("END", __func__, #op); \
	__x1_result; \
})

#define X1_NVME_VOID(op) do { \
	x1_nvme_stage("BEGIN", __func__, #op); \
	(op); \
	x1_nvme_stage("END", __func__, #op); \
} while (0)

#define X1_NVME_READ(op) ({ \
	bool __x1_trace = x1_nvme_trace_begin(__func__, #op); \
	__typeof__(op) __x1_value = (op); \
	if (__x1_trace) \
		pr_emerg("V33 NVME MMIO END %s: %s value=%016llx\n", \
			 __func__, #op, (unsigned long long)__x1_value); \
	__x1_value; \
})

#define X1_NVME_WRITE(op) do { \
	bool __x1_trace = x1_nvme_trace_begin(__func__, #op); \
	(op); \
	if (__x1_trace) \
		x1_nvme_stage("MMIO END", __func__, #op); \
} while (0)

#endif
