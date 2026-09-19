/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X1_NATIVE_H
#define X1_NATIVE_H

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/types.h>

struct device;
struct tb;
int tb_x1_native_rescan(struct tb *tb);

#if IS_ENABLED(CONFIG_USB4_X1_NATIVE)
bool x1_native_test(const struct tb *tb);
bool x1_native_pcie_authorizing(const struct tb *tb);
int x1_native_start(struct device *dev, void __iomem *router, int irq);
int x1_native_register_connect(int (*connect)(void *, u32), void *ctx);
int x1_native_connect(u32 word, bool (*valid)(void *), void *ctx);
void x1_native_stop(void);
int x1_native_guard_router(struct tb *tb, u64 route, const u32 *header);
void x1_native_scan_result(struct tb *tb, u64 route, unsigned int port,
			   const char *stage, int error, bool preconfig);
#else
static inline bool x1_native_test(const struct tb *tb) { return false; }
static inline bool x1_native_pcie_authorizing(const struct tb *tb) { return false; }
static inline int x1_native_start(struct device *dev, void __iomem *router, int irq) { return -EOPNOTSUPP; }
static inline int x1_native_register_connect(int (*connect)(void *, u32), void *ctx) { return -EOPNOTSUPP; }
static inline int x1_native_connect(u32 word, bool (*valid)(void *), void *ctx) { return -EOPNOTSUPP; }
static inline void x1_native_stop(void) { }
static inline int x1_native_guard_router(struct tb *tb, u64 route, const u32 *header) { return 0; }
static inline void x1_native_scan_result(struct tb *tb, u64 route, unsigned int port,
					 const char *stage, int error, bool preconfig) { }
#endif
#endif
