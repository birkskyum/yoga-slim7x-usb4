/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#include "kernel/drivers/thunderbolt/x1-native-policy.h"
int main(void)
{
    assert(x1_native_uid_allowed(2, 0) < 0);
    assert(x1_native_uid_allowed(0, X1_NATIVE_LACIE_UID) < 0);
    assert(x1_native_uid_allowed(1, X1_NATIVE_LACIE_UID) < 0);
    assert(x1_native_uid_allowed(3, X1_NATIVE_LACIE_UID) < 0);
    if (X1_NATIVE_LACIE_UID) {
        assert(X1_NATIVE_LACIE_UID == 0x123456789abcdef0ULL);
        assert(x1_native_uid_allowed(2, X1_NATIVE_LACIE_UID) == 0);
        for (unsigned int bit = 0; bit < 64; bit++)
            assert(x1_native_uid_allowed(2, X1_NATIVE_LACIE_UID ^ (1ULL << bit)) < 0);
        puts("PASS synthetic exact identity and 64 bit-mutation rejections; no real device UID.");
    } else {
        for (u64 uid = 0; uid < 65536; uid++)
            assert(x1_native_uid_allowed(2, uid) < 0);
        assert(x1_native_uid_allowed(2, UINT64_MAX) < 0);
        assert(x1_native_uid_allowed(2, 0x123456789abcdef0ULL) < 0);
        puts("PASS public default rejects every tested identity, including zero.");
    }
    return 0;
}
