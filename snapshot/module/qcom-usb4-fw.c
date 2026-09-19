// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm USB4 MCU firmware record decoding.
 *
 * Each record is a little-endian u32 byte offset, a little-endian u32 word
 * count, then that many little-endian u32 words. Records continue to EOF.
 * This format was verified against the Surface 8480 driver package; there
 * is no inferred file header, signature, or checksum field.
 *
 * This helper does not select firmware, reset the MCU, or access hardware.
 * The caller must select the correct image, establish the actual RAM size,
 * hold the MCU in the required loading state, and implement ordered writes.
 */

#include <linux/errno.h>
#include <linux/unaligned.h>

#include "qcom-usb4-fw.h"

int qcom_usb4_fw_validate(const u8 *data, size_t size, size_t ram_size)
{
	size_t pos = 0;

	if (!data || !size)
		return -EINVAL;

	while (pos < size) {
		size_t bytes;
		u32 offset, words;

		if (size - pos < 8)
			return -EINVAL;

		offset = get_unaligned_le32(data + pos);
		words = get_unaligned_le32(data + pos + 4);
		pos += 8;

		/* Bound multiplication and record traversal before doing either. */
		if (words > (size - pos) / sizeof(u32))
			return -EINVAL;
		bytes = (size_t)words * sizeof(u32);

		if (offset % sizeof(u32))
			return -EINVAL;

		/* Prevent the 32-bit destination arithmetic used by the transport. */
		if (bytes > (u32)-1 - offset)
			return -EOVERFLOW;

		if (offset > ram_size || bytes > ram_size - offset)
			return -EFBIG;

		pos += bytes;
	}

	return 0;
}

int qcom_usb4_fw_upload(const u8 *data, size_t size, size_t ram_size,
		       qcom_usb4_fw_write_word_t write_word, void *context)
{
	size_t pos = 0;
	int ret;

	if (!write_word)
		return -EINVAL;

	/* A bad later record must not leave a partially uploaded image. */
	ret = qcom_usb4_fw_validate(data, size, ram_size);
	if (ret)
		return ret;

	while (pos < size) {
		u32 offset = get_unaligned_le32(data + pos);
		u32 words = get_unaligned_le32(data + pos + 4);

		pos += 8;
		while (words--) {
			ret = write_word(context, offset, get_unaligned_le32(data + pos));
			if (ret)
				return ret;
			offset += sizeof(u32);
			pos += sizeof(u32);
		}
	}

	return 0;
}
