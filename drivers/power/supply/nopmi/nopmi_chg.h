/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nopmi_chg - NOPMI charger parent driver
 *
 * This is the top of the nopmi charger driver stack. It is the SOLE
 * owner of every hardware resource handle used by this driver and all
 * its sub-modules.
 *
 */

#if !defined(__NOPMI_CHG_H__)
#define __NOPMI_CHG_H__

#include "max77729_charger.h"
#include "nopmi_chg_jeita.h"

#define STEP_TABLE_MAX	2
#define STEP_DOWN_CURR_MA	100
#define CV_BATT_VOLT_HYSTERESIS	20
#define CC_CV_STEP_VOTER	"CC_CV_STEP_VOTER"

struct step_config {
	int volt_lim;
	int curr_lim;
};

struct nopmi_dt_props {
	int usb_icl_ua;
	int chg_inhibit_thr_mv;
	bool no_battery;
	bool hvdcp_disable;
	bool hvdcp_autonomous;
	bool adc_based_aicl;
	int sec_charger_config;
	int auto_recharge_soc;
	int auto_recharge_vbat_mv;
	int wd_bark_time;
	int wd_snarl_time_cfg;
	int batt_profile_fcc_ua;
	int batt_profile_fv_uv;
};

struct nopmi_chg {
	struct platform_device *pdev;
	struct device *dev;

	struct tcpc_device *tcpc_dev;
	struct notifier_block pd_nb;
	enum power_supply_typec_mode typec_mode;
	int cc_orientation;

	struct power_supply *bms_psy;
	struct power_supply *bbc_psy;
	struct power_supply *cp_psy;
	struct power_supply *batt_psy;
	struct power_supply *usb_psy;

	struct votable *fcc_votable;
	struct votable *fv_votable;
	struct votable *usb_icl_votable;
	struct votable *chg_dis_votable;
	struct votable *chgctrl_votable;

	struct nopmi_dt_props dt;
	struct nopmi_chg_jeita_st *jeita_st;

	struct workqueue_struct *usb_online_wq;
	struct delayed_work usb_online_work;
	struct delayed_work nopmi_chg_work;
	struct delayed_work cv_step_monitor_work;

	int pd_active;
	int real_type;
	int pd_min_vol;
	int pd_max_vol;
	int pd_cur_max;
	int pd_usb_suspend;
	int pd_in_hard_reset;
	int usb_online;
	int mtbf_cur;

	s32 batt_temp;
	s32 batt_volt;
	int batt_health;

	int input_suspend;
	int last_batt_status;
	int last_usb_online;
	u8 is_awake;

	u32 *thermal_mitigation;
	int thermal_levels;
	int system_temp_level;
	bool last_thermal_icl_disabled;
	bool last_thermal_icl_valid;

	s32 last_vote_fcc;
	u32 cv_step_count[STEP_TABLE_MAX];
};

#endif /* __NOPMI_CHG_H__ */
