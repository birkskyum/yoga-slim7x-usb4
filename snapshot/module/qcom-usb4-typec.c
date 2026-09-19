// SPDX-License-Identifier: GPL-2.0-only
/*
 * Original encoder of the Surface Qualcomm USB4 MCU mailbox contract.
 * Evidence: TYPEC-MAILBOX.md, UCSI 0xac18..0xace4 and filter 0x14720.
 * Command completion, power sequencing and link readiness belong to callers;
 * encoding a word does not establish that the MCU can accept it.
 */
#include <linux/errno.h>
#include <linux/usb/pd.h>

#include "qcom-usb4-typec.h"

int qcom_usb4_typec_connect_command(const struct qcom_usb4_pan_connect *pan,
				   u32 *command)
{
	u32 cable_class;

	if (!pan || !command)
		return -EINVAL;
	if (pan->mode > QCOM_USB4_TUNNEL_USB4 ||
	    pan->orientation > QCOM_USB4_PAN_REVERSE ||
	    pan->rounded > 1 || pan->board_retimer > 1)
		return -EINVAL;
	if (pan->cable_type > QCOM_USB4_PAN_RETIMER || pan->cable_speed > 1)
		return -EOPNOTSUPP;

	cable_class = 0;
	if (pan->cable_type == QCOM_USB4_PAN_RETIMER) {
		cable_class = 1;
		if (pan->mode == QCOM_USB4_TUNNEL_TBT && !pan->rounded)
			cable_class = 3;
	}

	*command = 1U | (u32)pan->mode << 8 | (u32)pan->orientation << 9 |
		   (u32)pan->board_retimer << 10 | cable_class << 11 |
		   (u32)pan->cable_speed << 13;
	return 0;
}

int qcom_usb4_typec_usb4_command(const struct enter_usb_data *data,
				enum typec_orientation orientation,
				u8 board_retimer, u32 *command)
{
	struct qcom_usb4_pan_connect pan = {
		.mode = QCOM_USB4_TUNNEL_USB4,
		.board_retimer = board_retimer,
	};
	u32 speed;

	if (!data || !command)
		return -EINVAL;
	if (((data->eudo & EUDO_USB_MODE_MASK) >> EUDO_USB_MODE_SHIFT) !=
	    EUDO_USB_MODE_USB4)
		return -EINVAL;
	if (data->active_link_training)
		return -EOPNOTSUPP;

	switch (orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		pan.orientation = QCOM_USB4_PAN_NORMAL;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		pan.orientation = QCOM_USB4_PAN_REVERSE;
		break;
	default:
		return -EINVAL;
	}

	speed = (data->eudo & EUDO_CABLE_SPEED_MASK) >> EUDO_CABLE_SPEED_SHIFT;
	switch (speed) {
	case EUDO_CABLE_SPEED_USB4_GEN2:
		pan.cable_speed = 0;
		break;
	case EUDO_CABLE_SPEED_USB4_GEN3:
		pan.cable_speed = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}
	pan.cable_type = (data->eudo & EUDO_CABLE_TYPE_MASK) >> EUDO_CABLE_TYPE_SHIFT;

	return qcom_usb4_typec_connect_command(&pan, command);
}

int qcom_usb4_typec_tbt_command(const struct typec_thunderbolt_data *data,
			      enum typec_orientation orientation,
			      u8 board_retimer, u32 *command)
{
	struct qcom_usb4_pan_connect pan = {
		.mode = QCOM_USB4_TUNNEL_TBT,
		.board_retimer = board_retimer,
	};
	u32 cable, enter, speed, rounded;
	bool active, retimer;

	if (!data || !command)
		return -EINVAL;
	cable = data->cable_mode;
	enter = data->enter_vdo;
	if (!(data->device_mode & TBT_MODE) || !(cable & TBT_MODE) ||
	    !(enter & TBT_MODE))
		return -EINVAL;
	if (TBT_ADAPTER(data->device_mode) != TBT_ADAPTER_TBT3)
		return -EOPNOTSUPP;
	if ((cable | enter) & (TBT_CABLE_OPTICAL | TBT_CABLE_LINK_TRAINING))
		return -EOPNOTSUPP;

	active = !!(cable & TBT_CABLE_ACTIVE_PASSIVE);
	retimer = !!(cable & TBT_CABLE_RETIMER);
	if (active != !!(enter & TBT_ENTER_MODE_ACTIVE_CABLE) ||
	    retimer != !!(enter & TBT_CABLE_RETIMER) || (!active && retimer))
		return -EINVAL;
	/* Redriver or old GLINK active-only data must not become a retimer. */
	if (active && !retimer)
		return -EOPNOTSUPP;
	pan.cable_type = retimer ? QCOM_USB4_PAN_RETIMER : QCOM_USB4_PAN_PASSIVE;

	speed = TBT_CABLE_SPEED(cable);
	if (speed != TBT_CABLE_SPEED(enter))
		return -EINVAL;
	switch (speed) {
	case TBT_CABLE_USB3_PASSIVE:
		pan.cable_speed = 0;
		break;
	case TBT_CABLE_10_AND_20GBPS:
		pan.cable_speed = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}
	rounded = TBT_CABLE_ROUNDED_SUPPORT(cable);
	if (rounded != TBT_CABLE_ROUNDED_SUPPORT(enter))
		return -EINVAL;
	if (rounded > 1)
		return -EOPNOTSUPP;
	pan.rounded = rounded;

	switch (orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		pan.orientation = QCOM_USB4_PAN_NORMAL;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		pan.orientation = QCOM_USB4_PAN_REVERSE;
		break;
	default:
		return -EINVAL;
	}

	return qcom_usb4_typec_connect_command(&pan, command);
}

int qcom_usb4_typec_preset_command(u32 preset, u32 *command)
{
	if (!command || preset > 0xffff)
		return -EINVAL;
	*command = 2U | preset << 8;
	return 0;
}

int qcom_usb4_typec_disconnect_command(u32 *command)
{
	if (!command)
		return -EINVAL;
	*command = 6;
	return 0;
}
