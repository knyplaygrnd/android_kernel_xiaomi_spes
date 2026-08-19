/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Richtek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __LINUX_RT_TCPC_H__
#define __LINUX_RT_TCPC_H__

#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/pm_wakeup.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/sched.h>

#ifdef CONFIG_DUAL_ROLE_USB_INTF
#include "class-dual-role.h"
#endif /* CONFIG_DUAL_ROLE_USB_INTF */

#include "tcpci_core.h"

#ifdef CONFIG_PD_DBG_INFO
#include "pd_dbg_info.h"
#endif /* CONFIG_PD_DBG_INFO */

#ifdef CONFIG_USB_POWER_DELIVERY
#include "pd_core.h"
#endif /* CONFIG_USB_POWER_DELIVERY */

#define PE_STATE_FULL_NAME	0

#define TCPC_LOW_RP_DUTY	(100)	/* 10 % */
#define TCPC_NORMAL_RP_DUTY	(330)	/* 33 % */

/* provide to TCPC interface */
int tcpci_report_usb_port_changed(struct tcpc_device *tcpc);
int tcpci_report_power_control(struct tcpc_device *tcpc, bool en);
int tcpc_typec_init(struct tcpc_device *tcpc, u8 typec_role);
void tcpc_typec_deinit(struct tcpc_device *tcpc);
int tcpc_dual_role_phy_init(struct tcpc_device *tcpc);
int tcpc_device_irq_enable(struct tcpc_device *tcpc);
int tcpci_late_sync_enable(struct tcpc_device *tcpc);

struct tcpc_device *tcpc_device_register(struct device *parent,
					 struct tcpc_desc *tcpc_desc,
					 struct tcpc_ops *ops, void *drv_data);
void tcpc_device_unregister(struct device *dev, struct tcpc_device *tcpc);
int tcpc_schedule_init_work(struct tcpc_device *tcpc);

void *tcpc_get_dev_data(struct tcpc_device *tcpc);
void tcpci_lock_typec(struct tcpc_device *tcpc);
void tcpci_unlock_typec(struct tcpc_device *tcpc);
int tcpci_alert(struct tcpc_device *tcpc);

void tcpci_vbus_level_init(struct tcpc_device *tcpc, u16 power_status);

static inline int tcpci_check_vbus_valid(struct tcpc_device *tcpc)
{
	return tcpc->vbus_level >= TCPC_VBUS_VALID;
}

int tcpci_check_vbus_valid_from_ic(struct tcpc_device *tcpc);
bool tcpci_check_vsafe0v(struct tcpc_device *tcpc);
int tcpci_alert_status_clear(struct tcpc_device *tcpc, u32 mask);
int tcpci_fault_status_clear(struct tcpc_device *tcpc, u8 status);
int tcpci_set_alert_mask(struct tcpc_device *tcpc, u32 mask);
int tcpci_get_alert_mask(struct tcpc_device *tcpc, u32 *mask);
int tcpci_get_alert_status(struct tcpc_device *tcpc, u32 *alert);
int tcpci_get_fault_status(struct tcpc_device *tcpc, u8 *fault);
int tcpci_get_power_status(struct tcpc_device *tcpc, u16 *pw_status);
int tcpci_init(struct tcpc_device *tcpc, bool sw_reset);
int tcpci_init_alert_mask(struct tcpc_device *tcpc);

int tcpci_get_cc(struct tcpc_device *tcpc);
int tcpci_set_cc(struct tcpc_device *tcpc, int pull);
int tcpci_set_polarity(struct tcpc_device *tcpc, int polarity);
int tcpci_set_low_rp_duty(struct tcpc_device *tcpc, bool low_rp);
int tcpci_set_vconn(struct tcpc_device *tcpc, int enable);

int tcpci_is_low_power_mode(struct tcpc_device *tcpc);
int tcpci_set_low_power_mode(struct tcpc_device *tcpc, bool en, int pull);
int tcpci_idle_poll_ctrl(struct tcpc_device *tcpc, bool en, bool lock);
int tcpci_set_watchdog(struct tcpc_device *tcpc, bool en);
int tcpci_alert_vendor_defined_handler(struct tcpc_device *tcpc);
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
int tcpci_is_vsafe0v(struct tcpc_device *tcpc);
#endif /* CONFIG_TCPC_VSAFE0V_DETECT_IC */

#ifdef CONFIG_WATER_DETECTION
int tcpci_is_water_detected(struct tcpc_device *tcpc);
int tcpci_set_water_protection(struct tcpc_device *tcpc, bool en);
int tcpci_set_usbid_polling(struct tcpc_device *tcpc, bool en);
int tcpci_notify_wd_status(struct tcpc_device *tcpc, bool water_detected);
#endif /* CONFIG_WATER_DETECTION */

#ifdef CONFIG_CABLE_TYPE_DETECTION
int tcpci_notify_cable_type(struct tcpc_device *tcpc);
#endif /* CONFIG_CABLE_TYPE_DETECTION */

#ifdef CONFIG_USB_POWER_DELIVERY

int tcpci_set_msg_header(struct tcpc_device *tcpc,
			 u8 power_role, u8 data_role);

int tcpci_set_rx_enable(struct tcpc_device *tcpc, u8 enable);

int tcpci_protocol_reset(struct tcpc_device *tcpc);

int tcpci_get_message(struct tcpc_device *tcpc,
		      u32 *payload, u16 *head, enum tcpm_transmit_type *type);

int tcpci_transmit(struct tcpc_device *tcpc,
		   enum tcpm_transmit_type type, u16 header, const u32 *data);

int tcpci_set_bist_test_mode(struct tcpc_device *tcpc, bool en);

int tcpci_set_bist_carrier_mode(struct tcpc_device *tcpc, u8 pattern);

#ifdef CONFIG_USB_PD_RETRY_CRC_DISCARD
int tcpci_retransmit(struct tcpc_device *tcpc);
#endif	/* CONFIG_USB_PD_RETRY_CRC_DISCARD */
#endif	/* CONFIG_USB_POWER_DELIVERY */

int tcpci_notify_typec_state(struct tcpc_device *tcpc);

int tcpci_notify_role_swap(struct tcpc_device *tcpc, u8 event, u8 role);
int tcpci_notify_pd_state(struct tcpc_device *tcpc, u8 connect);

int tcpci_set_intrst(struct tcpc_device *tcpc, bool en);
int tcpci_enable_watchdog(struct tcpc_device *tcpc, bool en);

int tcpci_source_vbus(struct tcpc_device *tcpc, u8 type, int mv, int ma);
int tcpci_sink_vbus(struct tcpc_device *tcpc, u8 type, int mv, int ma);
int tcpci_disable_vbus_control(struct tcpc_device *tcpc);
int tcpci_notify_attachwait_state(struct tcpc_device *tcpc, bool as_sink);
int tcpci_enable_auto_discharge(struct tcpc_device *tcpc, bool en);
int tcpci_enable_force_discharge(struct tcpc_device *tcpc, bool en, int mv);

#ifdef CONFIG_USB_POWER_DELIVERY

int tcpci_notify_hard_reset_state(struct tcpc_device *tcpc, u8 state);

int tcpci_enter_mode(struct tcpc_device *tcpc,
		     u16 svid, u8 ops, u32 mode);
int tcpci_exit_mode(struct tcpc_device *tcpc, u16 svid);

#ifdef CONFIG_USB_PD_ALT_MODE
int tcpci_report_hpd_state(struct tcpc_device *tcpc, u32 dp_status);
int tcpci_dp_status_update(struct tcpc_device *tcpc, u32 dp_status);
int tcpci_dp_configure(struct tcpc_device *tcpc, u32 dp_config);
int tcpci_dp_attention(struct tcpc_device *tcpc, u32 dp_status);

int tcpci_dp_notify_status_update_done(struct tcpc_device *tcpc, u32 dp_status, bool ack);

int tcpci_dp_notify_config_start(struct tcpc_device *tcpc);
int tcpci_dp_notify_config_done(struct tcpc_device *tcpc,
				u32 local_cfg, u32 remote_cfg, bool ack);
#endif	/* CONFIG_USB_PD_ALT_MODE */

#ifdef CONFIG_USB_PD_CUSTOM_VDM
int tcpci_notify_uvdm(struct tcpc_device *tcpc, bool ack);
#endif	/* CONFIG_USB_PD_CUSTOM_VDM */

#ifdef CONFIG_USB_PD_ALT_MODE_RTDC
int tcpci_dc_notify_en_unlock(struct tcpc_device *tcpc);
#endif	/* CONFIG_USB_PD_ALT_MODE_RTDC */

#ifdef CONFIG_USB_PD_REV30

#ifdef CONFIG_USB_PD_REV30_ALERT_REMOTE
int tcpci_notify_alert(struct tcpc_device *tcpc, u32 ado);
#endif	/* CONFIG_USB_PD_REV30_ALERT_REMOTE */

#ifdef CONFIG_USB_PD_REV30_STATUS_REMOTE
int tcpci_notify_status(struct tcpc_device *tcpc, struct pd_status *sdb);
#endif	/* CONFIG_USB_PD_REV30_STATUS_REMOTE */

#ifdef CONFIG_USB_PD_REV30_BAT_INFO
int tcpci_notify_request_bat_info(struct tcpc_device *tcpc, enum pd_battery_reference ref);
#endif	/* CONFIG_USB_PD_REV30_BAT_INFO */
#endif	/* CONFIG_USB_PD_REV30 */

#endif	/* CONFIG_USB_POWER_DELIVERY */

#endif /* __LINUX_RT_TCPC_H__ */
