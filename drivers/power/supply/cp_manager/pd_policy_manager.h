/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * usb_pd_policy_manager
 *
 *  Created on: Mar 27, 2017
 *      Author: a0220433
 */
#ifndef __SRC_PDLIB_USB_PD_POLICY_MANAGER_H__
#define __SRC_PDLIB_USB_PD_POLICY_MANAGER_H__

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <linux/pmic-voter.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/usb/typec/maxim/max77729_usbc.h>
#include "inc/tcpm.h"
#include "inc/tcpci.h"
#include "nopmi_chg_common.h"
#include "bq2589x_charger.h"

#define BQ_TAPER_HYS_MV		10
#define TAPER_VOL_HYS		80

#define NON_FFC_BQ_TAPER_HYS_MV		50
#define BQ_TAPER_DECREASE_STEP_MA	200

#define QUICK_RAISE_VOLT_INTERVAL_S	(10 * MSEC_PER_SEC)

#define BQ_TAPER_FCC_VOTER	"BQ_TAPER_FCC_VOTER"
#define JEITA_VOTER		"JEITA_VOTER"

#define MAX_THERMAL_LEVEL_FOR_CP 10

enum pm_state {
	PD_PM_STATE_ENTRY,
	PD_PM_STATE_FC2_ENTRY,
	PD_PM_STATE_FC2_ENTRY_1,
	PD_PM_STATE_FC2_ENTRY_2,
	PD_PM_STATE_FC2_ENTRY_3,
	PD_PM_STATE_FC2_TUNE,
	PD_PM_STATE_FC2_EXIT,
};

/*
 * add hysteresis for warm threshold to avoid flash
 * charge and normal charge switch frequently at
 * the warm threshold
 */
#define JEITA_HYSTERESIS	20

#define BAT_OVP_FAULT_SHIFT		0
#define BAT_OCP_FAULT_SHIFT		1
#define BUS_OVP_FAULT_SHIFT		2
#define BUS_OCP_FAULT_SHIFT		3
#define BAT_THERM_FAULT_SHIFT	4
#define BUS_THERM_FAULT_SHIFT	5
#define DIE_THERM_FAULT_SHIFT	6

#define BAT_OVP_FAULT_MASK		BIT(BAT_OVP_FAULT_SHIFT)
#define BAT_OCP_FAULT_MASK		BIT(BAT_OCP_FAULT_SHIFT)
#define BUS_OVP_FAULT_MASK		BIT(BUS_OVP_FAULT_SHIFT)
#define BUS_OCP_FAULT_MASK		BIT(BUS_OCP_FAULT_SHIFT)
#define BAT_THERM_FAULT_MASK		BIT(BAT_THERM_FAULT_SHIFT)
#define BUS_THERM_FAULT_MASK		BIT(BUS_THERM_FAULT_SHIFT)
#define DIE_THERM_FAULT_MASK		BIT(DIE_THERM_FAULT_SHIFT)

#define BAT_OVP_ALARM_SHIFT		0
#define BAT_OCP_ALARM_SHIFT		1
#define BUS_OVP_ALARM_SHIFT		2
#define BUS_OCP_ALARM_SHIFT		3
#define BAT_THERM_ALARM_SHIFT		4
#define BUS_THERM_ALARM_SHIFT		5
#define DIE_THERM_ALARM_SHIFT		6
#define BAT_UCP_ALARM_SHIFT		7

#define BAT_OVP_ALARM_MASK		BIT(BAT_OVP_ALARM_SHIFT)
#define BAT_OCP_ALARM_MASK		BIT(BAT_OCP_ALARM_SHIFT)
#define BUS_OVP_ALARM_MASK		BIT(BUS_OVP_ALARM_SHIFT)
#define BUS_OCP_ALARM_MASK		BIT(BUS_OCP_ALARM_SHIFT)
#define BAT_THERM_ALARM_MASK		BIT(BAT_THERM_ALARM_SHIFT)
#define BUS_THERM_ALARM_MASK		BIT(BUS_THERM_ALARM_SHIFT)
#define DIE_THERM_ALARM_MASK		BIT(DIE_THERM_ALARM_SHIFT)
#define BAT_UCP_ALARM_MASK		BIT(BAT_UCP_ALARM_SHIFT)

#define VBAT_REG_STATUS_SHIFT	0
#define IBAT_REG_STATUS_SHIFT	1

#define VBAT_REG_STATUS_MASK		BIT(VBAT_REG_STATUS_SHIFT)
#define IBAT_REG_STATUS_MASK		BIT(VBAT_REG_STATUS_SHIFT)

struct sw_device {
	bool charge_enabled;
};

struct cp_device {
	bool charge_enabled;

	bool batt_pres;
	bool vbus_pres;

	/* alarm/fault status */
	bool bat_ovp_fault;
	bool bat_ocp_fault;
	bool bus_ovp_fault;
	bool bus_ocp_fault;

	bool bat_ovp_alarm;
	bool bat_ocp_alarm;
	bool bus_ovp_alarm;
	bool bus_ocp_alarm;

	bool bat_ucp_alarm;

	bool bat_therm_alarm;
	bool bus_therm_alarm;
	bool die_therm_alarm;

	bool bat_therm_fault;
	bool bus_therm_fault;
	bool die_therm_fault;

	bool therm_shutdown_flag;
	bool therm_shutdown_stat;

	bool vbat_reg;
	bool ibat_reg;

	int vout_volt;
	int vbat_volt;
	int vbus_volt;
	int ibat_curr;
	int ibat_curr_cp;
	int ibat_curr_sw;
	int ibus_curr;
	int ibus_curr_cp;
	int ibus_curr_sw;

	int vbus_error_low;
	int vbus_error_high;

	int bat_temp;
	int bus_temp;
	int die_temp;
};

#define PDPM_RL_SLOTS		64
#define PDPM_RL_INTERVAL	(3 * HZ)
#define PDPM_RL_BURST		1

#ifdef __COUNTER__
#define _PDPM_ID()	__COUNTER__
#else
#define _PDPM_ID()	__LINE__
#endif

struct usbpd_pm {
	struct device *dev;
	struct platform_device *pdev;

	struct tcpc_device *tcpc;
	struct notifier_block tcp_nb;
	struct charger_device *sw_chg;
	bool is_pps_en_unlock;
	int hrst_cnt;
	POWER_LIST *pdo;

	enum pm_state state;

	struct cp_device cp;
	struct cp_device cp_sec;

	struct sw_device sw;

	bool cp_sec_stopped;
	bool pd_active;
	bool pps_supported;

	int request_voltage;
	int request_current;
	int apdo_max_volt;
	int apdo_max_curr;
	int apdo_selected_pdo;

	int pd_cv;
	int adapter_voltage;
	int adapter_current;
	int adapter_ptf;
	bool adapter_omf;

	ktime_t entry_bq_cv_time;
	struct delayed_work pm_work;

	struct notifier_block nb;

	bool psy_change_running;
	struct work_struct cp_psy_change_work;
	struct work_struct usb_psy_change_work;
	spinlock_t psy_change_lock;

	struct power_supply *cp_psy;
	struct power_supply *cp_sec_psy;
	struct power_supply *batt_psy;
	struct power_supply *usb_psy;
	struct power_supply *bms_psy;
	struct votable *fcc_votable;
	struct votable *fv_votable;

	/* dtsi properties */
	int bat_volt_max;
	int ffc_bat_volt_max;
	int bat_curr_max;
	int bus_volt_max;
	int bus_curr_max;
	int bus_curr_compensate;
	int batt_temp;
	bool cp_sec_enable;

	/* jeita or thermal related */
	bool jeita_triggered;
	bool is_temp_out_fc2_range;
	bool bq_cool_warm_done;

	int ibus_limit;
	int fcc_limit;
	int tune_vbus_retry;
	bool stop_sw;
	bool recover;
	int fc2_taper_timer;
	int ibus_lmt_change_timer;

	struct ratelimit_state rl_slots[PDPM_RL_SLOTS];
};

struct pdpm_config {
	int bat_volt_lp_lmt; /*bat volt loop limit*/
	int bat_curr_lp_lmt;
	int bus_volt_lp_lmt;
	int bus_curr_lp_lmt;

	int fc2_taper_current;
	int fc2_steps;

	int min_adapter_volt_required;
	int min_adapter_curr_required;

	int min_vbat_for_cp;

	bool cp_sec_enable;
	bool fc2_disable_sw;
};

extern POWER_LIST *usbpd_fetch_pdo(void);

int usbpd_select_pdo_maxim(int pdo, int mv, int ma);

#endif
