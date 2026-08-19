// SPDX-License-Identifier: GPL-2.0-only
/*
 * otg-gpio BQ2589x battery charging driver
 *
 * Copyright (C) 2013 Texas Instruments
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#define pr_fmt(fmt)	"[bq2589x_chg]: %s: " fmt, __func__

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/ratelimit.h>
#include <linux/printk.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <asm/unaligned.h>
#include "bq2589x_reg.h"
#include "bq2589x_charger.h"

#define PROFILE_CHG_VOTER	"PROFILE_CHG_VOTER"
#define MAIN_SET_VOTER		"MAIN_SET_VOTER"
#define JEITA_VOTER		"JEITA_VOTER"
//#define PD2SW_HITEMP_OCCURE_VOTER	"PD2SW_HITEMP_OCCURE_VOTER"
#define CHG_FCC_CURR_MAX	5950
#define CHG_FV_CURR_MAX		4450
#define CHG_ICL_CURR_MAX	2950
#define NOTIFY_COUNT_MAX	40
#define NO_CHANGE_MAX		5
#define RETRY_MS		500
#define RETRY_TIMEOUT_MS	10000
#define POST_INTERVAL		(2 * HZ)
#define RESET_GAP		(5 * HZ)
#define MAIN_ICL_MIN		100
#define MAIN_FCC_MIN		500

enum print_reason {
	PR_INTERRUPT	= BIT(0),
	PR_REGISTER	= BIT(1),
	PR_OEM		= BIT(2),
	PR_DEBUG	= BIT(3),
};

static int debug_mask = PR_OEM;
module_param_named(debug_mask, debug_mask, int, 0600);

// Legacy
#define bq_dbg(...)	do { } while (0)

#define bq_log(fmt, ...)							\
do {										\
	if (bq && __ratelimit(&bq->rl_slots[_BQ_ID() & (BQ_RL_SLOTS - 1)]))	\
		pr_info(fmt, ##__VA_ARGS__);					\
} while (0)

#define bq_err(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)
#define bq_info(fmt, ...)	pr_info(fmt, ##__VA_ARGS__)
#define bq_debug(fmt, ...)	pr_debug(fmt, ##__VA_ARGS__)

static const struct regmap_config bq2589x_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= BQ2589X_REG_14,
	.cache_type	= REGCACHE_NONE,
};

static struct bq2589x *g_bq;
//static struct pe_ctrl pe;
//int get_apdo_regain;
//static bool vbus_on = false;

static int bq2589x_read_byte(struct bq2589x *bq, u8 reg, u8 *data)
{
	int ret, retry;
	unsigned int val;
	const int max_retry = 3;

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_read(bq->regmap, reg, &val);
		if (ret == 0) {
			*data = (u8)val;
			return 0;
		}

		bq_err("read 0x%02x failed (try %d/%d): %d\n",
		       reg, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	*data = 0;
	return ret;
}

static int bq2589x_write_byte(struct bq2589x *bq, u8 reg, u8 data)
{
	int ret, retry;
	const int max_retry = 3;

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_write(bq->regmap, reg, data);
		if (ret == 0)
			return 0;

		bq_err("write 0x%02x->0x%02x failed (try %d/%d): %d\n",
		       data, reg, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	return ret;
}

static int bq2589x_update_bits(struct bq2589x *bq, u8 reg, u8 mask, u8 data)
{
	int ret, retry;
	const int max_retry = 3;

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_update_bits(bq->regmap, reg, mask, data);
		if (ret == 0)
			return 0;

		bq_err("update_bits 0x%02x (mask=0x%02x val=0x%02x) failed (try %d/%d): %d\n",
		       reg, mask, data, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	return ret;
}

static void bq_ws_get(struct bq2589x *bq)
{
	if (atomic_inc_return(&bq->ws_count) == 1)
		__pm_stay_awake(bq->bq_ws);
}

static void bq_ws_put(struct bq2589x *bq)
{
	if (atomic_dec_return(&bq->ws_count) == 0)
		__pm_relax(bq->bq_ws);
}

static int bq2589x_enable_otg(struct bq2589x *bq)
{
	u8 val = BQ2589X_OTG_ENABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03,
		BQ2589X_OTG_CONFIG_MASK, val);
}

static int bq2589x_disable_otg(struct bq2589x *bq)
{
	u8 val = BQ2589X_OTG_DISABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03,
		BQ2589X_OTG_CONFIG_MASK, val);
}

#if 0
static int bq2589x_set_otg_volt(struct bq2589x *bq, int volt)
{
	u8 val = 0;

	if (bq->part_no == SC89890H) {
		if (volt < SC89890H_BOOSTV_BASE)
			volt = SC89890H_BOOSTV_BASE;
		if (volt > SC89890H_BOOSTV_BASE +
		    (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) *
		    SC89890H_BOOSTV_LSB)
			volt = SC89890H_BOOSTV_BASE +
				(BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) *
				SC89890H_BOOSTV_LSB;

		val = ((volt - SC89890H_BOOSTV_BASE) /
			SC89890H_BOOSTV_LSB) << BQ2589X_BOOSTV_SHIFT;
	} else {
		if (volt < BQ2589X_BOOSTV_BASE)
			volt = BQ2589X_BOOSTV_BASE;
		if (volt > BQ2589X_BOOSTV_BASE +
		    (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) *
		    BQ2589X_BOOSTV_LSB)
			volt = BQ2589X_BOOSTV_BASE +
				(BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) *
				BQ2589X_BOOSTV_LSB;

		val = ((volt - BQ2589X_BOOSTV_BASE) / BQ2589X_BOOSTV_LSB) <<
			BQ2589X_BOOSTV_SHIFT;
	}

	return bq2589x_update_bits(bq, BQ2589X_REG_0A,
		BQ2589X_BOOSTV_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_volt);
#endif

static int bq2589x_set_otg_current(struct bq2589x *bq, int curr)
{
	u8 temp;

	if (bq->part_no == SC89890H) {
		if (curr < 600)
			temp = SC89890H_BOOST_LIM_500MA;
		else if (curr < 900)
			temp = SC89890H_BOOST_LIM_750MA;
		else if (curr < 1300)
			temp = SC89890H_BOOST_LIM_1200MA;
		else if (curr < 1500)
			temp = SC89890H_BOOST_LIM_1400MA;
		else if (curr < 1700)
			temp = SC89890H_BOOST_LIM_1650MA;
		else if (curr < 1900)
			temp = SC89890H_BOOST_LIM_1875MA;
		else if (curr < 2200)
			temp = SC89890H_BOOST_LIM_2150MA;
		else if (curr < 2500)
			temp = SC89890H_BOOST_LIM_2450MA;
		else
			temp = SC89890H_BOOST_LIM_1400MA;
	} else {
		if (curr <= 500)
			temp = BQ2589X_BOOST_LIM_500MA;
		else if (curr > 500 && curr <= 800)
			temp = BQ2589X_BOOST_LIM_700MA;
		else if (curr > 800 && curr <= 1200)
			temp = BQ2589X_BOOST_LIM_1100MA;
		else if (curr > 1200 && curr <= 1400)
			temp = BQ2589X_BOOST_LIM_1300MA;
		else if (curr > 1400 && curr <= 1700)
			temp = BQ2589X_BOOST_LIM_1600MA;
		else if (curr > 1700 && curr <= 1900)
			temp = BQ2589X_BOOST_LIM_1800MA;
		else if (curr > 1900 && curr <= 2200)
			temp = BQ2589X_BOOST_LIM_2100MA;
		else if (curr > 2200 && curr <= 2300)
			temp = BQ2589X_BOOST_LIM_2400MA;
		else
			temp = BQ2589X_BOOST_LIM_2400MA;
	}

	return bq2589x_update_bits(bq, BQ2589X_REG_0A,
		BQ2589X_BOOST_LIM_MASK,
		temp << BQ2589X_BOOST_LIM_SHIFT);
}

static int bq2589x_enable_charger(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_CHG_ENABLE << BQ2589X_CHG_CONFIG_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_03,
				  BQ2589X_CHG_CONFIG_MASK, val);
	if (ret == 0)
		bq->status |= BQ2589X_STATUS_CHARGE_ENABLE;

	return ret;
}

static int bq2589x_disable_charger(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_CHG_DISABLE << BQ2589X_CHG_CONFIG_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_03,
				  BQ2589X_CHG_CONFIG_MASK, val);
	if (ret == 0)
		bq->status &= ~BQ2589X_STATUS_CHARGE_ENABLE;

	return ret;
}

/* interfaces that can be called by other module */
static int bq2589x_adc_start(struct bq2589x *bq, bool oneshot)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_02, &val);
	if (ret < 0) {
		bq_err("failed to read register 0x02:%d\n", ret);
		return ret;
	}

	if (((val & BQ2589X_CONV_RATE_MASK) >> BQ2589X_CONV_RATE_SHIFT) ==
	    BQ2589X_ADC_CONTINUE_ENABLE)
		return 0; /*is doing continuous scan*/

	if (oneshot)
		return bq2589x_update_bits(bq, BQ2589X_REG_02,
			BQ2589X_CONV_START_MASK,
			BQ2589X_CONV_START << BQ2589X_CONV_START_SHIFT);

	return bq2589x_update_bits(bq, BQ2589X_REG_02,
		BQ2589X_CONV_RATE_MASK,
		BQ2589X_ADC_CONTINUE_ENABLE << BQ2589X_CONV_RATE_SHIFT);
}

static int bq2589x_adc_stop(struct bq2589x *bq)
{
	return bq2589x_update_bits(bq, BQ2589X_REG_02,
		BQ2589X_CONV_RATE_MASK,
		BQ2589X_ADC_CONTINUE_DISABLE << BQ2589X_CONV_RATE_SHIFT);
}

static int bq2589x_adc_read_battery_volt(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0E, &val);
	if (ret < 0) {
		bq_err("read battery voltage failed: %d\n", ret);
		return ret;
	}

	return BQ2589X_BATV_BASE +
		((val & BQ2589X_BATV_MASK) >> BQ2589X_BATV_SHIFT) *
		BQ2589X_BATV_LSB;
}

#if 0
int bq2589x_adc_read_sys_volt(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0F, &val);
	if (ret < 0) {
		bq_err("read system voltage failed: %d\n", ret);
		return ret;

	return BQ2589X_SYSV_BASE +
		((val & BQ2589X_SYSV_MASK) >> BQ2589X_SYSV_SHIFT) *
		BQ2589X_SYSV_LSB;
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_sys_volt);
#endif

static int bq2589x_adc_read_vbus_volt(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_11, &val);
	if (ret < 0) {
		bq_err("read vbus voltage failed: %d\n", ret);
		return ret;
	}

	return BQ2589X_VBUSV_BASE +
		((val & BQ2589X_VBUSV_MASK) >> BQ2589X_VBUSV_SHIFT) *
		BQ2589X_VBUSV_LSB;
}

#if 0
int bq2589x_adc_read_temperature(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_10, &val);
	if (ret < 0) {
		bq_err("read temperature failed: %d\n", ret);
		return ret;
	}

	return BQ2589X_TSPCT_BASE +
		((val & BQ2589X_TSPCT_MASK) >> BQ2589X_TSPCT_SHIFT) *
		BQ2589X_TSPCT_LSB;
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_temperature);
#endif

static int bq2589x_adc_read_charge_current(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_12, &val);
	if (ret < 0) {
		bq_err("read charge current failed: %d\n", ret);
		return ret;
	}

	return (BQ2589X_ICHGR_BASE +
		((val & BQ2589X_ICHGR_MASK) >> BQ2589X_ICHGR_SHIFT) *
		BQ2589X_ICHGR_LSB);
}

static int bq2589x_set_charge_current(struct bq2589x *bq, int curr)
{
	u8 ichg;

	if (curr < 0)
		curr = 0;

	if (bq->part_no == SC89890H)
		ichg = (curr - SC89890H_ICHG_BASE) / SC89890H_ICHG_LSB;
	else
		ichg = (curr - BQ2589X_ICHG_BASE) / BQ2589X_ICHG_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_04,
		BQ2589X_ICHG_MASK, ichg << BQ2589X_ICHG_SHIFT);
}

static int bq2589x_get_charge_current(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_04, &val);
	if (ret < 0) {
		bq_err("get charge current failed: %d\n", ret);
		return ret;
	}

	return ((val & BQ2589X_ICHG_MASK) >> BQ2589X_ICHG_SHIFT) *
		BQ2589X_ICHG_LSB + BQ2589X_ICHG_BASE;
}

static int bq2589x_set_term_current(struct bq2589x *bq, int curr)
{
	u8 iterm;

	if (bq->part_no == SC89890H) {
		if (curr > SC89890H_ITERM_MAX)
			curr = SC89890H_ITERM_MAX;
		iterm = (curr - SC89890H_ITERM_BASE) / SC89890H_ITERM_LSB;
	} else {
		iterm = (curr - BQ2589X_ITERM_BASE) / BQ2589X_ITERM_LSB;
	}

	return bq2589x_update_bits(bq, BQ2589X_REG_05,
		BQ2589X_ITERM_MASK,
		iterm << BQ2589X_ITERM_SHIFT);
}

static int bq2589x_set_prechg_current(struct bq2589x *bq, int curr)
{
	u8 iprechg;

	if (bq->part_no == SC89890H)
		iprechg = (curr - SC89890H_IPRECHG_BASE) / SC89890H_IPRECHG_LSB;
	else
		iprechg = (curr - BQ2589X_IPRECHG_BASE) / BQ2589X_IPRECHG_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_05,
		BQ2589X_IPRECHG_MASK,
		iprechg << BQ2589X_IPRECHG_SHIFT);
}

static int bq2589x_set_chargevoltage(struct bq2589x *bq, int volt)
{
	u8 val = (volt - BQ2589X_VREG_BASE) / BQ2589X_VREG_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_06,
		BQ2589X_VREG_MASK,
		val << BQ2589X_VREG_SHIFT);
}

static int bq2589x_get_chargevoltage(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_06, &val);
	if (ret < 0) {
		bq_err("get charge voltage failed: %d\n", ret);
		return ret;
	}

	return ((val & BQ2589X_VREG_MASK) >> BQ2589X_VREG_SHIFT) *
		BQ2589X_VREG_LSB + BQ2589X_VREG_BASE;
}

static int main_set_charge_voltage(int volt)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	return bq2589x_set_chargevoltage(g_bq, volt);
}

static int bq2589x_set_input_volt_limit(struct bq2589x *bq, int volt)
{
	u8 val = (volt - BQ2589X_VINDPM_BASE) / BQ2589X_VINDPM_LSB;

	return bq2589x_update_bits(bq, BQ2589X_REG_0D,
		BQ2589X_VINDPM_MASK,
		val << BQ2589X_VINDPM_SHIFT);
}

static int bq2589x_set_input_current_limit(struct bq2589x *bq, int curr)
{
	u8 val;

	if (curr < BQ2589X_IINLIM_BASE)
		curr = BQ2589X_IINLIM_BASE;

	val = (curr - BQ2589X_IINLIM_BASE) / BQ2589X_IINLIM_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_00,
		BQ2589X_IINLIM_MASK,
		val << BQ2589X_IINLIM_SHIFT);
}

static int bq2589x_get_input_current_limit(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_00, &val);
	if (ret < 0) {
		bq_err("get input current limit failed: %d\n", ret);
		return ret;
	}

	return ((val & BQ2589X_IINLIM_MASK) >> BQ2589X_IINLIM_SHIFT) *
		BQ2589X_IINLIM_LSB + BQ2589X_IINLIM_BASE;
}

static int bq2589x_set_vindpm_offset(struct bq2589x *bq, int offset)
{
	u8 val;

	if (bq->part_no == SC89890H) {
		val = (offset < 500) ? SC89890h_VINDPMOS_400MV : SC89890h_VINDPMOS_600MV;
		return bq2589x_update_bits(bq, BQ2589X_REG_01,
			SC89890H_VINDPMOS_MASK,
			val << SC89890H_VINDPMOS_SHIFT);
	}

	val = (offset - BQ2589X_VINDPMOS_BASE) / BQ2589X_VINDPMOS_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_01,
		BQ2589X_VINDPMOS_MASK,
		val << BQ2589X_VINDPMOS_SHIFT);
}

static u8 bq2589x_get_charging_status(struct bq2589x *bq)
{
	u8 reg;
	int ret;
	int cap = -1;
	union power_supply_propval propval = {0, };

	if (!bq)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &reg);
	if (ret < 0) {
		bq_err("failed to read register 0x0b: %d\n", ret);
		return POWER_SUPPLY_STATUS_UNKNOWN;
	}

	switch ((reg & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT) {
	case BQ2589X_CHRG_STAT_IDLE:
		bq_log("not charging\n");
		return POWER_SUPPLY_STATUS_DISCHARGING;
	case BQ2589X_CHRG_STAT_PRECHG:
		bq_log("pre charging\n");
		return POWER_SUPPLY_STATUS_CHARGING;
	case BQ2589X_CHRG_STAT_FASTCHG:
		bq_log("fast charging\n");
		return POWER_SUPPLY_STATUS_CHARGING;
	case BQ2589X_CHRG_STAT_CHGDONE:
		bq_log("charge done!\n");

		if (!bq->bms_psy)
			bq->bms_psy = power_supply_get_by_name("bms");

		if (bq->bms_psy) {
			ret = power_supply_get_property(bq->bms_psy,
							POWER_SUPPLY_PROP_CAPACITY, &propval);
			if (ret == 0) {
				cap = propval.intval;
				bq_log("battery cap: %d\n", cap);
			} else {
				bq_err("get battery cap fail: %d\n", ret);
			}
		}

		if (cap > 95)
			return POWER_SUPPLY_STATUS_FULL;
		return POWER_SUPPLY_STATUS_CHARGING;

	default:
		return POWER_SUPPLY_STATUS_UNKNOWN;
	}
}

static void bq2589x_set_otg(struct bq2589x *bq, bool enable)
{
	int ret;

	if (enable) {
		bq2589x_disable_charger(bq);

		ret = bq2589x_enable_otg(bq);
		if (ret < 0) {
			bq_err("failed to enable otg: %d\n", ret);
			return;
		}
	} else {
		ret = bq2589x_disable_otg(bq);
		if (ret < 0) {
			bq_err("failed to disable otg: %d\n", ret);
			return;
		}

		bq2589x_enable_charger(bq);
	}
}

static int bq2589x_disable_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_DISABLE << BQ2589X_WDT_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_07,
		BQ2589X_WDT_MASK, val);
}

static __maybe_unused int bq2589x_set_watchdog_timer(struct bq2589x *bq, u8 timeout)
{
	return bq2589x_update_bits(bq, BQ2589X_REG_07,
		BQ2589X_WDT_MASK,
		(u8)((timeout - BQ2589X_WDT_BASE) / BQ2589X_WDT_LSB) << BQ2589X_WDT_SHIFT);
}

static int bq2589x_reset_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_RESET << BQ2589X_WDT_RESET_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03,
		BQ2589X_WDT_RESET_MASK, val);
}

static int bq2589x_force_dpdm(struct bq2589x *bq)
{
	int ret;
	u8 data = 0;
	u8 val = BQ2589X_FORCE_DPDM << BQ2589X_FORCE_DPDM_SHIFT;

//modify by HTH-209427/HTH-209841/HTH-234945/HTH-234948 at 2022/06/08 begin
	if (bq->part_no == SC89890H &&
	    bq->vbus_type == BQ2589X_VBUS_MAXC) {
		ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &data);
		if (ret < 0) {
			bq_err("failed to read REG_0B: %d\n", ret);
			return ret;
		}

		if ((data & 0xE0) == 0x80) {
			bq2589x_write_byte(bq, BQ2589X_REG_01, 0x45);
			msleep(30);
			bq2589x_write_byte(bq, BQ2589X_REG_01, 0x25);
			msleep(30);
		}
	}
//modify by HTH-209427/HTH-209841/HTH-234945/HTH-234948 at 2022/06/08 end

	return bq2589x_update_bits(bq, BQ2589X_REG_02,
		BQ2589X_FORCE_DPDM_MASK, val);
}

static int bq2589x_is_dpdm_done(struct bq2589x *bq, int *done)
{
	int ret;
	u8 data = 0, force_dpdm_bit = 0;

	if (!done)
		return -EINVAL;

	if (bq->part_no == SC89890H) {
		/*
		 * SC89890H: Check PG_STAT (Power Good Status)
		 * PG_STAT = 1: Power Good
		 * PG_STAT = 0: Power Bad (under DPM)
		 * done = 1 when power is good (DPDM process finished)
		 */
		ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &data);
		if (ret < 0)
			return ret;

		*done = (data & BQ2589X_PG_STAT_MASK) ? 1 : 0;
		bq_dbg("SC89890H DPDM: REG_0B=0x%02x, PG_STAT=%d\n", data, *done);
	} else {
		/*
		 * BQ2589X: Check FORCE_DPDM bit
		 * FORCE_DPDM = 0: DPDM detection done
		 * FORCE_DPDM = 1: DPDM detection in progress
		 * done = 1 when bit is cleared (DPDM process finished)
		 */
		ret = bq2589x_read_byte(bq, BQ2589X_REG_02, &data);
		if (ret < 0)
			return ret;

		force_dpdm_bit = (data & BQ2589X_FORCE_DPDM_MASK) >> BQ2589X_FORCE_DPDM_SHIFT;
		*done = (force_dpdm_bit == 0) ? 1 : 0; /* Clear = Done */
		bq_dbg("BQ2589X DPDM: REG_02=0x%02x, FORCE_DPDM=%d, done=%d\n",
		       data, force_dpdm_bit, *done);
	}

	return ret;
}

static int bq2589x_force_dpdm_done(struct bq2589x *bq)
{
	int ret;
	int done = 0; /* 0: not done, 1: done */
	int retry = 200; /* 200 × 20 ms ≈ 4 s total timeout */

	bq->status &= ~BQ2589X_STATUS_PLUGIN;
	bq_info("force DPDM start\n");

	ret = bq2589x_force_dpdm(bq);
	if (ret < 0) {
		bq_err("failed to trigger DPDM: %d\n", ret);
		return ret;
	}

	while (retry-- > 0) {
		ret = bq2589x_is_dpdm_done(bq, &done);
		if (ret < 0) {
			bq_err("read DPDM done status failed: %d\n", ret);
			return ret;
		}

		if (done) {
			bq_info("DPDM done\n");
			return 0;
		}

		usleep_range(20000, 20100);
	}

	bq_err("DPDM timeout (after ~4 seconds)\n");
	return -ETIMEDOUT;
}

static enum bq2589x_vbus_type bq2589x_get_vbus_type(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &val);
	if (ret < 0) {
		bq_err("failed to read 0B byte, ret: %d\n", ret);
		return BQ2589X_VBUS_UNKNOWN;
	}

	return (val & BQ2589X_VBUS_STAT_MASK) >> BQ2589X_VBUS_STAT_SHIFT;
}

static enum bq2589x_vbus_type bq2589x_get_vbus_valid(struct bq2589x *bq)
{
	enum bq2589x_vbus_type vbus_type = BQ2589X_VBUS_UNKNOWN;
	int ret;
	int retry = 3;

	ret = bq2589x_force_dpdm_done(bq);
	if (ret < 0) {
		bq_err("DPDM handshake failed: %d\n", ret);
		return vbus_type;
	}

	do {
		vbus_type = bq2589x_get_vbus_type(bq);
		if (vbus_type != BQ2589X_VBUS_UNKNOWN)
			break;
		usleep_range(10000, 10100);
	} while (--retry > 0);

	bq_info("vbus_type: %d\n", vbus_type);
	return vbus_type;
}

static int bq2589x_reset_chip(struct bq2589x *bq)
{
	u8 val = BQ2589X_RESET << BQ2589X_RESET_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_14,
		BQ2589X_RESET_MASK, val);
}

#if 0
int bq2589x_enter_ship_mode(struct bq2589x *bq)
{
	int ret;
	u8 val = BQ2589X_BATFET_OFF << BQ2589X_BATFET_DIS_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09,
				  BQ2589X_BATFET_DIS_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_enter_ship_mode);
#endif

static int bq2589x_enter_hiz_mode(struct bq2589x *bq)
{
	u8 val = BQ2589X_HIZ_ENABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00,
		BQ2589X_ENHIZ_MASK, val);
}

static int bq2589x_exit_hiz_mode(struct bq2589x *bq)
{
	u8 val = BQ2589X_HIZ_DISABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00,
		BQ2589X_ENHIZ_MASK, val);
}

#if 0
int bq2589x_get_hiz_mode(struct bq2589x *bq, u8 *state)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_00, &val);
	if (ret)
		return ret;

	*state = (val & BQ2589X_ENHIZ_MASK) >> BQ2589X_ENHIZ_SHIFT;

	return 0;
}
EXPORT_SYMBOL_GPL(bq2589x_get_hiz_mode);

int bq2589x_pumpx_enable(struct bq2589x *bq, int enable)
{
	u8 val;

	if (enable)
		val = BQ2589X_PUMPX_ENABLE << BQ2589X_EN_PUMPX_SHIFT;
	else
		val = BQ2589X_PUMPX_DISABLE << BQ2589X_EN_PUMPX_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_04,
		BQ2589X_EN_PUMPX_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_enable);
#endif

static int bq2589x_pumpx_increase_volt(struct bq2589x *bq)
{
	u8 val = BQ2589X_PUMPX_UP << BQ2589X_PUMPX_UP_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_09,
		BQ2589X_PUMPX_UP_MASK, val);
}

static int bq2589x_pumpx_increase_volt_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_09, &val);
	if (ret < 0)
		return ret;

	/* 1 = bit still set = not finished; 0 = cleared by HW = done */
	return (val & BQ2589X_PUMPX_UP_MASK) ? 1 : 0;
}

static int bq2589x_pumpx_decrease_volt(struct bq2589x *bq)
{
	u8 val = BQ2589X_PUMPX_DOWN << BQ2589X_PUMPX_DOWN_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_09,
		BQ2589X_PUMPX_DOWN_MASK, val);
}

static int bq2589x_pumpx_decrease_volt_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_09, &val);
	if (ret < 0)
		return ret;

	/* 1 = bit still set = not finished; 0 = cleared by HW = done */
	return (val & BQ2589X_PUMPX_DOWN_MASK) ? 1 : 0;
}

static int bq2589x_force_ico(struct bq2589x *bq)
{
	u8 val = BQ2589X_FORCE_ICO << BQ2589X_FORCE_ICO_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_09,
		BQ2589X_FORCE_ICO_MASK, val);
}

static int bq2589x_check_force_ico_done(struct bq2589x *bq)
{
	u8 val;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_14, &val);
	if (ret < 0)
		return ret;

	/* 1 = bit still set = not finished; 0 = cleared by HW = done */
	return (val & BQ2589X_ICO_OPTIMIZED_MASK) ? 1 : 0;
}

static int bq2589x_enable_term(struct bq2589x *bq, bool enable)
{
	u8 val = enable ? (BQ2589X_TERM_ENABLE << BQ2589X_EN_TERM_SHIFT)
			: (BQ2589X_TERM_DISABLE << BQ2589X_EN_TERM_SHIFT);

	return bq2589x_update_bits(bq, BQ2589X_REG_07,
		BQ2589X_EN_TERM_MASK, val);
}

static int bq2589x_enable_auto_dpdm(struct bq2589x *bq, bool enable)
{
	u8 val = enable ? (BQ2589X_AUTO_DPDM_ENABLE << BQ2589X_AUTO_DPDM_EN_SHIFT)
			: (BQ2589X_AUTO_DPDM_DISABLE << BQ2589X_AUTO_DPDM_EN_SHIFT);

	return bq2589x_update_bits(bq, BQ2589X_REG_02,
		BQ2589X_AUTO_DPDM_EN_MASK, val);
}

static int bq2589x_use_absolute_vindpm(struct bq2589x *bq, bool enable)
{
	u8 val = enable ? (BQ2589X_FORCE_VINDPM_ENABLE << BQ2589X_FORCE_VINDPM_SHIFT)
			: (BQ2589X_FORCE_VINDPM_DISABLE << BQ2589X_FORCE_VINDPM_SHIFT);

	return bq2589x_update_bits(bq, BQ2589X_REG_0D,
		BQ2589X_FORCE_VINDPM_MASK, val);
}

static int bq2589x_enable_ico(struct bq2589x *bq, bool enable)
{
	u8 val = enable ? (BQ2589X_ICO_ENABLE << BQ2589X_ICOEN_SHIFT)
			: (BQ2589X_ICO_DISABLE << BQ2589X_ICOEN_SHIFT);

	return bq2589x_update_bits(bq, BQ2589X_REG_02,
		BQ2589X_ICOEN_MASK, val);
}

#if 0
static int bq2589x_read_idpm_limit(struct bq2589x *bq)
{
	u8 val;
	int curr;
	int ret;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_13, &val);
	if (ret < 0) {
		bq_err("read vbus voltage failed: %d\n", ret);
		return ret;
	}

	return BQ2589X_IDPM_LIM_BASE +
		((val & BQ2589X_IDPM_LIM_MASK) >> BQ2589X_IDPM_LIM_SHIFT) *
		BQ2589X_IDPM_LIM_LSB;
}
EXPORT_SYMBOL_GPL(bq2589x_read_idpm_limit);
#endif

bool bq2589x_is_charge_done(void)
{
	u8 val;
	int ret;

	if (IS_ERR_OR_NULL(g_bq))
		return false;

	ret = bq2589x_read_byte(g_bq, BQ2589X_REG_0B, &val);
	if (ret < 0) {
		bq_err("read REG0B failed: %d\n", ret);
		return false;
	}

	val = (val & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT;
	return (val == BQ2589X_CHRG_STAT_CHGDONE);
}
EXPORT_SYMBOL_GPL(bq2589x_is_charge_done);

static __maybe_unused void bq2589x_dump_regs(struct bq2589x *bq)
{
	int addr, ret;
	u8 val;

	bq_debug("dump_regs:\n");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(bq, addr, &val);
		if (ret == 0)
			bq_debug("Reg[%02x] = 0x%02x\n", (unsigned int)addr, (unsigned int)val);
	}
}

static int bq2589x_init_device(struct bq2589x *bq)
{
	int ret;

	if (bq->part_no == SC89890H)
		bq2589x_update_bits(bq, BQ2589X_REG_00,
				    BQ2589X_ENILIM_MASK,
				    BQ2589X_ENILIM_DISABLE << BQ2589X_ENILIM_SHIFT);

	bq2589x_enable_ico(bq, false);
	bq2589x_disable_watchdog_timer(bq);
	bq2589x_enable_auto_dpdm(bq, bq->cfg.enable_auto_dpdm);
	bq2589x_enable_term(bq, bq->cfg.enable_term);

	/* Force absolute VINDPM when auto-DPDM is disabled */
	if (!bq->cfg.enable_auto_dpdm)
		bq->cfg.use_absolute_vindpm = true;
	bq2589x_use_absolute_vindpm(bq, bq->cfg.use_absolute_vindpm);

	ret = bq2589x_set_vindpm_offset(bq, 600);
	if (ret < 0) {
		bq_err("failed to set vindpm offset: %d\n", ret);
		return ret;
	}

	ret = bq2589x_set_term_current(bq, bq->cfg.term_current);
	if (ret < 0) {
		bq_err("failed to set termination current: %d\n", ret);
		return ret;
	}

	ret = bq2589x_set_prechg_current(bq, 200);
	if (ret < 0) {
		bq_err("failed to set prechg current: %d\n", ret);
		return ret;
	}

	ret = bq2589x_set_chargevoltage(bq, bq->cfg.charge_voltage);
	if (ret < 0) {
		bq_err("failed to set charge voltage: %d\n", ret);
		return ret;
	}

	bq2589x_enable_charger(bq);

	bq2589x_update_bits(bq, BQ2589X_REG_00,
			    BQ2589X_ENILIM_MASK,
			    BQ2589X_ENILIM_DISABLE << BQ2589X_ENILIM_SHIFT);
	bq2589x_update_bits(bq, BQ2589X_REG_02, 0x8, 1 << 3);

	if (bq->part_no == SYV690) {
		bq_info("init syv690 HV_TYPE 9/12V\n");
		/* ASSUME: bit2 of REG_02 selects 9 V (0) or 12 V (1) per SYV690 datasheet */
		bq2589x_update_bits(bq, BQ2589X_REG_02, 0x4, 0 << 2);
	}

	bq2589x_adc_stop(bq);

	return 0;
}

static int bq2589x_charge_status(struct bq2589x *bq)
{
	u8 val = 0;
	int ret;

	if (!bq)
		return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &val);
	if (ret < 0) {
		bq_err("read REG_0B failed: %d\n", ret);
		return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	}

	bq_debug("REG_0B=0x%x\n", val);

	val = (val & BQ2589X_CHRG_STAT_MASK) >> BQ2589X_CHRG_STAT_SHIFT;
	switch (val) {
	case BQ2589X_CHRG_STAT_FASTCHG:
		return POWER_SUPPLY_CHARGE_TYPE_FAST;
	case BQ2589X_CHRG_STAT_PRECHG:
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
	case BQ2589X_CHRG_STAT_CHGDONE:
	case BQ2589X_CHRG_STAT_IDLE:
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	default:
		return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	}
}

static enum power_supply_property bq2589x_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_ONLINE, /* External power source */
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_ENABLED,
	POWER_SUPPLY_PROP_TERMINATION_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE, /* Charger status output */
};

static int bq2589x_wall_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct bq2589x *bq = power_supply_get_drvdata(psy);
	int online = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bq2589x_get_charging_status(bq);
#if 0
		if (get_effective_result_locked(bq->fv_votable) < 4450) {
			if (val->intval == POWER_SUPPLY_STATUS_FULL)
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
		} else if (get_client_vote_locked(bq->usb_icl_votable,
				"MAIN_CHG_SUSPEND_VOTER") == MAIN_ICL_MIN) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		}
#endif
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		online = READ_ONCE(bq->chg_online);
		if (bq->vbat_volt < 3300)
			online = 0;
		val->intval = online;
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = READ_ONCE(bq->chg_type);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bq->vbus_volt;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = bq->chg_current;
		break;
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		val->intval = bq->enabled;
		break;
	case POWER_SUPPLY_PROP_TERMINATION_CURRENT:
		val->intval = bq->cfg.term_current;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = bq2589x_charge_status(bq);
		bq_log("CHARGE_TYPE: %d\n", val->intval);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int bq2589x_wall_set_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     const union power_supply_propval *val)
{
	int ret = 0;
	struct bq2589x *bq = power_supply_get_drvdata(psy);

	switch (psp) {
	//case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_REAL_TYPE:
		WRITE_ONCE(bq->chg_type, val->intval);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		WRITE_ONCE(bq->chg_online, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		bq->vbus_volt = val->intval;
		ret = main_set_charge_voltage(bq->vbus_volt);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		bq->chg_current = val->intval;
		ret = main_set_charge_current(bq->chg_current);
		break;
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		bq->enabled = val->intval;
		ret = main_set_charge_enable(bq->enabled);
		break;
	case POWER_SUPPLY_PROP_TERMINATION_CURRENT:
		bq->cfg.term_current = val->intval;
		ret = bq2589x_set_term_current(bq, bq->cfg.term_current);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static int bq2589x_wall_prop_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	//case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_REAL_TYPE:
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_TERMINATION_CURRENT:
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		return 1;
	default:
		break;
	}

	return 0;
}

static int bq2589x_psy_register(struct bq2589x *bq)
{
	struct power_supply_config wall_cfg = {};

	bq->wall.name = "bbc";
	// bq->wall.type = POWER_SUPPLY_TYPE_MAINS;
	bq->wall.type = POWER_SUPPLY_TYPE_USB_TYPE_C;
	bq->wall.properties = bq2589x_charger_props;
	bq->wall.num_properties = ARRAY_SIZE(bq2589x_charger_props);
	bq->wall.get_property = bq2589x_wall_get_property;
	bq->wall.set_property = bq2589x_wall_set_property;
	bq->wall.property_is_writeable = bq2589x_wall_prop_is_writeable;
	bq->wall.external_power_changed = NULL;

	wall_cfg.drv_data = bq;
	wall_cfg.of_node = bq->dev->of_node;
	wall_cfg.num_supplicants = 0;

	bq->wall_psy = devm_power_supply_register(bq->dev, &bq->wall, &wall_cfg);
	if (IS_ERR(bq->wall_psy)) {
		bq_err("failed to register wall psy\n");
		return PTR_ERR(bq->wall_psy);
	}

	bq_info("%s power supply register successfully\n", bq->wall.name);

	return 0;
}

static void bq2589x_psy_unregister(struct bq2589x *bq)
{
	bq_info("start unregister\n");
}

static ssize_t registers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	u8 addr;
	u8 val;
	int idx = 0;
	int ret;

	if (IS_ERR_OR_NULL(g_bq))
		return -ENODEV;

	idx += scnprintf(buf + idx, PAGE_SIZE - idx, "Charger 1:\n");

	for (addr = 0x00; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(g_bq, addr, &val);
		if (ret) {
			dev_warn(g_bq->dev, "read reg 0x%02x failed: %d\n", addr, ret);
			continue;
		}

		idx += scnprintf(buf + idx, PAGE_SIZE - idx, "Reg[0x%02x] = 0x%02x\n", addr, val);
		if (idx >= PAGE_SIZE)
			break;
	}

	return idx;
}

static ssize_t registers_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	char tmp[32];
	unsigned long reg_ul;
	unsigned long val_ul;
	int ret;
	char *p;

	if (IS_ERR_OR_NULL(g_bq))
		return -ENODEV;

	if (count == 0 || count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';

	ret = kstrtoul(tmp, 0, &reg_ul);
	if (ret)
		return -EINVAL;

	p = tmp;
	while (*p && !isspace(*p))
		p++;
	while (*p && isspace(*p))
		p++;
	if (!*p)
		return -EINVAL;

	ret = kstrtoul(p, 0, &val_ul);
	if (ret)
		return -EINVAL;

	if (reg_ul > 0x14 || val_ul > 0xff) {
		dev_err(g_bq->dev, "invalid reg/val: reg=0x%lx val=0x%lx\n", reg_ul, val_ul);
		return -EINVAL;
	}

	ret = bq2589x_write_byte(g_bq, (u8)reg_ul, (u8)val_ul);
	if (ret) {
		dev_err(g_bq->dev, "write reg 0x%02lx failed: %d\n", reg_ul, ret);
		return ret;
	}

	dev_info(g_bq->dev, "wrote 0x%02lx to reg 0x%02lx\n", val_ul, reg_ul);
	return count;
}

static DEVICE_ATTR_RW(registers);

static struct attribute *bq2589x_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group bq2589x_attr_group = {
	.attrs = bq2589x_attributes,
};

static int bq2589x_parse_dt(struct device *dev, struct bq2589x *bq)
{
	int ret;
	struct device_node *np = dev->of_node;

	// validate of
	if (!np) {
		bq_err("no device tree node\n");
		return -ENODEV;
	}

	bq->irq_gpiod = devm_gpiod_get(dev, "intr", GPIOD_IN);
	if (IS_ERR(bq->irq_gpiod)) {
		ret = PTR_ERR(bq->irq_gpiod);
		bq_err("devm_gpiod_get(intr) failed: %d\n", ret);
		return ret;
	}
	bq_info("intr descriptor acquired\n");

	ret = of_property_read_u32(np, "ti,bq2589x,vbus-volt-high-level",
				   &bq->pe.high_volt_level);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,vbus-volt-low-level",
				   &bq->pe.low_volt_level);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,vbat-min-volt-to-tuneup",
				   &bq->pe.vbat_min_volt);
	if (ret)
		return ret;

	bq->cfg.enable_auto_dpdm = of_property_read_bool(np,
							 "ti,bq2589x,enable-auto-dpdm");
	bq->cfg.enable_term = of_property_read_bool(np,
						    "ti,bq2589x,enable-termination");
	bq->cfg.enable_ico = of_property_read_bool(np,
						   "ti,bq2589x,enable-ico");
	bq->cfg.use_absolute_vindpm = of_property_read_bool(np,
							    "ti,bq2589x,use-absolute-vindpm");

	ret = of_property_read_u32(np, "ti,bq2589x,charge-voltage",
				   &bq->cfg.charge_voltage);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current",
				   &bq->cfg.charge_current);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current-3500",
				   &bq->cfg.charge_current_3500);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current-1500",
				   &bq->cfg.charge_current_1500);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current-1000",
				   &bq->cfg.charge_current_1000);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,charge-current-500",
				   &bq->cfg.charge_current_500);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,input-current-2000",
				   &bq->cfg.input_current_2000);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ti,bq2589x,term-current",
				   &bq->cfg.term_current);
	if (ret)
		return ret;

	return 0;
}

static void bq2589x_usb_switch(struct bq2589x *bq, bool en)
{
	if (!bq || !bq->usb_switch1_gpiod)
		return;

	mutex_lock(&bq->usb_switch_lock);
	if (bq->usb_switch_flag != en) {
		gpiod_set_value_cansleep(bq->usb_switch1_gpiod, en);
		bq->usb_switch_flag = !!gpiod_get_value_cansleep(bq->usb_switch1_gpiod);
		bq_info("usb_switch set to %d\n", bq->usb_switch_flag);
	}
	mutex_unlock(&bq->usb_switch_lock);
}

static int bq2589x_detect_device(struct bq2589x *bq)
{
	int ret;
	u8 data;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_14, &data);
	if (ret == 0) {
		bq->part_no = (data & BQ2589X_PN_MASK) >> BQ2589X_PN_SHIFT;
		bq->revision = (data & BQ2589X_DEV_REV_MASK) >> BQ2589X_DEV_REV_SHIFT;
	}

	return ret;
}

static int bq2589x_read_batt_rsoc(struct bq2589x *bq)
{
	union power_supply_propval ret = {0, };

	if (!bq->batt_psy)
		bq->batt_psy = power_supply_get_by_name("battery");

	if (bq->batt_psy) {
		power_supply_get_property(bq->batt_psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
		return ret.intval;
	}

	return 50;
}

static void bq2589x_adjust_absolute_vindpm(struct bq2589x *bq)
{
	int vbus_volt;
	int vindpm_volt;
	int ret;

	vbus_volt = bq2589x_adc_read_vbus_volt(bq);
	if (vbus_volt < 6000)
		vindpm_volt = (bq->vbus_type == BQ2589X_VBUS_USB_DCP) ? 4500 : 4600;
	else
		vindpm_volt = 8300;

	bq->vbus_volt = vbus_volt;

	ret = bq2589x_set_input_volt_limit(bq, vindpm_volt);
	if (ret < 0)
		bq_err("set absolute vindpm threshold %d failed: %d\n", vindpm_volt, ret);
	else
		bq_info("set absolute vindpm threshold %d successfully\n", vindpm_volt);
}

int main_set_charge_enable(bool en)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	bq_info("charge_enable: %d\n", en);

	return en ? bq2589x_enable_charger(g_bq)
		  : bq2589x_disable_charger(g_bq);
}
EXPORT_SYMBOL_GPL(main_set_charge_enable);

int main_set_hiz_mode(bool en)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	return en ? bq2589x_enter_hiz_mode(g_bq)
		  : bq2589x_exit_hiz_mode(g_bq);
}
EXPORT_SYMBOL_GPL(main_set_hiz_mode);

int main_set_input_current_limit(int curr)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	return bq2589x_set_input_current_limit(g_bq, curr);
}
EXPORT_SYMBOL_GPL(main_set_input_current_limit);

int main_set_charge_current(int curr)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	bq_info("charge_current: %d\n", curr);
	vote(g_bq->fcc_votable, MAIN_SET_VOTER, true, curr);

	return 0;
}
EXPORT_SYMBOL_GPL(main_set_charge_current);

int main_get_charge_type(void)
{
	if (IS_ERR_OR_NULL(g_bq))
		return -EINVAL;

	/* TODO: protect shared field with lock */
	return g_bq->vbus_type;
}
EXPORT_SYMBOL_GPL(main_get_charge_type);

static void bq2589x_adapter_in_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_in_work);
	int ret;
	union power_supply_propval propval = {0, };

	bq_ws_get(bq);
	switch (bq->vbus_type) {
	case BQ2589X_VBUS_MAXC:
		bq_info("charger_type: MAXC\n");
		bq2589x_enable_ico(bq, !bq->cfg.enable_ico);
		bq2589x_set_input_volt_limit(bq, 8300);
		vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, 1800);
		bq2589x_usb_switch(bq, true);
		break;

	case BQ2589X_VBUS_USB_DCP:
		bq_info("charger_type: DCP, pd_active=%d\n", bq->pd_active);
		vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, bq->cfg.input_current_2000);
		schedule_delayed_work(&bq->check_pe_tuneup_work, 0);
		bq2589x_usb_switch(bq, true);
		break;

	case BQ2589X_VBUS_USB_CDP:
		bq_info("charger_type: CDP\n");
		vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, 1500);
		msleep(1000);
		bq2589x_usb_switch(bq, false);
		break;

	case BQ2589X_VBUS_USB_SDP:
		bq_info("charger_type: SDP, pd_active=%d\n", bq->pd_active);
		if (!bq->usb_psy)
			bq->usb_psy = power_supply_get_by_name("usb");
		if (bq->usb_psy) {
			ret = power_supply_get_property(bq->usb_psy,
							POWER_SUPPLY_PROP_MTBF_CURRENT,
							&propval);
			if (ret < 0)
				bq_err("get mtbf current fail\n");
		}

		if (!bq->pd_active) {
			vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true,
			     (propval.intval >= 1500) ? propval.intval : 500);
		} else {
			vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, 1500);
		}
		bq2589x_usb_switch(bq, false);
		break;

	case BQ2589X_VBUS_NONSTAND:
	case BQ2589X_VBUS_UNKNOWN:
		bq_info("charger_type: FLOAT, pd_active=%d\n", bq->pd_active);
		if (!bq->usb_psy)
			bq->usb_psy = power_supply_get_by_name("usb");
		if (bq->usb_psy) {
			ret = power_supply_get_property(bq->usb_psy,
							POWER_SUPPLY_PROP_MTBF_CURRENT,
							&propval);
			if (ret < 0)
				bq_err("get mtbf current fail\n");
		}

		if (!bq->pd_active) {
			vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true,
			     (propval.intval >= 1500) ? propval.intval : 1000);
		} else {
			vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true,
			     bq->cfg.input_current_2000);
		}
		bq2589x_usb_switch(bq, false);
		break;

	default:
		bq_info("charger_type: Other, vbus_type is %d\n", bq->vbus_type);
		bq2589x_usb_switch(bq, false);
		schedule_delayed_work(&bq->ico_work, 0);
		break;
	}

	if (bq->vbus_type == BQ2589X_VBUS_USB_SDP && !bq->pd_active)
		vote(bq->fcc_votable, MAIN_SET_VOTER, true, MAIN_FCC_MIN);
	else
		vote(bq->fcc_votable, MAIN_SET_VOTER, false, 0);

	schedule_delayed_work(&bq->monitor_work, 0);
	bq_ws_put(bq);
}

static void bq2589x_adapter_out_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_out_work);
	int ret;

	bq_ws_get(bq);
	ret = bq2589x_set_input_volt_limit(bq, 4600);
	if (ret < 0)
		bq_err("reset vindpm threshold to 4600 failed: %d\n", ret);
	else
		bq_info("reset vindpm threshold to 4600 successfully\n");

	vote(bq->fcc_votable, MAIN_SET_VOTER, true, MAIN_FCC_MIN);
	vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, MAIN_ICL_MIN);

	cancel_delayed_work_sync(&bq->monitor_work);
	bq_ws_put(bq);
}

static void bq2589x_charger_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, charger_work.work);
	u8 type_now;

	if (!bq->batt_psy)
		return;

	type_now = bq2589x_get_charging_status(bq);
	if (type_now > 0)
		power_supply_changed(bq->batt_psy);

	bq_info("type_now: %d\n", type_now);
}

static void bq2589x_ico_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, ico_work.work);
	int ret;
	int idpm;
	u8 status;
	static bool ico_issued;

	if (bq->part_no == SYV690) {
		bq_info("SYV690 IC detected, skip ico\n");
		return;
	}

	bq_ws_get(bq);
	if (!ico_issued) {
		ret = bq2589x_force_ico(bq);
		if (ret < 0) {
			schedule_delayed_work(&bq->ico_work, HZ);
		} else {
			ico_issued = true;
			schedule_delayed_work(&bq->ico_work, 3 * HZ);
		}
	} else {
		ico_issued = false;
		ret = bq2589x_check_force_ico_done(bq);
		if (ret == 1) {
			ret = bq2589x_read_byte(bq, BQ2589X_REG_13, &status);
			if (ret == 0)
				idpm = ((status & BQ2589X_IDPM_LIM_MASK) >>
					BQ2589X_IDPM_LIM_SHIFT) *
					BQ2589X_IDPM_LIM_LSB + BQ2589X_IDPM_LIM_BASE;
		}
	}
	bq_ws_put(bq);
}

static void bq2589x_check_pe_tuneup_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, check_pe_tuneup_work.work);

	if (!bq->pe.enable) {
		schedule_delayed_work(&bq->ico_work, 0);
		return;
	}

	bq_ws_get(bq);
	bq->vbat_volt = bq2589x_adc_read_battery_volt(bq);
	bq->rsoc = bq2589x_read_batt_rsoc(bq);

	if (bq->vbat_volt > bq->pe.vbat_min_volt && bq->rsoc < 95) {
		bq->pe.target_volt = bq->pe.high_volt_level;
		bq->pe.tune_up_volt = true;
		bq->pe.tune_down_volt = false;
		bq->pe.tune_done = false;
		bq->pe.tune_count = 0;
		bq->pe.tune_fail = false;
		schedule_delayed_work(&bq->tune_volt_work, 0);
	} else if (bq->rsoc >= 95) {
		schedule_delayed_work(&bq->ico_work, 0);
	} else {
		/* wait battery voltage up enough to check again */
		schedule_delayed_work(&bq->check_pe_tuneup_work, 2 * HZ);
	}
	bq_ws_put(bq);
}

//20220211 : Only for time delay
static void bq2589x_time_delay_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, time_delay_work.work);
	int rc;
	u8 status;

	if (bq->vbus_type != BQ2589X_VBUS_OTG) {
		bq2589x_usb_switch(bq, true);
		bq->vbus_type = bq2589x_get_vbus_valid(bq);
	}

	if (bq->vbus_type == BQ2589X_VBUS_UNKNOWN) {
		bq2589x_usb_switch(bq, false);
		return;
	}

	rc = bq2589x_read_byte(bq, BQ2589X_REG_13, &status);
	if (rc == 0 &&
	    (status & BQ2589X_VDPM_STAT_MASK) &&
	    !bq->pd_active) {
		if (bq->vbus_type == BQ2589X_VBUS_MAXC && bq->vbus_volt < 8000) {
			bq_info("HVDCP VINDPM occurred, vbus: %d, reset vindpm!\n",
				bq->vbus_volt);
			bq2589x_adjust_absolute_vindpm(bq);
		}
	}
}

static void bq2589x_usb_changed_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, usb_changed_work.work);
	union power_supply_propval val = {0, };
	int chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	static int last_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	static int no_change_count;
	static bool last_valid;
	static unsigned long last_jiffies;

	bq_ws_get(bq);
	if (last_jiffies && time_after(jiffies, last_jiffies + RESET_GAP)) {
		no_change_count = 0;
		last_valid = false;
		last_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	}
	last_jiffies = jiffies;

	if (!bq->usb_psy) {
		bq->usb_psy = power_supply_get_by_name("usb");
		if (!bq->usb_psy) {
			bq_err("fail to get usb_psy\n");
			goto out;
		}
	}

	if (power_supply_get_property(bq->usb_psy,
				      POWER_SUPPLY_PROP_REAL_TYPE, &val) == 0)
		chg_type = val.intval;

	if (!last_valid || chg_type != last_chg_type) {
		last_chg_type = chg_type;
		last_valid = true;
		no_change_count = NO_CHANGE_MAX;
	}

	if (no_change_count > 0) {
		if (bq->usb_psy)
			power_supply_changed(bq->usb_psy);
		if (bq->wall_psy)
			power_supply_changed(bq->wall_psy);

		no_change_count--;
		if (no_change_count > 0)
			schedule_delayed_work(&bq->usb_changed_work, POST_INTERVAL);
		else
			last_valid = false;
	} else {
		last_valid = false;
	}

out:
	bq_ws_put(bq);
}

static void bq2589x_tune_volt_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, tune_volt_work.work);
	int ret = 0;
	static bool pumpx_cmd_issued;

	bq_ws_get(bq);
	bq->vbus_volt = bq2589x_adc_read_vbus_volt(bq);

	if ((bq->pe.tune_up_volt && bq->vbus_volt > bq->pe.target_volt) ||
	    (bq->pe.tune_down_volt && bq->vbus_volt < bq->pe.target_volt)) {
		bq->pe.tune_done = true;
		bq2589x_adjust_absolute_vindpm(bq);
		if (bq->pe.tune_up_volt)
			schedule_delayed_work(&bq->ico_work, 0);
		goto out;
	}

	if (bq->pe.tune_count > 10) {
		bq->pe.tune_fail = true;
		bq2589x_adjust_absolute_vindpm(bq);
		if (bq->pe.tune_up_volt)
			schedule_delayed_work(&bq->ico_work, 0);
		goto out;
	}

	if (!pumpx_cmd_issued) {
		if (bq->pe.tune_up_volt)
			ret = bq2589x_pumpx_increase_volt(bq);
		else if (bq->pe.tune_down_volt)
			ret = bq2589x_pumpx_decrease_volt(bq);

		if (ret < 0) {
			schedule_delayed_work(&bq->tune_volt_work, HZ);
		} else {
			pumpx_cmd_issued = true;
			bq->pe.tune_count++;
			schedule_delayed_work(&bq->tune_volt_work, 3 * HZ);
		}
	} else {
		if (bq->pe.tune_up_volt)
			ret = bq2589x_pumpx_increase_volt_done(bq);
		else if (bq->pe.tune_down_volt)
			ret = bq2589x_pumpx_decrease_volt_done(bq);

		if (ret == 0) {
			bq2589x_adjust_absolute_vindpm(bq);
			pumpx_cmd_issued = false;
		}

		schedule_delayed_work(&bq->tune_volt_work, HZ);
	}

out:
	bq_ws_put(bq);
}

static void bq2589x_monitor_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, monitor_work.work);
	u8 status = 0;
	int ret = 0;
	int rawfcc = 0, rawfv = 0, rawicl = 0;
	int batt_temp = 0;
	union power_supply_propval propval = {0, };

	//bq2589x_dump_regs(bq);
	bq2589x_reset_watchdog_timer(bq);
	bq->rsoc = bq2589x_read_batt_rsoc(bq);
	bq->vbus_volt = bq2589x_adc_read_vbus_volt(bq);
	bq->vbat_volt = bq2589x_adc_read_battery_volt(bq);
	bq->chg_current = bq2589x_adc_read_charge_current(bq);
	rawfcc = bq2589x_get_charge_current(bq);
	rawfv = bq2589x_get_chargevoltage(bq);
	rawicl = bq2589x_get_input_current_limit(bq);

	if (!bq->bms_psy)
		bq->bms_psy = power_supply_get_by_name("bms");
	if (bq->bms_psy) {
		ret = power_supply_get_property(bq->bms_psy,
						POWER_SUPPLY_PROP_TEMP, &propval);
		if (ret < 0) {
			bq_err("get battery temp fail\n");
			batt_temp = 250;
		} else {
			batt_temp = propval.intval;
		}
		bq_info("batt_temp: %d\n", (batt_temp / 10));
	}

	if (batt_temp < 0) {
		bq2589x_update_bits(bq, BQ2589X_REG_06,
				    BQ2589X_VRECHG_MASK,
				    BQ2589X_VRECHG_200MV << BQ2589X_VRECHG_SHIFT);
	} else {
		bq2589x_update_bits(bq, BQ2589X_REG_06,
				    BQ2589X_VRECHG_MASK,
				    BQ2589X_VRECHG_100MV << BQ2589X_VRECHG_SHIFT);
	}

	ret = bq2589x_read_byte(bq, BQ2589X_REG_13, &status);
	if (ret == 0 && (status & BQ2589X_VDPM_STAT_MASK))
		bq_info("VINDPM occurred\n");
	if (ret == 0 && (status & BQ2589X_IDPM_STAT_MASK))
		bq_info("IINDPM occurred\n");

	if (bq->vbus_type == BQ2589X_VBUS_USB_DCP &&
	    bq->vbus_volt > bq->pe.high_volt_level &&
	    bq->rsoc > 95 && !bq->pe.tune_down_volt) {
		bq->pe.tune_down_volt = true;
		bq->pe.tune_up_volt = false;
		bq->pe.target_volt = bq->pe.low_volt_level;
		bq->pe.tune_done = false;
		bq->pe.tune_count = 0;
		bq->pe.tune_fail = false;
		schedule_delayed_work(&bq->tune_volt_work, 0);
	}

	switch (bq->vbus_type) {
	case BQ2589X_VBUS_MAXC:
		bq2589x_enable_ico(bq, false);
		/* fall through */
	case BQ2589X_VBUS_USB_DCP:
		if (rawicl > 2000)
			rerun_election(bq->usb_icl_votable);
		/* fall through */
	case BQ2589X_VBUS_USB_SDP:
	case BQ2589X_VBUS_USB_CDP:
	case BQ2589X_VBUS_NONSTAND:
	case BQ2589X_VBUS_UNKNOWN:
		if (rawfcc > get_effective_result_locked(bq->fcc_votable))
			rerun_election(bq->fcc_votable);
		if (rawfv > get_effective_result_locked(bq->fv_votable))
			rerun_election(bq->fv_votable);
		break;
	default:
		bq_info("unhandled vbus_type: %d\n", bq->vbus_type);
		break;
	}

	schedule_delayed_work(&bq->monitor_work, 10 * HZ);
}

static void bq2589x_start_charging_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, start_charging_work);
	struct sm_fg_chip *sm = fg_get_sm("sm-bms");
	int last_status = POWER_SUPPLY_STATUS_UNKNOWN;
	int status;
	unsigned long timeout_jiffies;

	if (!bq || !sm)
		return;

	if (!bq->bms_psy)
		bq->bms_psy = power_supply_get_by_name("bms");
	if (!bq->batt_psy)
		bq->batt_psy = power_supply_get_by_name("battery");

	if (!bq->bms_psy || !bq->batt_psy)
		return;

	bq_ws_get(bq);
	bq2589x_enable_charger(bq);

	stop_fg_monitor(sm);

	timeout_jiffies = jiffies + msecs_to_jiffies(10000);
	while (time_before(jiffies, timeout_jiffies)) {
		status = bq2589x_get_charging_status(bq);
		bq_info("waiting for charging: status=%d, last=%d\n",
			status, last_status);
		if (status != last_status) {
			last_status = status;
			power_supply_changed(bq->batt_psy);
		}

		if (status == POWER_SUPPLY_STATUS_CHARGING) {
			power_supply_changed(bq->bms_psy);
			break;
		}

		msleep(200);
	}

	start_fg_monitor(sm);

	fg_put_sm(sm);
	bq_ws_put(bq);
}

static int bq2589x_set_charger_type(struct bq2589x *bq,
				    enum power_supply_type chg_type)
{
	union power_supply_propval propval = {0, };
	bool online = (chg_type != POWER_SUPPLY_TYPE_UNKNOWN);
	int ret;

	WRITE_ONCE(bq->chg_online, online);
	WRITE_ONCE(bq->chg_type, chg_type);

	if (!bq->usb_psy) {
		bq->usb_psy = power_supply_get_by_name("usb");
		if (!bq->usb_psy) {
			bq_err("fail to get usb_psy\n");
			return -ENODEV;
		}
	}

	if (bq->pd_active && !online) {
		//fix CtoC disconnection
	} else {
		propval.intval = online;
		ret = power_supply_set_property(bq->usb_psy,
						POWER_SUPPLY_PROP_ONLINE, &propval);
		if (ret < 0)
			bq_err("inform power supply usb_online fail, ret=%d\n", ret);
	}

	bq_info("chg_type = %d\n", chg_type);
	propval.intval = chg_type;

	ret = power_supply_set_property(bq->usb_psy,
					POWER_SUPPLY_PROP_REAL_TYPE, &propval);
	if (ret < 0)
		bq_err("set prop REAL_TYPE fail, ret=%d\n", ret);

	power_supply_changed(bq->usb_psy);
	return ret;
}

static enum power_supply_type bq2589x_get_charger_type(struct bq2589x *bq)
{
	switch (bq->vbus_type) {
	case BQ2589X_VBUS_NONE:
		bq_info("charger_type: NONE\n");
		return POWER_SUPPLY_TYPE_UNKNOWN;
	case BQ2589X_VBUS_MAXC:
		bq_info("charger_type: HVDCP/Maxcharge\n");
		if (bq->part_no == SC89890H)
			bq2589x_write_byte(bq, BQ2589X_REG_01, 0xC9);
		return POWER_SUPPLY_TYPE_USB_HVDCP;
	case BQ2589X_VBUS_USB_DCP:
		bq_info("charger_type: DCP\n");
		return POWER_SUPPLY_TYPE_USB_DCP;
	case BQ2589X_VBUS_USB_CDP:
		bq_info("charger_type: CDP\n");
		return POWER_SUPPLY_TYPE_USB_CDP;
	case BQ2589X_VBUS_USB_SDP:
		bq_info("charger_type: SDP\n");
		return POWER_SUPPLY_TYPE_USB;
	case BQ2589X_VBUS_NONSTAND:
		bq_info("charger_type: FLOAT\n");
		/* fall through */
	case BQ2589X_VBUS_UNKNOWN:
		bq_info("charger_type: UNKNOWN\n");
		return POWER_SUPPLY_TYPE_USB_FLOAT;
	case BQ2589X_VBUS_OTG:
		bq_info("charger_type: OTG\n");
		return POWER_SUPPLY_TYPE_UNKNOWN;
	default:
		bq_info("charger_type: Other, vbus_type is %d\n", bq->vbus_type);
		return POWER_SUPPLY_TYPE_USB_FLOAT;
	}
}

static void bq2589x_handle_event(struct bq2589x *bq)
{
	u8 status = 0, fault = 0;
	u8 vbus_status = 0, pg_status = 0;
	int ret;
	enum power_supply_type chg_type;

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0B, &status);
	if (ret)
		return;

	bq->vbus_type = (status & BQ2589X_VBUS_STAT_MASK) >> BQ2589X_VBUS_STAT_SHIFT;
	pg_status = (status & BQ2589X_PG_STAT_MASK) >> BQ2589X_PG_STAT_SHIFT;
	if (!pg_status)
		bq->vbus_type = BQ2589X_VBUS_NONE;

	chg_type = bq2589x_get_charger_type(bq);
	bq2589x_set_charger_type(bq, chg_type);

	ret = bq2589x_read_byte(bq, BQ2589X_REG_0C, &fault);
	if (ret)
		return;

	bq_info("status=%02x, vbus=%d, chg_type=%d, fault=0x%02x\n",
		status, bq->vbus_type, chg_type, fault);

	if (bq->part_no == SC89890H) {
		ret = bq2589x_read_byte(bq, BQ2589X_REG_11, &vbus_status);
		if (ret)
			return;

		if (!(vbus_status & BQ2589X_VBUS_GD_MASK) &&
		    (bq->status & BQ2589X_STATUS_PLUGIN)) {
			bq2589x_usb_switch(bq, true);
			bq2589x_adc_stop(bq);
			bq->status &= ~BQ2589X_STATUS_PLUGIN;
			schedule_work(&bq->adapter_out_work);
			bq_info("adapter removed\n");
			schedule_delayed_work(&bq->charger_work, 0);
		} else if (bq->vbus_type != BQ2589X_VBUS_NONE &&
		    bq->vbus_type != BQ2589X_VBUS_OTG &&
		    !(bq->status & BQ2589X_STATUS_PLUGIN)) {
			bq->status |= BQ2589X_STATUS_PLUGIN;
			bq2589x_use_absolute_vindpm(bq, bq->cfg.use_absolute_vindpm);
			bq2589x_adc_start(bq, false);
			if (bq->cfg.use_absolute_vindpm)
				bq2589x_adjust_absolute_vindpm(bq);
			bq2589x_update_bits(bq, BQ2589X_REG_00,
					    BQ2589X_ENILIM_MASK,
					    BQ2589X_ENILIM_DISABLE << BQ2589X_ENILIM_SHIFT);
			schedule_delayed_work(&bq->usb_changed_work, 0);
			schedule_work(&bq->adapter_in_work);
			bq_info("adapter plugged in\n");
			schedule_delayed_work(&bq->charger_work, 100);
			schedule_work(&bq->start_charging_work);
		}
	} else {
		if ((bq->vbus_type == BQ2589X_VBUS_NONE ||
		     bq->vbus_type == BQ2589X_VBUS_OTG) &&
		    (bq->status & BQ2589X_STATUS_PLUGIN)) {
			bq2589x_usb_switch(bq, true);
			bq2589x_adc_stop(bq);
			bq->status &= ~BQ2589X_STATUS_PLUGIN;
			schedule_work(&bq->adapter_out_work);
			bq_info("adapter removed\n");
			schedule_delayed_work(&bq->charger_work, 0);
		} else if (bq->vbus_type != BQ2589X_VBUS_NONE &&
		    bq->vbus_type != BQ2589X_VBUS_OTG &&
		    !(bq->status & BQ2589X_STATUS_PLUGIN)) {
			bq->status |= BQ2589X_STATUS_PLUGIN;
			bq2589x_use_absolute_vindpm(bq, bq->cfg.use_absolute_vindpm);
			bq2589x_adc_start(bq, false);
			if (bq->cfg.use_absolute_vindpm)
				bq2589x_adjust_absolute_vindpm(bq);
			bq2589x_update_bits(bq, BQ2589X_REG_00,
					    BQ2589X_ENILIM_MASK,
					    BQ2589X_ENILIM_DISABLE << BQ2589X_ENILIM_SHIFT);
			schedule_delayed_work(&bq->usb_changed_work, 0);
			schedule_work(&bq->adapter_in_work);
			bq_info("adapter plugged in\n");
			schedule_delayed_work(&bq->charger_work, 100);
			schedule_work(&bq->start_charging_work);
		}
	}

	/* Update PG status */
	if ((status & BQ2589X_PG_STAT_MASK) &&
	    !(bq->status & BQ2589X_STATUS_PG))
		bq->status |= BQ2589X_STATUS_PG;
	else if (!(status & BQ2589X_PG_STAT_MASK) &&
		 (bq->status & BQ2589X_STATUS_PG))
		bq->status &= ~BQ2589X_STATUS_PG;

	/* Update fault status */
	if (fault && !(bq->status & BQ2589X_STATUS_FAULT))
		bq->status |= BQ2589X_STATUS_FAULT;
	else if (!fault && (bq->status & BQ2589X_STATUS_FAULT))
		bq->status &= ~BQ2589X_STATUS_FAULT;

	/* Bit 6: boost fault - disable OTG */
	if (fault & 0x40) {
		bq2589x_usb_switch(bq, true);
		bq2589x_set_otg(bq, false);
	}

	/* Bit 7: watchdog fault - reset chip */
	if (fault & 0x80) {
		bq2589x_reset_chip(bq);
		usleep_range(4500, 5500);
		bq2589x_init_device(bq);
	}
}

static irqreturn_t bq2589x_hard_irq(int irq, void *data)
{
	struct bq2589x *bq = data;

	if (!bq || !bq->usb_switch1_gpiod)
		return IRQ_NONE;

	bq_ws_get(bq);
	atomic_set(&bq->irq_pending, 1);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t bq2589x_thread_irq(int irq, void *data)
{
	struct bq2589x *bq = data;

	mutex_lock(&bq->irq_complete);

	if (atomic_read(&bq->shutting_down)) {
		mutex_unlock(&bq->irq_complete);
		goto out_relax;
	}

	if (!atomic_read(&bq->resume_completed)) {
		if (!atomic_read(&bq->irq_disabled)) {
			disable_irq_nosync(bq->irq);
			atomic_set(&bq->irq_disabled, 1);
		}
		mutex_unlock(&bq->irq_complete);
		goto out_relax;
	}

	mutex_unlock(&bq->irq_complete);
	bq2589x_handle_event(bq);
	atomic_set(&bq->irq_pending, 0);

out_relax:
	bq_ws_put(bq);
	return IRQ_HANDLED;
}

static void determine_initial_status(struct bq2589x *bq)
{
	if (bq)
		bq2589x_handle_event(bq);
}

#if defined(CONFIG_TCPC_RT1711H)
static int bq2589x_set_fast_charge_mode(struct bq2589x *bq, int pd_active)
{
	union power_supply_propval propval = {0, };
	int batt_verify = 0, batt_soc = 0, batt_temp = 0, rc = 0;

	if (!bq->bms_psy) {
		bq->bms_psy = power_supply_get_by_name("bms");
		if (!bq->bms_psy) {
			bq_err("bms_psy not found\n");
			return -ENOENT;
		}
	}

	rc = power_supply_get_property(bq->bms_psy,
				       POWER_SUPPLY_PROP_CHIP_OK, &propval);
	if (rc < 0)
		bq_err("get battery chip ok fail\n");
	else
		batt_verify = propval.intval;

	rc = power_supply_get_property(bq->bms_psy,
				       POWER_SUPPLY_PROP_CAPACITY, &propval);
	if (rc < 0)
		bq_err("get battery capacity fail\n");
	else
		batt_soc = propval.intval;

	rc = power_supply_get_property(bq->bms_psy,
				       POWER_SUPPLY_PROP_TEMP, &propval);
	if (rc < 0)
		bq_err("get battery temp fail\n");
	else
		batt_temp = propval.intval;

	if (pd_active == 2 && batt_verify && batt_soc < 95) {
		g_ffc_disable = false;
		propval.intval = (batt_temp >= 150 && batt_temp <= 480) ? 1 : 0;
	} else {
		propval.intval = 0;
		g_ffc_disable = true;
	}

	rc = power_supply_set_property(bq->bms_psy,
				       POWER_SUPPLY_PROP_FASTCHARGE_MODE, &propval);
	if (rc < 0)
		bq_err("set fastcharge mode fail!\n");

	return rc;
}

static void set_pd_active(struct bq2589x *bq, int pd_active)
{
	int rc = 0;
	union power_supply_propval val = {0, };

	if (!bq->usb_psy) {
		bq->usb_psy = power_supply_get_by_name("usb");
		if (!bq->usb_psy) {
			bq_err("fail to get usb_psy\n");
			return;
		}
	}

	if (pd_active)
		bq2589x_set_charger_type(bq,
					 POWER_SUPPLY_TYPE_USB_PD);
	else
		bq2589x_set_charger_type(bq,
					 POWER_SUPPLY_TYPE_UNKNOWN);

	bq->pd_active = pd_active;
	val.intval = pd_active;
	rc = power_supply_set_property(bq->usb_psy,
				       POWER_SUPPLY_PROP_PD_ACTIVE, &val);
	if (rc < 0)
		bq_err("Couldn't set USB PD_ACTIVE status, rc=%d\n", rc);

	bq2589x_set_fast_charge_mode(bq, pd_active);
}

static int get_source_mode(struct tcp_notify *noti)
{
	if (noti->typec_state.new_state == TYPEC_ATTACHED_CUSTOM_SRC ||
	    noti->typec_state.new_state == TYPEC_ATTACHED_NORP_SRC ||
	    noti->typec_state.new_state == TYPEC_ATTACHED_DBGACC_SNK)
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;

	switch (noti->typec_state.rp_level) {
	case TYPEC_CC_VOLT_SNK_1_5:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case TYPEC_CC_VOLT_SNK_3_0:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	case TYPEC_CC_VOLT_SNK_DFT:
	default:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int bq2589x_set_cc_orientation(struct bq2589x *bq, int cc_orientation)
{
	int ret = 0;
	union power_supply_propval propval = {0, };

	if (!bq->usb_psy) {
		bq->usb_psy = power_supply_get_by_name("usb");
		if (!bq->usb_psy) {
			bq_err("fail to get usb_psy\n");
			return -ENODEV;
		}
	}

	propval.intval = cc_orientation;
	ret = power_supply_set_property(bq->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION, &propval);
	if (ret < 0)
		bq_err("set prop CC_ORIENTATION fail: (%d)\n", ret);

	return ret;
}

static int bq2589x_set_typec_mode(struct bq2589x *bq,
				  enum power_supply_typec_mode typec_mode)
{
	int ret = 0;
	union power_supply_propval propval = {0, };

	if (!bq->usb_psy) {
		bq->usb_psy = power_supply_get_by_name("usb");
		if (!bq->usb_psy) {
			bq_err("fail to get usb_psy\n");
			return -ENODEV;
		}
	}

	propval.intval = typec_mode;
	ret = power_supply_set_property(bq->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_MODE, &propval);
	if (ret < 0)
		bq_err("set prop TYPEC_MODE fail: (%d)\n", ret);

	return ret;
}

static int pd_tcp_notifier_call(struct notifier_block *pnb,
				unsigned long event, void *data)
{
	struct tcp_notify *noti = data;
	struct bq2589x *bq = container_of(pnb, struct bq2589x, pd_nb);
	enum power_supply_typec_mode typec_mode = POWER_SUPPLY_TYPEC_NONE;
	int rc, cc_orientation = 0;
	u8 status;

	switch (event) {
	case TCP_NOTIFY_SINK_VBUS:
		bq_log("TCP_NOTIFY_SINK_VBUS\n");
		break;
	case TCP_NOTIFY_PD_STATE:
		bq_info("noti->pd_state connected: %d\n", noti->pd_state.connected);
		bq_ws_get(bq);
		switch (noti->pd_state.connected) {
		case PD_CONNECT_NONE:
			bq_info("disconnected\n");
			break;
		case PD_CONNECT_HARD_RESET:
			bq_info("hardreset\n");
			if (bq->pd_active)
				set_pd_active(bq, 0);
			break;
		case PD_CONNECT_PE_READY_SNK:
			bq_info("PD2.0 connect\n");
			set_pd_active(bq, 1);
			break;
		case PD_CONNECT_PE_READY_SNK_PD30:
			bq_info("PD3.0 connect\n");
			set_pd_active(bq, 1);
			break;
		case PD_CONNECT_PE_READY_SNK_APDO:
			bq_info("PPS connect\n");
			set_pd_active(bq, 2);
			break;
		case PD_CONNECT_TYPEC_ONLY_SNK_DFT:
		case PD_CONNECT_TYPEC_ONLY_SNK:
			rc = bq2589x_read_byte(bq, BQ2589X_REG_13, &status);
			if (rc == 0 &&
			    (status & BQ2589X_VDPM_STAT_MASK) &&
			    !bq->pd_active) {
				if (bq->vbus_type == BQ2589X_VBUS_MAXC &&
				    bq->vbus_volt < 8000) {
					bq_info("HVDCP VINDPM occurred, vbus: %d, reset vindpm!\n",
						bq->vbus_volt);
					bq2589x_usb_switch(bq, true);
					schedule_delayed_work(&bq->time_delay_work,
							      msecs_to_jiffies(3000));
				}
			}
			break;
		}
		bq_ws_put(bq);
		break;
	case TCP_NOTIFY_TYPEC_STATE:
		bq_ws_get(bq);
		/*
		 * polarity is only meaningful when attached.
		 * false(0) = CC1, true(1) = CC2 -> map to 1/2 per
		 * POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION (0=N/C, 1=CC1, 2=CC2).
		 * Evaluated here but only consumed inside attached branches;
		 * plug-out branches explicitly pass 0 (N/C) instead.
		 */
		cc_orientation = noti->typec_state.polarity ? 2 : 1;
		if (noti->typec_state.old_state == TYPEC_UNATTACHED &&
		    (noti->typec_state.new_state == TYPEC_ATTACHED_SNK ||
		     noti->typec_state.new_state == TYPEC_ATTACHED_CUSTOM_SRC ||
		     noti->typec_state.new_state == TYPEC_ATTACHED_NORP_SRC ||
		     noti->typec_state.new_state == TYPEC_ATTACHED_DBGACC_SNK)) {
			bq_info("USB Plug in, pol = %d, state = %d\n",
				cc_orientation, noti->typec_state.new_state);
			if (!bq->otg_attached &&
			    (bq->vbus_type == BQ2589X_VBUS_NONE ||
			     bq->vbus_type == BQ2589X_VBUS_UNKNOWN)) {
				bq2589x_init_device(bq);
				bq2589x_usb_switch(bq, true);
				/*
				 * On driven, instead only do DPDM handshake,
				 * find determined vbus_type, its call DPDM handshake too
				 */
				bq->vbus_type = bq2589x_get_vbus_valid(bq);
			}
			typec_mode = get_source_mode(noti);
			bq2589x_set_cc_orientation(bq, cc_orientation);
		} else if (noti->typec_state.old_state == TYPEC_UNATTACHED &&
			   (noti->typec_state.new_state == TYPEC_ATTACHED_SRC ||
			    noti->typec_state.new_state == TYPEC_ATTACHED_DEBUG)) {
			bq2589x_usb_switch(bq, false);
			bq->otg_attached = true;
			typec_mode = (noti->typec_state.new_state == TYPEC_ATTACHED_DEBUG) ?
				     POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY :
				     POWER_SUPPLY_TYPEC_SINK;
			bq2589x_set_cc_orientation(bq, cc_orientation);
			bq_info("OTG Type-C Plug in, pol = %d\n", cc_orientation);
		} else if (noti->typec_state.old_state == TYPEC_UNATTACHED &&
			   noti->typec_state.new_state == TYPEC_ATTACHED_AUDIO) {
			bq_info("Audio Accessory plug in\n");
			typec_mode = POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
		} else if (noti->typec_state.old_state == TYPEC_ATTACHED_AUDIO &&
			   noti->typec_state.new_state == TYPEC_UNATTACHED) {
			bq_info("Audio Accessory plug out\n");
			typec_mode = POWER_SUPPLY_TYPEC_NONE;
		} else if ((noti->typec_state.old_state == TYPEC_ATTACHED_SRC ||
			    noti->typec_state.old_state == TYPEC_ATTACHED_DEBUG) &&
			   noti->typec_state.new_state == TYPEC_UNATTACHED) {
			typec_mode = POWER_SUPPLY_TYPEC_NONE;
			bq->otg_attached = false;
			bq2589x_set_cc_orientation(bq, 0);
			bq_info("OTG Type-C Plug out\n");
			bq2589x_usb_switch(bq, true);
		} else if ((noti->typec_state.old_state == TYPEC_ATTACHED_SNK ||
			    noti->typec_state.old_state == TYPEC_ATTACHED_CUSTOM_SRC ||
			    noti->typec_state.old_state == TYPEC_ATTACHED_NORP_SRC ||
			    noti->typec_state.old_state == TYPEC_ATTACHED_DBGACC_SNK) &&
			   noti->typec_state.new_state == TYPEC_UNATTACHED) {
			bq_info("USB Plug out\n");
			typec_mode = POWER_SUPPLY_TYPEC_NONE;
			bq2589x_set_cc_orientation(bq, 0);
			bq2589x_usb_switch(bq, false);
		} else if (noti->typec_state.old_state == TYPEC_ATTACHED_SRC &&
			   noti->typec_state.new_state == TYPEC_ATTACHED_SNK) {
			bq->otg_attached = false;
			typec_mode = get_source_mode(noti);
			bq_info("Source_to_Sink, pol = %d\n", cc_orientation);
			bq2589x_set_cc_orientation(bq, cc_orientation);
		} else if (noti->typec_state.old_state == TYPEC_ATTACHED_SNK &&
			   noti->typec_state.new_state == TYPEC_ATTACHED_SRC) {
			bq->otg_attached = true;
			typec_mode = POWER_SUPPLY_TYPEC_SINK;
			bq_info("Sink_to_Source, pol = %d\n", cc_orientation);
			bq2589x_set_cc_orientation(bq, cc_orientation);
		}
		if (typec_mode >= POWER_SUPPLY_TYPEC_NONE &&
		    typec_mode <= POWER_SUPPLY_TYPEC_NON_COMPLIANT)
			bq2589x_set_typec_mode(bq, typec_mode);
		bq_ws_put(bq);
		break;
	case TCP_NOTIFY_EXT_DISCHARGE:
		bq_info("Ext discharge = %d\n", noti->en_state.en);
		if (noti->en_state.en) {
			bq2589x_usb_switch(bq, false);
		} else {
			bq2589x_usb_switch(bq, true);
			set_pd_active(bq, 0);
		}
		break;
	case TCP_NOTIFY_SOURCE_VBUS:
		bq_log("TCP_NOTIFY_SOURCE_VBUS\n");
		if (noti->vbus_state.mv == TCPC_VBUS_SOURCE_0V && bq->vbus_on) {
			bq_info("OTG VBUS Disabled\n");
			bq->vbus_on = false;
			bq2589x_set_otg(bq, false);
			bq2589x_usb_switch(bq, true);
		} else if (noti->vbus_state.mv == TCPC_VBUS_SOURCE_5V && !bq->vbus_on) {
			bq_info("OTG VBUS Enabled\n");
			bq->vbus_on = true;
			bq2589x_usb_switch(bq, false);
			bq2589x_set_otg(bq, true);
			bq2589x_set_otg_current(bq, bq->cfg.charge_current_1500);
		}
		break;
	}

	return NOTIFY_OK;
}
#endif

static int fcc_vote_callback(struct votable *votable, void *data,
			     int fcc_ma, const char *client)
{
	struct bq2589x *bq = data;
	int rc;

	bq_info("fcc: %d\n", fcc_ma);

	if (fcc_ma < 0)
		return 0;

	if (fcc_ma > BQ2589X_MAX_FCC)
		fcc_ma = BQ2589X_MAX_FCC;

	rc = bq2589x_set_charge_current(bq, fcc_ma);
	if (rc < 0)
		bq_err("failed to set charger current: (%d)\n", rc);

	return rc;
}

static int fv_vote_callback(struct votable *votable, void *data,
			    int fv_mv, const char *client)
{
	struct bq2589x *bq = data;
	int rc;

	bq_info("fv: %d\n", fv_mv);

	if (fv_mv < 0)
		return 0;

	rc = bq2589x_set_chargevoltage(bq, fv_mv);
	if (rc < 0)
		bq_err("failed to set charger voltage: (%d)\n", rc);

	return rc;
}

static int usb_icl_vote_callback(struct votable *votable, void *data,
				 int icl_ma, const char *client)
{
	struct bq2589x *bq = data;
	int rc;

	bq_info("icl: %d\n", icl_ma);

	if (icl_ma < 0)
		return 0;

	if (icl_ma > BQ2589X_MAX_ICL)
		icl_ma = BQ2589X_MAX_ICL;

	rc = bq2589x_set_input_current_limit(bq, icl_ma);
	if (rc < 0)
		bq_err("failed to set input current limit: (%d)\n", rc);

	return rc;
}

static int chg_dis_vote_callback(struct votable *votable, void *data,
				 int disable, const char *client)
{
	struct bq2589x *bq = data;
	int rc;

	bq_info("disable: %d\n", disable);

	rc = disable ? bq2589x_disable_charger(bq)
		  : bq2589x_enable_charger(bq);
	if (rc < 0)
		bq_err("failed to %s charger: %d\n", disable ? "disable" : "enable", rc);

	return rc;
}

/*
static int chgctrl_vote_callback(struct votable *votable, void *data,
			int disable, const char *client)
{
	struct bq2589x *bq = data;
	int rc;

	bq_info("chgctrl_vote_callback chgctrl disable: %d\n", disable);
	if (disable)
		rc = bq2589x_disable_charger(bq);
	else
		rc = bq2589x_enable_charger(bq);

	return 0;
}*/ //for ovp lead reboot

static int bq2589x_charger_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct bq2589x *bq;
	int irqn;
	int i, ret;

	bq_info("entry\n");
	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;

	bq->dev = &client->dev;
	bq->client = client;
	i2c_set_clientdata(client, bq);

	bq->regmap = devm_regmap_init_i2c(bq->client, &bq2589x_regmap_config);
	if (IS_ERR(bq->regmap)) {
		bq_err("failed to init regmap: %ld\n", PTR_ERR(bq->regmap));
		return PTR_ERR(bq->regmap);
	}

	mutex_init(&bq->usb_switch_lock);
	mutex_init(&bq->irq_complete);

	ret = bq2589x_detect_device(bq);
	if (!ret && bq->part_no == BQ25890) {
		bq->status |= BQ2589X_STATUS_EXIST;
		bq_info("charger device bq25890 detected, revision: (%d)\n", bq->revision);
	} else if (!ret && bq->part_no == SYV690) {
		bq->status |= BQ2589X_STATUS_EXIST;
		bq_info("charger device SYV690 detected, revision: (%d)\n", bq->revision);
		nopmi_set_charger_ic_type(NOPMI_CHARGER_IC_SYV);
	} else if (!ret && bq->part_no == SC89890H) {
		bq->status |= BQ2589X_STATUS_EXIST;
		bq_info("charger device SC89890H detected, revision: (%d)\n", bq->revision);
	} else {
		bq_err("no bq25890 charger device found: (%d)\n", ret);
		ret = -ENODEV;
		goto err_free;
	}

#if defined(CONFIG_TCPC_RT1711H)
	bq->tcpc_dev = tcpc_dev_get_by_name("type_c_port0");
	if (!bq->tcpc_dev) {
		bq_err("tcpc device not ready, defer\n");
		ret = -EPROBE_DEFER;
		goto err_tcpc_dev;
	}
#endif
	bq->usb_psy = power_supply_get_by_name("usb");
	bq->batt_psy = power_supply_get_by_name("battery");
	bq->bms_psy = power_supply_get_by_name("bms");

	atomic_set(&bq->irq_pending, 0);
	atomic_set(&bq->ws_count, 0);
	atomic_set(&bq->resume_completed, 1);
	atomic_set(&bq->irq_disabled, 0);
	atomic_set(&bq->shutting_down, 0);

	for (i = 0; i < BQ_RL_SLOTS; i++) {
		ratelimit_state_init(&bq->rl_slots[i], BQ_RL_INTERVAL, BQ_RL_BURST);
		bq->rl_slots[i].flags |= RATELIMIT_MSG_ON_RELEASE;
	}

	ret = bq2589x_parse_dt(bq->dev, bq);
	if (ret) {
		bq_err("DT parse failed: %d\n", ret);
		goto err_init;
	}

	ret = bq2589x_init_device(bq);
	if (ret) {
		bq_err("device init failure: (%d)\n", ret);
		goto err_init;
	}

	irqn = gpiod_to_irq(bq->irq_gpiod);
	if (irqn <= 0) {
		bq_err("gpiod_to_irq failed: %d\n", irqn);
		ret = irqn;
		goto err_gpio;
	}

	ret = bq2589x_psy_register(bq);
	if (ret) {
		bq_err("psy_register failed\n");
		goto err_psy;
	}

	bq->bq_ws = wakeup_source_register(NULL, "bq2589x_ws");
	if (IS_ERR_OR_NULL(bq->bq_ws)) {
		ret = IS_ERR(bq->bq_ws) ? PTR_ERR(bq->bq_ws) : -ENOMEM;
		bq_err("wakeup_source_register failed: %d\n", ret);
		goto err_ws;
	}

	INIT_WORK(&bq->adapter_in_work, bq2589x_adapter_in_workfunc);
	INIT_WORK(&bq->adapter_out_work, bq2589x_adapter_out_workfunc);
	INIT_WORK(&bq->start_charging_work, bq2589x_start_charging_workfunc);
	INIT_DELAYED_WORK(&bq->monitor_work, bq2589x_monitor_workfunc);
	INIT_DELAYED_WORK(&bq->ico_work, bq2589x_ico_workfunc);
	INIT_DELAYED_WORK(&bq->charger_work, bq2589x_charger_workfunc);
	INIT_DELAYED_WORK(&bq->tune_volt_work, bq2589x_tune_volt_workfunc);
	INIT_DELAYED_WORK(&bq->usb_changed_work, bq2589x_usb_changed_workfunc);
	INIT_DELAYED_WORK(&bq->check_pe_tuneup_work, bq2589x_check_pe_tuneup_workfunc);
	INIT_DELAYED_WORK(&bq->time_delay_work, bq2589x_time_delay_workfunc);

	bq->fcc_votable = create_votable("FCC", VOTE_MIN,
					 fcc_vote_callback, bq);
	if (IS_ERR(bq->fcc_votable)) {
		ret = PTR_ERR(bq->fcc_votable);
		bq->fcc_votable = NULL;
		goto destroy_votable;
	}

	bq->chg_dis_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					     chg_dis_vote_callback, bq);
	if (IS_ERR(bq->chg_dis_votable)) {
		ret = PTR_ERR(bq->chg_dis_votable);
		bq->chg_dis_votable = NULL;
		goto destroy_votable;
	}

	bq->fv_votable = create_votable("FV", VOTE_MIN,
					fv_vote_callback, bq);
	if (IS_ERR(bq->fv_votable)) {
		ret = PTR_ERR(bq->fv_votable);
		bq->fv_votable = NULL;
		goto destroy_votable;
	}

	bq->usb_icl_votable = create_votable("USB_ICL", VOTE_MIN,
					     usb_icl_vote_callback, bq);
	if (IS_ERR(bq->usb_icl_votable)) {
		ret = PTR_ERR(bq->usb_icl_votable);
		bq->usb_icl_votable = NULL;
		goto destroy_votable;
	}

	vote(bq->fcc_votable, PROFILE_CHG_VOTER, true, CHG_FCC_CURR_MAX);
	vote(bq->usb_icl_votable, PROFILE_CHG_VOTER, true, CHG_ICL_CURR_MAX);
	vote(bq->fcc_votable, MAIN_SET_VOTER, true, MAIN_FCC_MIN);
	vote(bq->fv_votable, JEITA_VOTER, true, CHG_FV_CURR_MAX);
	vote(bq->chg_dis_votable, "BMS_FC_VOTER", false, 0);

	ret = sysfs_create_group(&bq->dev->kobj, &bq2589x_attr_group);
	if (ret) {
		bq_err("failed to register sysfs: (%d)\n", ret);
		goto err_sysfs;
	}

	bq->pe.enable = false;
	ret = devm_request_threaded_irq(bq->dev, irqn,
					bq2589x_hard_irq, bq2589x_thread_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"bq2589x_charger_irq", bq);
	if (ret) {
		bq_err("request_threaded_irq failed for IRQ %d: %d\n", irqn, ret);
		goto err_irq;
	}
	bq->irq = irqn;
	/* at this point, disable irq before usb-switch registered */
	disable_irq(bq->irq);
	usleep_range(4500, 5500);

	bq->usb_switch1_gpiod = devm_gpiod_get(bq->dev, "usb-switch1", GPIOD_OUT_HIGH);
	if (IS_ERR(bq->usb_switch1_gpiod)) {
		ret = PTR_ERR(bq->usb_switch1_gpiod);
		bq_err("devm_gpiod_get(usb-switch1) failed: %d\n", ret);
		enable_irq(bq->irq);
		goto err_usb_switch;
	}

	bq_info("usb-switch1 descriptor acquired\n");
	bq->usb_switch_flag = !!gpiod_get_value_cansleep(bq->usb_switch1_gpiod);

	enable_irq(bq->irq);
	enable_irq_wake(bq->irq);

#if defined(CONFIG_TCPC_RT1711H)
	bq->pd_nb.notifier_call = pd_tcp_notifier_call;
	ret = register_tcp_dev_notifier(bq->tcpc_dev, &bq->pd_nb, TCP_NOTIFY_TYPE_ALL);
	if (ret < 0) {
		bq_err("register tcpc notifier fail: %d\n", ret);
		disable_irq_wake(bq->irq);
		goto err_tcpc_notifier;
	}
#endif

	g_bq = bq;
	determine_initial_status(bq);
	/*
	 * At this point vbus_type may already be set by IRQ or notifier
	 * (both can fire before or during probe). If not OTG,
	 * wait bc12 with 4s delay.
	 */
	if (bq->vbus_type != BQ2589X_VBUS_OTG)
		schedule_delayed_work(&bq->time_delay_work, msecs_to_jiffies(4000));

	//bq2589x_dump_regs(bq);
	bq_info("success\n");
	return 0;

#if defined(CONFIG_TCPC_RT1711H)
err_tcpc_notifier:
#endif
err_usb_switch:
err_irq:
	sysfs_remove_group(&bq->dev->kobj, &bq2589x_attr_group);
err_sysfs:
destroy_votable:
	if (!IS_ERR_OR_NULL(bq->usb_icl_votable))
		destroy_votable(bq->usb_icl_votable);
	if (!IS_ERR_OR_NULL(bq->fv_votable))
		destroy_votable(bq->fv_votable);
	if (!IS_ERR_OR_NULL(bq->chg_dis_votable))
		destroy_votable(bq->chg_dis_votable);
	if (!IS_ERR_OR_NULL(bq->fcc_votable))
		destroy_votable(bq->fcc_votable);
err_ws:
	if (bq->bq_ws) {
		wakeup_source_unregister(bq->bq_ws);
		bq->bq_ws = NULL;
	}
err_psy:
	bq2589x_psy_unregister(bq);
err_gpio:
err_init:
	if (bq->bms_psy)
		power_supply_put(bq->bms_psy);
	if (bq->batt_psy)
		power_supply_put(bq->batt_psy);
	if (bq->usb_psy)
		power_supply_put(bq->usb_psy);
#if defined(CONFIG_TCPC_RT1711H)
	if (bq->tcpc_dev)
		tcpc_dev_put(bq->tcpc_dev);
err_tcpc_dev:
#endif
err_free:
	mutex_destroy(&bq->usb_switch_lock);
	mutex_destroy(&bq->irq_complete);
	i2c_set_clientdata(client, NULL);

	bq_err("fail! (%d)\n", ret);
	return ret;
}

static int bq2589x_charger_remove(struct i2c_client *client)
{
	struct bq2589x *bq = i2c_get_clientdata(client);

	if (!bq)
		return 0;

	bq_info("entry\n");

	bq2589x_set_otg(bq, false);
	bq2589x_exit_hiz_mode(bq);
	bq2589x_adc_stop(bq);
	usleep_range(4500, 5500);

#if defined(CONFIG_TCPC_RT1711H)
	if (bq->tcpc_dev)
		unregister_tcp_dev_notifier(bq->tcpc_dev, &bq->pd_nb, TCP_NOTIFY_TYPE_ALL);
#endif

	if (bq->irq) {
		disable_irq_wake(bq->irq);
		disable_irq(bq->irq);
		synchronize_irq(bq->irq);
	}

	cancel_work_sync(&bq->adapter_in_work);
	cancel_work_sync(&bq->adapter_out_work);
	cancel_work_sync(&bq->start_charging_work);
	cancel_delayed_work_sync(&bq->monitor_work);
	cancel_delayed_work_sync(&bq->ico_work);
	cancel_delayed_work_sync(&bq->charger_work);
	cancel_delayed_work_sync(&bq->tune_volt_work);
	cancel_delayed_work_sync(&bq->usb_changed_work);
	cancel_delayed_work_sync(&bq->check_pe_tuneup_work);
	cancel_delayed_work_sync(&bq->time_delay_work);

	sysfs_remove_group(&bq->dev->kobj, &bq2589x_attr_group);

	if (bq->bq_ws) {
		wakeup_source_unregister(bq->bq_ws);
		bq->bq_ws = NULL;
	}

	if (!IS_ERR_OR_NULL(bq->usb_icl_votable))
		destroy_votable(bq->usb_icl_votable);
	if (!IS_ERR_OR_NULL(bq->fv_votable))
		destroy_votable(bq->fv_votable);
	if (!IS_ERR_OR_NULL(bq->chg_dis_votable))
		destroy_votable(bq->chg_dis_votable);
	if (!IS_ERR_OR_NULL(bq->fcc_votable))
		destroy_votable(bq->fcc_votable);

	bq2589x_psy_unregister(bq);
	if (bq->bms_psy)
		power_supply_put(bq->bms_psy);
	if (bq->batt_psy)
		power_supply_put(bq->batt_psy);
	if (bq->usb_psy)
		power_supply_put(bq->usb_psy);
#if defined(CONFIG_TCPC_RT1711H)
	if (bq->tcpc_dev)
		tcpc_dev_put(bq->tcpc_dev);
#endif

	mutex_destroy(&bq->usb_switch_lock);
	mutex_destroy(&bq->irq_complete);
	i2c_set_clientdata(client, NULL);
	g_bq = NULL;

	bq_info("success\n");
	return 0;
}

static void bq2589x_charger_shutdown(struct i2c_client *client)
{
	struct bq2589x *bq = i2c_get_clientdata(client);

	if (!bq)
		return;

	bq_info("entry\n");

	mutex_lock(&bq->irq_complete);
	atomic_set(&bq->shutting_down, 1);
	mutex_unlock(&bq->irq_complete);

	bq2589x_set_otg(bq, false);
	bq2589x_exit_hiz_mode(bq);
	bq2589x_adc_stop(bq);
	usleep_range(4500, 5500);

	if (bq->irq) {
		disable_irq_wake(bq->irq);
		disable_irq(bq->irq);
		synchronize_irq(bq->irq);
	}

	cancel_work_sync(&bq->adapter_in_work);
	cancel_work_sync(&bq->adapter_out_work);
	cancel_work_sync(&bq->start_charging_work);
	cancel_delayed_work_sync(&bq->monitor_work);
	cancel_delayed_work_sync(&bq->ico_work);
	cancel_delayed_work_sync(&bq->charger_work);
	cancel_delayed_work_sync(&bq->tune_volt_work);
	cancel_delayed_work_sync(&bq->usb_changed_work);
	cancel_delayed_work_sync(&bq->check_pe_tuneup_work);
	cancel_delayed_work_sync(&bq->time_delay_work);

	bq_info("success\n");
}

static int bq2589x_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2589x *bq = i2c_get_clientdata(client);

	if (!bq)
		return 0;

	atomic_set(&bq->resume_completed, 0);
	synchronize_irq(bq->irq);

	bq_info("Suspend successfully!\n");
	return 0;
}

static int bq2589x_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2589x *bq = i2c_get_clientdata(client);

	if (!bq)
		return 0;

	if (atomic_read(&bq->irq_disabled) || atomic_read(&bq->irq_pending)) {
		bq_err("Aborting suspend_noirq: pending IRQ activity\n");
		return -EBUSY;
	}

	return 0;
}

static int bq2589x_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2589x *bq = i2c_get_clientdata(client);

	if (!bq)
		return 0;

	/* Makesure mem operand is done before storing resume_completed */
	smp_wmb();
	atomic_set(&bq->resume_completed, 1);

	if (atomic_read(&bq->irq_disabled)) {
		enable_irq(bq->irq);
		atomic_set(&bq->irq_disabled, 0);
	}

	atomic_set(&bq->irq_pending, 0);
	determine_initial_status(bq);

	if (bq->wall_psy)
		power_supply_changed(bq->wall_psy);

	bq_info("Resume successfully!\n");
	return 0;
}

static const struct dev_pm_ops bq2589x_pm_ops = {
	.suspend	= bq2589x_suspend,
	.resume	= bq2589x_resume,
	.suspend_noirq	= bq2589x_suspend_noirq,
};

static const struct of_device_id bq2589x_charger_match_table[] = {
	{ .compatible = "ti,bq2589x-1", },
	{ },
};
MODULE_DEVICE_TABLE(of, bq2589x_charger_match_table);

static const struct i2c_device_id bq2589x_charger_id[] = {
	{ "bq2589x-1", BQ25890 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bq2589x_charger_id);

static struct i2c_driver bq2589x_charger_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "bq2589x-1",
		.of_match_table	= of_match_ptr(bq2589x_charger_match_table),
		.pm		= &bq2589x_pm_ops,
	},
	.id_table	= bq2589x_charger_id,
	.probe		= bq2589x_charger_probe,
	.remove	= bq2589x_charger_remove,
	.shutdown	= bq2589x_charger_shutdown,
};

module_i2c_driver(bq2589x_charger_driver);

MODULE_DESCRIPTION("TI BQ2589x Charger Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Texas Instruments");
