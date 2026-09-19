/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure predicates: no hardware access and no authority to create tunnels. */
#ifndef X1_NATIVE_POLICY_H
#define X1_NATIVE_POLICY_H
#include <linux/errno.h>
#include <linux/types.h>

/* Public snapshot intentionally has no approved external SSD identity.
 * Keep this fail-closed. An independently reviewed local hardware experiment
 * needs its own exact native-endian UID, not a vendor-only match.
 */
#ifndef X1_NATIVE_LACIE_UID
#define X1_NATIVE_LACIE_UID 0ULL
#endif

static inline int x1_native_route_allowed(u64 route)
{
	return route == 0 || route == 2 ? 0 : -EPERM;
}

static inline int x1_native_header_allowed(u64 route, const u32 *h)
{
	if (!h || x1_native_route_allowed(route))
		return -EPERM;
	/* Identity, USB4 revision, port count and upstream adapter, not route state. */
	if (!route)
		return h[0] == 0x093805c6 && (h[1] & 0xfffff) == 0x1c11c ? 0 : -ENODEV;
	return h[0] == 0x113b059f && (h[1] & 0xfffff) == 0x14120 ? 0 : -ENODEV;
}

static inline int x1_native_uid_allowed(u64 route, u64 uid)
{
	return X1_NATIVE_LACIE_UID != 0 && route == 2 && uid == X1_NATIVE_LACIE_UID ? 0 : -ENODEV;
}

static inline int x1_native_connect_word_allowed(u32 word)
{
	/* Same passive USB4 cable, board retimer, normal or flipped orientation. */
	return word == 0x2501 || word == 0x2701 ? 0 : -EPERM;
}

/* Only a pre-configuration transient failure can request one rescan. */
static inline bool x1_native_rescan_allowed(bool used, bool preconfig, int error)
{
	return !used && preconfig &&
		(error == -ETIMEDOUT || error == -ENOTCONN || error == -EAGAIN);
}
#endif
