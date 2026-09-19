/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_TYPEC_H
#define QCOM_USB4_TYPEC_H

#include <linux/types.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_tbt.h>

enum qcom_usb4_tunnel_mode {
	QCOM_USB4_TUNNEL_TBT = 0,
	QCOM_USB4_TUNNEL_USB4 = 1,
};

/* PAN wire values, deliberately distinct from enum typec_orientation. */
enum qcom_usb4_pan_orientation {
	QCOM_USB4_PAN_NORMAL = 0,
	QCOM_USB4_PAN_REVERSE = 1,
};

enum qcom_usb4_pan_cable_type {
	QCOM_USB4_PAN_PASSIVE = 0,
	QCOM_USB4_PAN_RETIMER = 1,
};

/*
 * Validated PAN fields for the observed Surface PS8830 path.
 * cable_speed is the PAN value 0/1: USB4 Gen2/Gen3 respectively;
 * TBT maps these to TBT_CABLE_USB3_PASSIVE/TBT_CABLE_10_AND_20GBPS.
 * rounded and board_retimer must be 0/1. The latter describes the board,
 * never the cable. Determine it from verified board configuration.
 * Do not truncate raw fields before calling: unavailable mode/orientation,
 * unsupported cable types and higher cable speeds must remain distinguishable.
 */
struct qcom_usb4_pan_connect {
	u8 mode;
	u8 orientation;
	u8 cable_type;
	u8 cable_speed;
	u8 rounded;
	u8 board_retimer;
};

/* All helpers leave *command unchanged on error. They perform no hardware I/O. */
int qcom_usb4_typec_connect_command(const struct qcom_usb4_pan_connect *pan,
				   u32 *command);

/*
 * Adapter for TYPEC_MODE_USB4's enter_usb_data, not a mux callback.
 * Caller must establish the payload's mode/type before passing it here.
 * Gen4, redriver, optical and active-link-training cases are unsupported.
 */
int qcom_usb4_typec_usb4_command(const struct enter_usb_data *data,
				enum typec_orientation orientation,
				u8 board_retimer, u32 *command);

/*
 * Adapter for a verified TBT modal payload. Requires standard cable-type and
 * Enter Mode flags, including TBT_CABLE_RETIMER for a retimer cable. The older
 * GLINK producer omits this flag; its ambiguous active payload is rejected.
 * Optical/redriver cables, link training and unsupported rates are rejected.
 */
int qcom_usb4_typec_tbt_command(const struct typec_thunderbolt_data *data,
			      enum typec_orientation orientation,
			      u8 board_retimer, u32 *command);

/* Only packs a verified 16-bit PMSK/preset; does not choose or apply a preset. */
int qcom_usb4_typec_preset_command(u32 preset, u32 *command);
int qcom_usb4_typec_disconnect_command(u32 *command);

#endif
