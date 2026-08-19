/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * Copyright (C) 2018 SiliconMitus
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef __SM5602_FG_HEADER__
#define __SM5602_FG_HEADER__

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/regmap.h>
#include <linux/ratelimit.h>
#include <linux/pmic-voter.h>

//#define ENABLE_MIX_COMP
//#define ENABLE_VLCM_MODE
#define ENABLE_TEMBASE_ZDSCON
#define ENABLE_IOCV_ADJ
#define ENABLE_NTC_COMPENSATION
#define ENABLE_TEMP_AVG
#define ENABLE_CURRENT_AVG

#define FG_INIT_MARK			0xA000

#define FG_PARAM_UNLOCK_CODE		0x3700
#define FG_PARAM_LOCK_CODE		0x0000
#define FG_TABLE_LEN			0x18
#define FG_ADD_TABLE_LEN		0x8
#define FG_INIT_B_LEN			0x7

/* CNTL register bit-fields */
#define ENABLE_EN_TEMP_IN		0x0200
#define ENABLE_EN_TEMP_EX		0x0400
#define ENABLE_EN_BATT_DET		0x0800
#define ENABLE_IOCV_MAN_MODE		0x1000
#define ENABLE_FORCED_SLEEP		0x2000
#define ENABLE_SLEEPMODE_EN		0x4000
#define ENABLE_SHUTDOWN			0x8000

#define FG_REG_SOC_CYCLE		0x0B
#define FG_REG_SOC_CYCLE_CFG		0x15
#define FG_REG_ALPHA			0x20
#define FG_REG_BETA			0x21
#define FG_REG_RS			0x24
#define FG_REG_RS_1			0x25
#define FG_REG_RS_2			0x26
#define FG_REG_RS_3			0x27
#define FG_REG_RS_0			0x29
#define FG_REG_END_V_IDX		0x2F
#define FG_REG_START_LB_V		0x30
#define FG_REG_START_CB_V		0x38
#define FG_REG_START_LB_I		0x40
#define FG_REG_START_CB_I		0x48
#define FG_REG_VOLT_CAL			0x70
#define FG_REG_CURR_IN_OFFSET		0x75
#define FG_REG_CURR_IN_SLOPE		0x76
#define FG_REG_RMC			0x84
#define FG_REG_SRADDR			0x8C
#define FG_REG_SRDATA			0x8D
#define FG_REG_SWADDR			0x8E
#define FG_REG_SWDATA			0x8F
#define FG_REG_AGING_CTRL		0x9C

#define FG_TEMP_TABLE_CNT_MAX		0x65
#define FG_PARAM_VERION			0x1E

#define INIT_CHECK_MASK			0x0010
#define DISABLE_RE_INIT			0x0010

#define EX_TEMP_MIN			(-20)
#define EX_TEMP_MAX			80

#define SM_RAW_SOC_FULL			990	/* 99.0% — tracking target */
#define SM_RECHARGE_SOC			971	/* 97.1% — recharge trigger */

#define MONITOR_WORK_10S		10
#define MONITOR_WORK_5S			5
#define MONITOR_WORK_1S			1

#define BMS_FG_VERIFY			"BMS_FG_VERIFY"
#define BMS_FC_VOTER			"BMS_FC_VOTER"

#ifdef ENABLE_TEMBASE_ZDSCON
#define ZDSCON_ACT_TEMP_GAP		15
#define T_GAP_DENOM			5
#define HMINMAN_T_VALUE_FACT		125
#define I_GAP_DENOM			1000
#define HMINMAN_I_VALUE_FACT		150
#endif

#ifdef ENABLE_IOCV_ADJ
#define IOCV_MAX_ADJ_LEVEL		0x1F33
#define IOCV_MIN_ADJ_LEVEL		0x1D70
#define IOCI_MAX_ADJ_LEVEL		0x1000
#define IOCI_MIN_ADJ_LEVEL		0xCC
#define IOCV_I_SLOPE			100
#define IOCV_I_OFFSET			0
#endif

#ifdef ENABLE_NTC_COMPENSATION
#define OVERHEAT_TH_DEG			50
#define COLD_TH_DEG			0
#endif

#define BATT_MA_AVG_SAMPLES		5
#define BATT_TEMP_AVG_SAMPLES		5

#define SM_RL_SLOTS	16
#define SM_RL_INTERVAL	(5 * HZ)
#define SM_RL_BURST	1

#ifdef __COUNTER__
#define _SM_ID()	__COUNTER__
#else
#define _SM_ID()	__LINE__
#endif

enum sm_fg_reg_idx {
	SM_FG_REG_DEVICE_ID = 0,
	SM_FG_REG_CNTL,
	SM_FG_REG_INT,
	SM_FG_REG_INT_MASK,
	SM_FG_REG_STATUS,
	SM_FG_REG_SOC,
	SM_FG_REG_OCV,
	SM_FG_REG_VOLTAGE,
	SM_FG_REG_CURRENT,
	SM_FG_REG_TEMPERATURE_IN,
	SM_FG_REG_TEMPERATURE_EX,
	SM_FG_REG_V_L_ALARM,
	SM_FG_REG_V_H_ALARM,
	SM_FG_REG_A_H_ALARM,
	SM_FG_REG_T_IN_H_ALARM,
	SM_FG_REG_SOC_L_ALARM,
	SM_FG_REG_FG_OP_STATUS,
	SM_FG_REG_TOPOFFSOC,
	SM_FG_REG_PARAM_CTRL,
	SM_FG_REG_SHUTDOWN,
	SM_FG_REG_VIT_PERIOD,
	SM_FG_REG_CURRENT_RATE,
	SM_FG_REG_BAT_CAP,
	SM_FG_REG_CURR_OFFSET,
	SM_FG_REG_CURR_SLOPE,
	SM_FG_REG_MISC,
	SM_FG_REG_RESET,
	SM_FG_REG_RSNS_SEL,
	SM_FG_REG_VOL_COMP,
	SM_FG_REG_COUNT,
};

enum sm_fg_device {
	SM5602,
};

enum sm_fg_temperature_type {
	TEMPERATURE_IN = 0,
	TEMPERATURE_EX,
};

enum battery_table_type {
	BATTERY_TABLE0 = 0,
	BATTERY_TABLE1,
	BATTERY_TABLE2,
	BATTERY_TABLE_MAX,
};

enum fg_update_idx {
	FG_UPDATE_SOC = 0,
	FG_UPDATE_VOLT,
	FG_UPDATE_TEMP,
	FG_UPDATE_CYCLE,
	FG_UPDATE_CURRENT,
	FG_UPDATE_OCV,
	FG_UPDATE_FCC,
	FG_UPDATE_RMC,
	FG_UPDATE_COUNT,
};

enum {
	BATTERY_VENDOR_NVT = 0,
	BATTERY_VENDOR_GY = 1,
	BATTERY_VENDOR_XWD = 2,
	BATTERY_VENDOR_UNKNOWN = 3,
};

struct batt_params {
	bool	update_now;

	/* SOC */
	int	batt_raw_soc;
	int	batt_soc;

	/* Current averaging */
	int	batt_ma_samples_num;
	int	batt_ma_samples_index;
	int	batt_ma_avg_samples[BATT_MA_AVG_SAMPLES];
	int	batt_ma_avg;
	int	batt_ma_prev;
	int	batt_ma;

	/* Temperature averaging */
	int	batt_temp_samples_num;
	int	batt_temp_samples_index;
	int	batt_temp_avg_samples[BATT_TEMP_AVG_SAMPLES];
	int	batt_temp_avg;
	int	batt_temp_prev;
	int	batt_temp;
};

/**
 * struct sm_fg_chip - per-device state for the SM5602 fuel gauge.
 */
struct sm_fg_chip {
	struct device		*dev;
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct mutex		data_lock; /* protects cached readings */
	u8			chip; /* sm_fg_device */
	u8			regs[SM_FG_REG_COUNT];
	int			batt_id;
	int			gpio_int;
	atomic64_t		last_update_time[FG_UPDATE_COUNT];

	struct notifier_block	nb;
	struct ratelimit_state	rl_slots[SM_RL_SLOTS];

	/* Status flags */
	bool	batt_present;
	bool	batt_fc;	/* full condition */
	bool	batt_ot;	/* over-temperature */
	bool	batt_ut;	/* under-temperature */
	bool	batt_soc1;	/* low SOC level 1 */
	bool	batt_socp;	/* low SOC level 2 (poor) */
	bool	batt_dsg;	/* discharging */

	/* Cached measurements */
	int	batt_soc;
	int	batt_ocv;
	int	batt_fcc;	/* full-charge capacity, mAh */
	int	batt_rmc;	/* remaining capacity, mAh */
	int	batt_volt;	/* mV */
	int	aver_batt_volt;
	int	batt_temp;	/* °C */
	int	batt_curr;	/* mA, positive = charging */
	int	is_charging;
	int	batt_soc_cycle;
	int	topoff_soc;
	int	topoff;
	int	topoff_margin;
	int	iocv_error_count;
	int	charge_status;
	int	health;
	int	recharge_vol;

	bool	usb_present;
	bool	batt_sw_fc;
	bool	fast_mode;
	bool	shutdown_delay_enable;
	bool	shutdown_delay;
	bool	last_shutdown_delay;
	bool	soc_reporting_ready;
	bool	soc_smooth_initialized;

	/* Previous-cycle voltage/current for OCV check */
	int	p_batt_voltage;
	int	p_batt_current;

	/* Device-tree parameters */
	bool	en_temp_ex;
	bool	en_temp_in;
	bool	en_batt_det;
	bool	iocv_man_mode;
	int	aging_ctrl;
	int	batt_rsns;	/* sense-resistor, mOhm */
	int	cycle_cfg;
	int	fg_irq_set;
	int	low_soc1;
	int	low_soc2;
	int	v_l_alarm;
	int	v_h_alarm;
	int	battery_table_num;
	int	misc;
	int	batt_v_max;
	int	min_cap;
	u32	common_param_version;
	int	t_l_alarm_in;
	int	t_h_alarm_in;
	u32	t_l_alarm_ex;
	u32	t_h_alarm_ex;

	/* Battery profile tables */
	int	battery_table[BATTERY_TABLE_MAX][FG_TABLE_LEN];
	/* External NTC lookup (signed 16-bit ADC codes, -20..+80 °C) */
	s16	battery_temp_table[FG_TEMP_TABLE_CNT_MAX];
	int	alpha;
	int	beta;
	int	rs;
	int	rs_value[4];
	int	vit_period;
	int	mix_value;
	const char *battery_type;
	int	volt_cal;
	int	curr_offset;
	int	curr_slope;
	int	cap;
	int	n_tempoff;
	int	n_tempoff_offset;
	int	batt_max_voltage_uv;
	int	temp_std;
	int	en_high_fg_temp_offset;
	int	high_fg_temp_offset_denom;
	int	high_fg_temp_offset_fact;
	int	en_low_fg_temp_offset;
	int	low_fg_temp_offset_denom;
	int	low_fg_temp_offset_fact;
	int	en_high_fg_temp_cal;
	int	high_fg_temp_p_cal_denom;
	int	high_fg_temp_p_cal_fact;
	int	high_fg_temp_n_cal_denom;
	int	high_fg_temp_n_cal_fact;
	int	en_low_fg_temp_cal;
	int	low_fg_temp_p_cal_denom;
	int	low_fg_temp_p_cal_fact;
	int	low_fg_temp_n_cal_denom;
	int	low_fg_temp_n_cal_fact;
	int	en_high_temp_cal;
	int	high_temp_p_cal_denom;
	int	high_temp_p_cal_fact;
	int	high_temp_n_cal_denom;
	int	high_temp_n_cal_fact;
	int	en_low_temp_cal;
	int	low_temp_p_cal_denom;
	int	low_temp_p_cal_fact;
	int	low_temp_n_cal_denom;
	int	low_temp_n_cal_fact;
	u32	battery_param_version;
	int	fcm_offset;
#ifdef ENABLE_NTC_COMPENSATION
	int	rtrace;
	s64	prev_corr_uv;
#endif
	int	battery_id;

	struct workqueue_struct	*main_wq;
	struct workqueue_struct	*aux_wq;

	/* Work items */
	struct delayed_work	monitor_work;
	struct delayed_work	overtemp_delay_work;
	struct delayed_work	low_battery_check_work;
	unsigned long		last_update;

	/* Votables */
	struct votable	*fcc_votable;
	struct votable	*fv_votable;
	struct votable	*chg_dis_votable;

	/* Debug / fake overrides */
	int	skip_reads;
	int	skip_writes;
	int	fake_soc;
	int	fake_temp;
	u32	*dec_rate_seq;
	int	dec_rate_len;
	int	fake_chip_ok;
	struct dentry		*debug_root;

	/* Power supply objects */
	struct power_supply	*fg_psy;
	struct power_supply	*usb_psy;
	struct power_supply	*batt_psy;
	struct power_supply	*bbc_psy;
	struct power_supply	*cp_psy;
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	struct power_supply	*verify_psy;
#endif
	struct power_supply_desc fg_psy_d;

	/* Averaged measurements */
	struct batt_params	param;

	/* Overtemp workaround flags (W/A for >60 °C) */
	bool	overtemp_delay_on;
	bool	overtemp_allow_restart;

	/* Low-battery power-off check */
	bool	low_battery_power;
	bool	start_low_battery_check;

	/* Charge-pump work active flag */
	bool	cp_work_flag;

	/* Full condition */
	int	full_check;
	bool	need_full_check;
	bool	need_full_reset;
	struct delayed_work	full_check_work;
	struct work_struct	recharge_work;

	/* Linked-list node for multi-instance lookup */
	struct list_head	node;
	struct kref		kref;
	char			name[32];

	/* init guard */
	atomic_t init_running;
};

bool bq2589x_is_charge_done(void);

struct sm_fg_chip *fg_get_sm(const char *name);
void fg_put_sm(struct sm_fg_chip *sm);

void start_fg_monitor(struct sm_fg_chip *sm);
void stop_fg_monitor(struct sm_fg_chip *sm);

#endif /* SM5602_FG_H */
