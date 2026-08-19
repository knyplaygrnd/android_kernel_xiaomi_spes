/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nopmi_chg_jeita - Software JEITA charging control sub-module
 *
 * This module implements the SW JEITA temperature-based charging algorithm.
 *
 */

#if !defined(__NOPMI_CHG_JEITA_H__)
#define __NOPMI_CHG_JEITA_H__

#include "nopmi_chg_common.h"
#include "bq2589x_charger.h"

/*
 * Forward declaration of the parent driver struct.
 */
struct nopmi_chg;

#define JEITA_TEMP_ABOVE_T4_CV	4240
#define JEITA_TEMP_T3_TO_T4_CV	4240
#define JEITA_TEMP_T2_TO_T3_CV	4350
#define JEITA_TEMP_T1P5_TO_T2_CV	4240
#define JEITA_TEMP_T1_TO_T1P5_CV	4040
#define JEITA_TEMP_T0_TO_T1_CV	4040
#define JEITA_TEMP_TN1_TO_T0_CV	4040
#define JEITA_TEMP_BELOW_T0_CV	4040
#define JEITA_TEMP_NORMAL_VOLTAGE	4450 /* T2_TO_T3 normal FV */

#define TEMP_T4_THRES	50
#define TEMP_T4_THRES_MINUS_X_DEGREE	47
#define TEMP_T3_THRES	45
#define TEMP_T3_THRES_MINUS_X_DEGREE	39
#define TEMP_T2_THRES	12
#define TEMP_T2_THRES_PLUS_X_DEGREE	16
#define TEMP_T1P5_THRES	6
#define TEMP_T1P5_THRES_PLUS_X_DEGREE	11
#define TEMP_T1_THRES	3
#define TEMP_T1_THRES_PLUS_X_DEGREE	5
#define TEMP_T0_THRES	0
#define TEMP_T0_THRES_PLUS_X_DEGREE	2
#define TEMP_TN1_THRES	(-5)
#define TEMP_TN1_THRES_PLUS_X_DEGREE	(-3)
#define TEMP_NEG_10_THRES	(-10)

#define TEMP_TN1_TO_T0_FCC	442
#define TEMP_T0_TO_T1_FCC	884
#define TEMP_T1_TO_T1P5_FCC	2210
#define TEMP_T1P5_TO_T2_FCC	3536
#define TEMP_T2_TO_T3_FCC	4000 /* default jeita_current_limit */
#define TEMP_T3_TO_T4_FCC	2210

#define JEITA_WORK_DELAY_MS	2000
#define JEITA_VOTER	"JEITA_VOTER"
#define JEITA_CHG_VOTER	"JEITA_CHG_VOTER"

enum sw_jeita_state_enum {
	TEMP_BELOW_T0 = 0,
	TEMP_TN1_TO_T0,
	TEMP_T0_TO_T1,
	TEMP_T1_TO_T1P5,
	TEMP_T1P5_TO_T2,
	TEMP_T2_TO_T3,
	TEMP_T3_TO_T4,
	TEMP_ABOVE_T4,
};

enum charge_mode {
	CHG_MODE_BUCK_OFF = 0,
	CHG_MODE_CHARGING_OFF,
	CHG_MODE_CHARGING,
	CHG_MODE_MAX,
};

struct sw_jeita_data {
	int sm;
	int pre_sm;
	int cv;
	int pre_cv;
	int term_curr;
	bool charging;
	bool can_recharging;
	bool error_recovery_flag;
};

struct nopmi_chg_jeita_config {
	bool enable_sw_jeita;

	int jeita_temp_above_t4_cv;
	int jeita_temp_t3_to_t4_cv;
	int jeita_temp_t2_to_t3_cv;
	int jeita_temp_t1p5_to_t2_cv;
	int jeita_temp_t1_to_t1p5_cv;
	int jeita_temp_t0_to_t1_cv;
	int jeita_temp_tn1_to_t0_cv;
	int jeita_temp_below_t0_cv;
	int normal_charge_voltage;

	int temp_t4_thres;
	int temp_t4_thres_minus_x_degree;
	int temp_t3_thres;
	int temp_t3_thres_minus_x_degree;
	int temp_t2_thres;
	int temp_t2_thres_plus_x_degree;
	int temp_t1p5_thres;
	int temp_t1p5_thres_plus_x_degree;
	int temp_t1_thres;
	int temp_t1_thres_plus_x_degree;
	int temp_t0_thres;
	int temp_t0_thres_plus_x_degree;
	int temp_tn1_thres;
	int temp_tn1_thres_plus_x_degree;
	int temp_neg_10_thres;

	int temp_t3_to_t4_fcc;
	int temp_t2_to_t3_fcc;
	int temp_t1p5_to_t2_fcc;
	int temp_t1_to_t1p5_fcc;
	int temp_t0_to_t1_fcc;
	int temp_tn1_to_t0_fcc;
};

struct nopmi_chg_jeita_st {
	struct nopmi_chg *parent;

	struct nopmi_chg_jeita_config dt;
	/* Embed */
	struct sw_jeita_data sw_jeita;

	struct delayed_work jeita_work;
	bool sw_jeita_start;
	bool usb_present;

	int battery_temp;
	int battery_id;

	int jeita_current_limit;
	int fast_charge_mode;
};

int nopmi_chg_jeita_init(struct nopmi_chg *chg);
int nopmi_chg_jeita_deinit(struct nopmi_chg *chg);

void start_nopmi_chg_jeita_workfunc(struct nopmi_chg *chg);
void stop_nopmi_chg_jeita_workfunc(struct nopmi_chg *chg);

#endif /* __NOPMI_CHG_JEITA_H__ */
