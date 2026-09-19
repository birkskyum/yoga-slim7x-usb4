// SPDX-License-Identifier: GPL-2.0-only
/*
 * X1 reference DROM variant of Jim Martin's qcom-usb4-drom helper.
 * Preserve the imported helper unchanged; the two variant-specific fields
 * below come from the pinned 8380 constructor audited in check_x1_drom.py.
 * No partial descriptor is sent to hardware by this memory-only operation.
 */
#include <linux/crc32.h>
#include <linux/unaligned.h>
#include "qcom-usb4-drom.h"
#include "qcom-usb4-x1-drom.h"

int qcom_usb4_x1_drom_build(u8 *data, size_t size, u32 serial, u8 router)
{
    int ret = qcom_usb4_drom_build(data, size, serial, router);

    if (ret)
        return ret;
    data[0x4d] = 'A';
    data[0x59] = 0x33;
    data[0x5a] = 0x28;
    put_unaligned_le32(~crc32c(~0U, data + 13, QCOM_USB4_DROM_SIZE - 13), data + 9);
    return 0;
}
