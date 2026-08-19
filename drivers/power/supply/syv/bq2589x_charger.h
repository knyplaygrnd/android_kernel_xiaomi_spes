/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __BQ2589X_CHARGER_HEADER__
#define __BQ2589X_CHARGER_HEADER__

#include <linux/pmic-voter.h>
#include <linux/ratelimit.h>
#include "inc/tcpm.h"
#include "inc/tcpci.h"
#include "sm5602_fuelgauge.h"
#include "nopmi_chg_common.h"

#define BQ_RL_SLOTS	16
#define BQ_RL_INTERVAL	(5 * HZ)
#define BQ_RL_BURST	1

#ifdef __COUNTER__
#define _BQ_ID()	__COUNTER__
#else
#define _BQ_ID()	__LINE__
#endif

enum bq2589x_vbus_type {
	BQ2589X_VBUS_NONE,
	BQ2589X_VBUS_USB_SDP, // 5V/500MA
	BQ2589X_VBUS_USB_CDP, // CDP for bq25890, Adapter for bq25892: 5V/1A
	BQ2589X_VBUS_USB_DCP, // 5V/2A
	BQ2589X_VBUS_MAXC, // HVDCP 9V/1A
	BQ2589X_VBUS_UNKNOWN, // 5V/500MA
	BQ2589X_VBUS_NONSTAND, // float 5V/1A
	BQ2589X_VBUS_OTG, // 5V
	BQ2589X_VBUS_TYPE_NUM,
};

enum bq2589x_part_no {
	SYV690 = 0x01,
	BQ25890 = 0x03,
	BQ25892 = 0x00,
	BQ25895 = 0x07,
	SC89890H = 0x04,
};

#define BQ2589X_STATUS_PLUGIN		0x0001
#define BQ2589X_STATUS_PG			0x0002
#define BQ2589X_STATUS_CHARGE_ENABLE	0x0004
#define BQ2589X_STATUS_FAULT		0x0008

#define BQ2589X_STATUS_EXIST		0x0100

#define BQ2589X_MAX_ICL			2950
#define BQ2589X_MAX_FCC			5950

/*enum adapter_cap_type {
	MTK_PD_APDO_START,
	MTK_PD_APDO_END,
	MTK_PD,
	MTK_PD_APDO,
	MTK_CAP_TYPE_UNKNOWN,
	MTK_PD_APDO_REGAIN,
};
#define ADAPTER_CAP_MAX_NR 10

struct adapter_power_cap {
	uint8_t selected_cap_idx;
	uint8_t nr;
	uint8_t pdp;
	uint8_t pwr_limit[ADAPTER_CAP_MAX_NR];
	int max_mv[ADAPTER_CAP_MAX_NR];
	int min_mv[ADAPTER_CAP_MAX_NR];
	int ma[ADAPTER_CAP_MAX_NR];
	int maxwatt[ADAPTER_CAP_MAX_NR];
	int minwatt[ADAPTER_CAP_MAX_NR];
	uint8_t type[ADAPTER_CAP_MAX_NR];
	int info[ADAPTER_CAP_MAX_NR];
};*/

struct bq2589x_config {
	bool	enable_auto_dpdm;
	/*bool	enable_12v;*/

	int		charge_voltage;
	int		charge_current;
	int		charge_current_3500;
	int		charge_current_1500;
	int		charge_current_1000;
	int		charge_current_500;
	int		input_current_2000;

	bool	enable_term;
	int		term_current;

	bool	enable_ico;
	bool	use_absolute_vindpm;
};

struct pe_ctrl {
	bool enable;
	bool tune_up_volt;
	bool tune_down_volt;
	bool tune_done;
	bool tune_fail;
	int tune_count;
	int target_volt;
	int high_volt_level; /* vbus volt > this threshold means tune up successfully */
	int low_volt_level; /* vbus volt < this threshold means tune down successfully */
	int vbat_min_volt; /* to tune up voltage only when vbat > this threshold */
};

struct bq2589x {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	enum bq2589x_part_no part_no;
	enum bq2589x_vbus_type vbus_type;

	struct tcpc_device *tcpc_dev;
	struct notifier_block pd_nb;

	struct gpio_desc *irq_gpiod;
	struct gpio_desc *usb_switch1_gpiod;

	int		revision;
	unsigned int	status;
	//int		vbus_type;
	int		vbus_volt;
	int		vbat_volt;
	int		chg_current;
	int		rsoc;
	int		pd_active;
	bool	enabled;

	struct mutex usb_switch_lock;
	struct mutex irq_complete;
	//atomic_t dpdm_running;

	struct bq2589x_config cfg;
	struct pe_ctrl pe;

	struct work_struct adapter_in_work;
	struct work_struct adapter_out_work;
	struct work_struct start_charging_work;
	struct delayed_work monitor_work;
	struct delayed_work ico_work;
	struct delayed_work charger_work;
	struct delayed_work tune_volt_work;
	struct delayed_work check_pe_tuneup_work;
	struct delayed_work time_delay_work;
	struct delayed_work usb_changed_work;
	//struct delayed_work period_work;

	struct power_supply_desc usb;
	struct power_supply_desc wall;
	struct power_supply *batt_psy;
	struct power_supply *usb_psy;
	struct power_supply *wall_psy;
	struct power_supply *bms_psy;
	struct power_supply_config usb_cfg;
	struct power_supply_config wall_cfg;

	struct votable		*fcc_votable;
	struct votable		*fv_votable;
	struct votable		*usb_icl_votable;
	struct votable		*chg_dis_votable;
	//struct votable		*chgctrl_votable;

	enum power_supply_type chg_type;
	bool chg_online;

	int irq_gpio;
	int usb_switch1_gpio;
	bool usb_switch_flag;
	bool irq_requested;
	bool usb_switch1_requested;

	// pm ops
	int irq;
	atomic_t irq_pending;
	atomic_t ws_count;
	atomic_t resume_completed;
	atomic_t irq_disabled;
	atomic_t shutting_down;

	struct wakeup_source *bq_ws;
	struct ratelimit_state rl_slots[BQ_RL_SLOTS];

	bool vbus_on;
	bool otg_attached;
};

extern bool g_ffc_disable;
int get_prop_battery_charging_enabled(struct votable *usb_icl_votable,
				      union power_supply_propval *val);

int main_set_hiz_mode(bool en);
int main_set_charge_enable(bool en);
int main_set_input_current_limit(int curr);
int main_set_charge_current(int curr);
int main_get_charge_type(void);

#endif
