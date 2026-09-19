/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define __KERNEL__
struct device_node { const char *full_name; bool compatible; };
static struct device_node good = {"remoteproc@6800000", true};
static struct device_node wrong = {"remoteproc@6800000", true};
static bool missing;
static int refs;
static struct device_node *of_find_node_by_path(const char *path)
{
	assert(!strcmp(path, "/soc@0/remoteproc@6800000"));
	if (missing) return NULL;
	refs++;
	return &good;
}
static void of_node_put(struct device_node *np) { if (np) refs--; }
static bool of_device_is_compatible(struct device_node *np, const char *name)
{
	assert(!strcmp(name, "qcom,x1e80100-adsp-pas"));
	return np->compatible;
}
#include "kernel/include/linux/soc/qcom/x1-adsp-handoff-policy.h"

int main(void)
{
	unsigned int i, j;
	assert(x1_adsp_exact_node(&good) && !refs);
	assert(!x1_adsp_exact_node(&wrong) && !refs);
	assert(!x1_adsp_exact_node(NULL) && !refs);
	good.compatible = false;
	assert(!x1_adsp_exact_node(&good) && !refs);
	good.compatible = true;
	missing = true;
	assert(!x1_adsp_exact_node(&good) && !refs);
	missing = false;
	for (i = 0; i < 128; i++)
		assert(x1_adsp_handoff_allowed(i & 1, i & 2, i & 4, i & 8,
			i & 16, i & 32, i & 64) == (i == 63));
	for (i = 0; i < 16; i++) {
		bool state[4] = {i & 1, i & 2, i & 4, i & 8};
		int err[4] = {};
		assert(x1_adsp_signals_healthy(err, state) == (i == 3));
		for (j = 0; j < 4; j++) {
			err[j] = -19;
			assert(!x1_adsp_signals_healthy(err, state));
			err[j] = 0;
		}
	}
	assert(x1_adsp_smem_valid(360, 360, 0x504d5324, 1, 16, 1, 0, 2, 0, 2));
	assert(x1_adsp_smem_valid(360, 360, 0x504d5324, 2, 16, 16, 2, 0, 2, 0));
	assert(!x1_adsp_smem_valid(359, 360, 0x504d5324, 2, 16, 16, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0, 2, 16, 16, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0x504d5324, 0, 16, 16, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0x504d5324, 3, 16, 16, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0x504d5324, 2, 17, 16, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0x504d5324, 2, 16, 17, 0, 2, 0, 2));
	assert(!x1_adsp_smem_valid(360, 360, 0x504d5324, 2, 16, 1, 2, 0, 0, 2));
	puts("PASS ADSP handoff policy: 128 environments, all signal masks and errors, SMEM bounds");
	return 0;
}
