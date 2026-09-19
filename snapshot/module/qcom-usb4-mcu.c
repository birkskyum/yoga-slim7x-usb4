// SPDX-License-Identifier: GPL-2.0-only
/* Qualcomm USB4 MCU run/ready handshake, recovered from the 8480 driver. */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/unaligned.h>

#include "qcom-usb4-drom.h"
#include "qcom-usb4-fw.h"
#include "qcom-usb4-mcu.h"

#define QCOM_USB4_MCU_STATUS		0x18
#define QCOM_USB4_MCU_HOST_CONTROL	0x8018
#define QCOM_USB4_MCU_UID_HIGH		0x801c
#define QCOM_USB4_MCU_UID_LOW		0x8020
#define QCOM_USB4_MCU_READY		BIT(24)
#define QCOM_USB4_MCU_RUN			BIT(0)
#define QCOM_USB4_MCU_SHARED_MIN_SIZE	12
#define QCOM_USB4_MCU_COMMAND_RELATIVE	0xc
#define QCOM_USB4_MCU_MAX_COMMAND_TIMEOUT_US	1000000

int qcom_usb4_mcu_init(struct qcom_usb4_mcu *mcu, void __iomem *base,
		       size_t mapping_size, size_t ram_size,
		       enum qcom_usb4_mcu_layout layout)
{
	u32 ram, control, shared;

	if (!mcu)
		return -EINVAL;
	*mcu = (struct qcom_usb4_mcu) { 0 };
	if (!base || (unsigned long)base % sizeof(u32) ||
	    ram_size < QCOM_USB4_MCU_SHARED_MIN_SIZE)
		return -EINVAL;

	switch (layout) {
	case QCOM_USB4_MCU_LEGACY:
		ram = 0x13000;
		control = 0x22000;
		shared = 0x22010;
		break;
	case QCOM_USB4_MCU_V2:
		ram = 0x1e000;
		control = 0x14000;
		shared = 0x14010;
		break;
	default:
		return -EINVAL;
	}

	/* The shared register is the highest control register accessed here. */
	if (mapping_size < shared + sizeof(u32) || ram > mapping_size ||
	    ram_size > mapping_size - ram ||
	    (ram <= control && ram_size > control - ram))
		return -EINVAL;

	mcu->base = base;
	mcu->ram_size = ram_size;
	mcu->ram_offset = ram;
	mcu->control_offset = control;
	mcu->shared_offset_reg = shared;
	mcu->drom_offset = layout == QCOM_USB4_MCU_V2 ? 0xff20 : 0xdf20;
	return 0;
}

static int qcom_usb4_mcu_write_word(void *context, u32 offset, u32 value)
{
	struct qcom_usb4_mcu *mcu = context;

	writel(value, mcu->base + mcu->ram_offset + offset);
	return 0;
}

static int qcom_usb4_mcu_prepare_drom(struct qcom_usb4_mcu *mcu, u8 *drom,
				      u32 serial, u8 router)
{
	if (!mcu || !mcu->base)
		return -EINVAL;
	if (mcu->drom_offset > mcu->ram_size ||
	    QCOM_USB4_DROM_SIZE > mcu->ram_size - mcu->drom_offset)
		return -EFBIG;
	return qcom_usb4_drom_build(drom, QCOM_USB4_DROM_SIZE, serial, router);
}

int qcom_usb4_mcu_upload(struct qcom_usb4_mcu *mcu, const u8 *data, size_t size)
{
	int ret;

	if (!mcu || !mcu->base)
		return -EINVAL;
	ret = qcom_usb4_fw_validate(data, size, mcu->ram_size);
	if (ret)
		return ret;
	if (readl(mcu->base + mcu->control_offset) & QCOM_USB4_MCU_RUN)
		return -EBUSY;

	/* The observed upload sequence writes zero before copying any records. */
	writel(0, mcu->base + mcu->control_offset);
	ret = qcom_usb4_fw_upload(data, size, mcu->ram_size,
				 qcom_usb4_mcu_write_word, mcu);
	if (!ret)
		/* Flush writes before an intervening provider operation. */
		readl(mcu->base + mcu->control_offset);
	return ret;
}

static void qcom_usb4_mcu_store_drom(struct qcom_usb4_mcu *mcu, const u8 *drom)
{
	unsigned int i;

	writel(get_unaligned_le32(drom + 5), mcu->base + QCOM_USB4_MCU_UID_HIGH);
	writel(get_unaligned_le32(drom + 1), mcu->base + QCOM_USB4_MCU_UID_LOW);
	for (i = 0; i < QCOM_USB4_DROM_SIZE; i += sizeof(u32))
		qcom_usb4_mcu_write_word(mcu, mcu->drom_offset + i,
					 get_unaligned_le32(drom + i));
	readl(mcu->base + mcu->control_offset);
}

int qcom_usb4_mcu_write_drom(struct qcom_usb4_mcu *mcu, u32 serial, u8 router)
{
	u8 drom[QCOM_USB4_DROM_SIZE];
	int ret;

	ret = qcom_usb4_mcu_prepare_drom(mcu, drom, serial, router);
	if (ret)
		return ret;
	if (readl(mcu->base + mcu->control_offset) & QCOM_USB4_MCU_RUN)
		return -EBUSY;
	qcom_usb4_mcu_store_drom(mcu, drom);
	return 0;
}

int qcom_usb4_mcu_load(struct qcom_usb4_mcu *mcu, const u8 *data, size_t size,
		       u32 serial, u8 router)
{
	u8 drom[QCOM_USB4_DROM_SIZE];
	int ret;

	/* Validate the descriptor before upload can modify any hardware. */
	ret = qcom_usb4_mcu_prepare_drom(mcu, drom, serial, router);
	if (ret)
		return ret;
	ret = qcom_usb4_mcu_upload(mcu, data, size);
	if (ret)
		return ret;
	qcom_usb4_mcu_store_drom(mcu, drom);
	return 0;
}

static void qcom_usb4_mcu_update(struct qcom_usb4_mcu *mcu, u32 offset,
				u32 mask, u32 value)
{
	u32 reg = readl(mcu->base + offset);

	writel((reg & ~mask) | (value & mask), mcu->base + offset);
}

void qcom_usb4_mcu_stop(struct qcom_usb4_mcu *mcu)
{
	if (!mcu || !mcu->base)
		return;

	qcom_usb4_mcu_update(mcu, mcu->control_offset, QCOM_USB4_MCU_RUN, 0);
	qcom_usb4_mcu_update(mcu, QCOM_USB4_MCU_HOST_CONTROL,
			     QCOM_USB4_MCU_READY, 0);
	/* Complete posted writes before the caller releases power or clocks. */
	readl(mcu->base + QCOM_USB4_MCU_HOST_CONTROL);
}

int qcom_usb4_mcu_start(struct qcom_usb4_mcu *mcu, u32 *shared_offset)
{
	u32 status, offset;
	int ret;

	if (!shared_offset)
		return -EINVAL;
	*shared_offset = 0;
	if (!mcu || !mcu->base)
		return -EINVAL;
	if (readl(mcu->base + mcu->control_offset) & QCOM_USB4_MCU_RUN)
		return -EBUSY;

	qcom_usb4_mcu_update(mcu, QCOM_USB4_MCU_HOST_CONTROL,
			     QCOM_USB4_MCU_READY, 0);
	qcom_usb4_mcu_update(mcu, mcu->control_offset,
			     QCOM_USB4_MCU_RUN, QCOM_USB4_MCU_RUN);
	ret = readl_poll_timeout(mcu->base + QCOM_USB4_MCU_STATUS, status,
				 status & QCOM_USB4_MCU_READY, 5000, 1000000);
	if (ret)
		goto stop;

	offset = readl(mcu->base + mcu->shared_offset_reg);
	if (offset % sizeof(u32) ||
	    offset > mcu->ram_size - QCOM_USB4_MCU_SHARED_MIN_SIZE) {
		ret = -EPROTO;
		goto stop;
	}

	*shared_offset = offset;
	return 0;

stop:
	qcom_usb4_mcu_stop(mcu);
	return ret;
}

int qcom_usb4_mcu_command(struct qcom_usb4_mcu *mcu, u32 command,
			 unsigned int timeout_us)
{
	void __iomem *mailbox;
	u32 value;

	if (!mcu || !mcu->base || !command || !timeout_us ||
	    timeout_us > QCOM_USB4_MCU_MAX_COMMAND_TIMEOUT_US)
		return -EINVAL;
	if (!(readl(mcu->base + mcu->control_offset) & QCOM_USB4_MCU_RUN))
		return -EHOSTDOWN;
	if (!(readl(mcu->base + QCOM_USB4_MCU_STATUS) & QCOM_USB4_MCU_READY))
		return -EAGAIN;

	/* Both layouts place the mailbox before the validated shared register. */
	mailbox = mcu->base + mcu->control_offset + QCOM_USB4_MCU_COMMAND_RELATIVE;
	if (readl(mailbox))
		return -EBUSY;

	writel(command, mailbox);
	return readl_poll_timeout(mailbox, value, !value, 1, timeout_us);
}
