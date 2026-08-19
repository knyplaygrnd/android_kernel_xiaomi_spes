// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fuelgauge battery driver
 *
 * Copyright (C) 2018 Siliconmitus
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#define pr_fmt(fmt)	"[sm5602_fg]: %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/atomic.h>
#include <linux/param.h>
#include <linux/ratelimit.h>
#include <linux/printk.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/idr.h>
#include <linux/acpi.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/debugfs.h>
#include <linux/regmap.h>
#include <linux/random.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <asm/unaligned.h>
#include "sm5602_fuelgauge.h"

#define INVALID_REG_ADDR	0xFF

static const u8 sm5602_regs[SM_FG_REG_COUNT] = {
	0x00, /* DEVICE_ID */
	0x01, /* CNTL */
	0x02, /* INT */
	0x03, /* INT_MASK */
	0x04, /* STATUS */
	0x05, /* SOC */
	0x06, /* OCV */
	0x07, /* VOLTAGE */
	0x08, /* CURRENT */
	0x09, /* TEMPERATURE_IN */
	0x0A, /* TEMPERATURE_EX */
	0x0C, /* V_L_ALARM */
	0x0D, /* V_H_ALARM */
	0x0E, /* A_H_ALARM */
	0x0F, /* T_IN_H_ALARM */
	0x10, /* SOC_L_ALARM */
	0x11, /* FG_OP_STATUS */
	0x12, /* TOPOFFSOC */
	0x13, /* PARAM_CTRL */
	0x14, /* SHUTDOWN */
	0x1A, /* VIT_PERIOD */
	0x1B, /* CURRENT_RATE */
	0x62, /* BAT_CAP */
	0x73, /* CURR_OFFSET */
	0x74, /* CURR_SLOPE */
	0x90, /* MISC */
	0x91, /* RESET */
	0x95, /* RSNS_SEL */
	0x96, /* VOL_COMP */
};

const unsigned char *device2str[] = {
	"sm5602",
};

#ifdef ENABLE_NTC_COMPENSATION
#define SCALE_TABLE_LEN 23
#define LUT_SIM_TABLE_LEN 43
#define LUT_TABLE_LEN 249

static const s32 LUT_SIM_UV[LUT_SIM_TABLE_LEN] = {
	0, 270, 480, 510, 910,
	1180, 1330, 2120, 2220, 2400,
	2660, 3290, 4000, 4790, 5910,
	6650, 7440, 8000, 11670, 11940,
	13260, 13350, 14370, 16040, 20930,
	23760, 26370, 28720, 42490, 47060,
	64860, 75410, 87080, 114290, 126320,
	152320, 218180, 229150, 383190, 508830,
	783080, 1024200, 1374360
};

static const s16 LUT_SIM_ADC[LUT_SIM_TABLE_LEN] = {
	0x8001, 0x8D10, 0x8D1B, 0x8D1C, 0x8D30,
	0x8D3E, 0x8D45, 0x8D6B, 0x8D70, 0x8D78,
	0x8D86, 0x8DA5, 0x8DC8, 0x8DEE, 0x8E25,
	0x8E4A, 0x8E70, 0x8E8B, 0x8F40, 0x8F4D,
	0x8F8F, 0x8F93, 0x8FC4, 0x9018, 0x9106,
	0x9192, 0x9213, 0x9288, 0x9530, 0x960B,
	0x997E, 0x9B85, 0x9DD5, 0xA2FB, 0xA573,
	0xAA5F, 0xB70C, 0xB9E2, 0xD720, 0xEFF9,
	0x222B, 0x452A, 0x7FC0
};

static const s32 LUT_UV[LUT_TABLE_LEN] = {
	0, 5000, 10000, 15000, 20000,
	25000, 30000, 35000, 40000, 45000,
	50000, 55000, 60000, 65000, 70000,
	75000, 80000, 85000, 90000, 95000,
	100000, 105000, 110000, 115000, 120000,
	125000, 130000, 135000, 140000, 145000,
	150000, 155000, 160000, 165000, 170000,
	175000, 180000, 185000, 190000, 195000,
	200000, 205000, 210000, 215000, 220000,
	225000, 230000, 235000, 240000, 245000,
	250000, 255000, 260000, 265000, 270000,
	275000, 280000, 285000, 290000, 295000,
	300000, 305000, 310000, 315000, 320000,
	325000, 330000, 335000, 340000, 345000,
	350000, 355000, 360000, 365000, 370000,
	375000, 380000, 385000, 390000, 395000,
	400000, 405000, 410000, 415000, 420000,
	425000, 430000, 435000, 440000, 445000,
	450000, 455000, 460000, 465000, 470000,
	475000, 480000, 485000, 490000, 495000,
	500000, 505000, 510000, 515000, 520000,
	525000, 530000, 535000, 540000, 545000,
	550000, 555000, 560000, 565000, 570000,
	575000, 580000, 585000, 590000, 595000,
	600000, 605000, 610000, 615000, 620000,
	625000, 630000, 635000, 640000, 645000,
	650000, 655000, 660000, 665000, 670000,
	675000, 680000, 685000, 690000, 695000,
	700000, 705000, 710000, 715000, 720000,
	725000, 730000, 735000, 740000, 745000,
	750000, 755000, 760000, 765000, 770000,
	775000, 780000, 785000, 790000, 795000,
	800000, 805000, 810000, 815000, 820000,
	825000, 830000, 835000, 840000, 845000,
	850000, 855000, 860000, 865000, 870000,
	875000, 880000, 885000, 890000, 895000,
	900000, 905000, 910000, 915000, 920000,
	925000, 930000, 935000, 940000, 945000,
	950000, 955000, 960000, 965000, 970000,
	975000, 980000, 985000, 990000, 995000,
	1000000, 1005000, 1010000, 1015000, 1020000,
	1025000, 1030000, 1035000, 1040000, 1045000,
	1050000, 1055000, 1060000, 1065000, 1070000,
	1075000, 1080000, 1085000, 1090000, 1095000,
	1100000, 1105000, 1110000, 1115000, 1120000,
	1125000, 1130000, 1135000, 1140000, 1145000,
	1150000, 1155000, 1160000, 1165000, 1170000,
	1175000, 1180000, 1185000, 1190000, 1195000,
	1200000, 1205000, 1210000, 1215000, 1220000,
	1225000, 1230000, 1235000, 1240000
};

static const s16 LUT_ADC[LUT_TABLE_LEN] = {
	0x8D18, 0x8DEF, 0x8ED4, 0x8FB5, 0x909C,
	0x917F, 0x9267, 0x9348, 0x9430, 0x9516,
	0x95FC, 0x96DF, 0x97C2, 0x98AA, 0x998B,
	0x9A70, 0x9B54, 0x9C3C, 0x9D1F, 0x9E02,
	0x9EEC, 0x9FD1, 0xA0B2, 0xA198, 0xA27D,
	0xA35E, 0xA448, 0xA529, 0xA610, 0xA6F0,
	0xA7DF, 0xA8C0, 0xA9A7, 0xAA8B, 0xAB6D,
	0xAC53, 0xAD39, 0xAE20, 0xAF04, 0xAFEA,
	0xB0CB, 0xB1B3, 0xB298, 0xB37E, 0xB461,
	0xB547, 0xB62C, 0xB712, 0xB7F5, 0xB8D9,
	0xB9C1, 0xBAA5, 0xBB88, 0xBC6F, 0xBD51,
	0xBE3B, 0xBF1E, 0xC003, 0xC0E8, 0xC1CE,
	0xC2B1, 0xC397, 0xC47C, 0xC562, 0xC644,
	0xC72C, 0xC811, 0xC8F3, 0xC9D6, 0xCABE,
	0xCB9F, 0xCC88, 0xCD6B, 0xCE52, 0xCF36,
	0xD01A, 0xD101, 0xD1E6, 0xD2CB, 0xD3AE,
	0xD492, 0xD578, 0xD65A, 0xD745, 0xD826,
	0xD90D, 0xD9F2, 0xDAD9, 0xDBBE, 0xDCA1,
	0xDD85, 0xDE6B, 0xDF4E, 0xE036, 0xE118,
	0xE1FB, 0xE2E4, 0xE3C5, 0xE4AB, 0xE591,
	0xE678, 0xE75C, 0xE840, 0xE925, 0xEA0B,
	0xEAEC, 0xEBD1, 0xECBA, 0xED9E, 0xEE85,
	0xEF85, 0xF061, 0xF148, 0xF22F, 0xF30A,
	0xF3F6, 0xF4D9, 0xF5B0, 0xF6A1, 0xF78B,
	0x9F, 0x17D, 0x26A, 0x34C, 0x43B,
	0x520, 0x5F5, 0x6E0, 0x7C5, 0x8B5,
	0x997, 0xA76, 0xB5B, 0xC3D, 0xD23,
	0xE08, 0xEF4, 0xFDA, 0x10C0, 0x11A2,
	0x128F, 0x1371, 0x142A, 0x150C, 0x15F7,
	0x16D4, 0x17BD, 0x189E, 0x1950, 0x1A6A,
	0x1B4F, 0x1C31, 0x1D1B, 0x1DFB, 0x1EDB,
	0x1FBF, 0x20AE, 0x2191, 0x2273, 0x235B,
	0x2445, 0x2530, 0x261B, 0x26FA, 0x27E5,
	0x28BB, 0x29A4, 0x2A9F, 0x2B6F, 0x2C5C,
	0x2D38, 0x2E11, 0x2F2A, 0x2FE9, 0x30D7,
	0x31B9, 0x3296, 0x3381, 0x3467, 0x3553,
	0x3637, 0x3717, 0x3802, 0x38E4, 0x39CA,
	0x3AAD, 0x3B91, 0x3C79, 0x3D5B, 0x3E47,
	0x3F20, 0x400B, 0x40F0, 0x41DB, 0x42C5,
	0x43A0, 0x447F, 0x4566, 0x4653, 0x472D,
	0x4815, 0x48F0, 0x49E1, 0x4ACA, 0x4BA8,
	0x4C8D, 0x4D76, 0x4E5E, 0x4F36, 0x5021,
	0x5110, 0x51F2, 0x52D3, 0x53BB, 0x54AA,
	0x558D, 0x5671, 0x5759, 0x583B, 0x5926,
	0x5A0E, 0x5AEF, 0x5BD5, 0x5CBD, 0x5D9C,
	0x5E7E, 0x5F67, 0x604B, 0x612D, 0x6210,
	0x62FB, 0x63E2, 0x64CE, 0x65A5, 0x668E,
	0x6775, 0x6858, 0x6935, 0x6A22, 0x6B05,
	0x6BEF, 0x6CCB, 0x6DD5, 0x6E9E, 0x6F6F,
	0x7050, 0x7127, 0x7208, 0x72E7
};

static const s32 scale_thresholds_ma[SCALE_TABLE_LEN] = {
	500, 750, 1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750,
	3000, 3250, 3500, 3750, 4000, 4250, 4500, 4750, 5000, 5250,
	5500, 5750, 6000
};

static const s32 scale_x1000_table[SCALE_TABLE_LEN] = {
	750, 773, 795, 818, 841, 864, 886, 909, 932, 955,
	977, 1000, 1023, 1045, 1068, 1091, 1114, 1136, 1159, 1182,
	1205, 1227, 1250
};

#define OVERHEAT_TH_DEG	50
#define COLD_TH_DEG	0
#endif /* ENABLE_NTC_COMPENSATION */

#define sm_log(fmt, ...)							\
do {										\
	if (sm && __ratelimit(&sm->rl_slots[_SM_ID() & (SM_RL_SLOTS - 1)]))	\
		pr_info(fmt, ##__VA_ARGS__);					\
} while (0)

#define sm_err(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)
#define sm_info(fmt, ...)	pr_info(fmt, ##__VA_ARGS__)
#define sm_dbg(fmt, ...)	pr_debug(fmt, ##__VA_ARGS__)

static const struct regmap_config sm5602_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 16,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
	.max_register	= 0xE0,
	.cache_type	= REGCACHE_NONE,
};

static LIST_HEAD(sm_list);
static DEFINE_MUTEX(sm_list_lock);
static bool fg_init(struct sm_fg_chip *sm);
static bool fg_reg_init(struct sm_fg_chip *sm);
static bool fg_check_reg_init_need(struct sm_fg_chip *sm);
static int fg_read_current(struct sm_fg_chip *sm);
static void fg_monitor_workfunc(struct work_struct *work);

static int fg_read_word(struct sm_fg_chip *sm, u8 reg, u16 *val)
{
	int ret, retry;
	unsigned int rval;
	const int max_retry = 3;

	if (sm->skip_reads) {
		*val = 0;
		return 0;
	}

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_read(sm->regmap, reg, &rval);
		if (ret == 0) {
			*val = (u16)rval;
			return 0;
		}

		sm_err("read 0x%02x failed (try %d/%d): %d\n",
		       reg, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	*val = 0;
	return ret;
}

static int fg_write_word(struct sm_fg_chip *sm, u8 reg, u16 val)
{
	int ret, retry;
	const int max_retry = 3;

	if (sm->skip_writes)
		return 0;

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_write(sm->regmap, reg, val);
		if (ret == 0)
			return 0;

		sm_err("write 0x%04x->0x%02x failed (try %d/%d): %d\n",
		       val, reg, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	return ret;
}

static int fg_update_bits(struct sm_fg_chip *sm, u8 reg, u16 mask, u16 val)
{
	int ret, retry;
	const int max_retry = 3;

	if (sm->skip_reads || sm->skip_writes)
		return 0;

	for (retry = 1; retry <= max_retry; retry++) {
		ret = regmap_update_bits(sm->regmap, reg, mask, val);
		if (ret == 0)
			return 0;

		sm_err("update_bits 0x%02x (mask=0x%04x val=0x%04x) failed (try %d/%d): %d\n",
		       reg, mask, val, retry, max_retry, ret);

		if (retry < max_retry)
			usleep_range(200, 300);
	}

	return ret;
}

#define FG_STATUS_SLEEP				BIT(10)
#define FG_STATUS_BATT_PRESENT		BIT(9)
#define FG_STATUS_SOC_UPDATE		BIT(8)
#define FG_STATUS_TOPOFF			BIT(7)
#define FG_STATUS_LOW_SOC2			BIT(6)
#define FG_STATUS_LOW_SOC1			BIT(5)
#define FG_STATUS_HIGH_CURRENT		BIT(4)
#define FG_STATUS_HIGH_TEMPERATURE	BIT(3)
#define FG_STATUS_LOW_TEMPERATURE	BIT(2)
#define FG_STATUS_HIGH_VOLTAGE		BIT(1)
#define FG_STATUS_LOW_VOLTAGE		BIT(0)
/* if can use the charger information,
 * please use the charger information for CHG/DISCHG condition.
 */
#define FG_OP_STATUS_CHG_DISCHG		BIT(15)

static int fg_read_status(struct sm_fg_chip *sm)
{
	int ret;
	u16 flags1, flags2;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_STATUS], &flags1);
	if (ret < 0)
		return ret;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_FG_OP_STATUS], &flags2);
	if (ret < 0)
		return ret;

	mutex_lock(&sm->data_lock);
	sm->batt_present	= !!(flags1 & FG_STATUS_BATT_PRESENT);
	sm->batt_ot		= !!(flags1 & FG_STATUS_HIGH_TEMPERATURE);
	sm->batt_ut		= !!(flags1 & FG_STATUS_LOW_TEMPERATURE);
	sm->batt_fc		= !!(flags1 & FG_STATUS_TOPOFF);
	sm->batt_soc1	= !!(flags1 & FG_STATUS_LOW_SOC2);
	sm->batt_socp	= !!(flags1 & FG_STATUS_LOW_SOC1);
	sm->batt_dsg		= !!!(flags2 & FG_OP_STATUS_CHG_DISCHG);
	mutex_unlock(&sm->data_lock);

	return 0;
}

static inline bool do_update(struct sm_fg_chip *sm,
			     enum fg_update_idx idx,
			     unsigned long interval_ms)
{
	ktime_t now = ktime_get_boottime();
	s64 old = atomic64_read(&sm->last_update_time[idx]);

	if (ktime_ms_delta(now, (ktime_t)old) < interval_ms)
		return false;

	return atomic64_cmpxchg(&sm->last_update_time[idx], old, now) == old;
}

#define STEP_TENTHS 10
static void fg_read_scale_soc(struct sm_fg_chip *sm)
{
	int raw_soc = sm->param.batt_raw_soc;
	int orig_soc = (raw_soc <= 0) ? 0 : (raw_soc * 100 + 960) / 97;
	int target_soc, delta_soc, bucket_soc;
	int curr_soc, delta_tenths, new_soc;

	if (raw_soc >= 970) {
		target_soc = 1000;
	} else if (orig_soc <= 990) {
		target_soc = orig_soc;
	} else {
		delta_soc = orig_soc - 990;
		bucket_soc = delta_soc >> 1;
		target_soc = 990 + bucket_soc;
	}

	if (!sm->soc_smooth_initialized) {
		sm->param.batt_soc = target_soc;
		sm->soc_smooth_initialized = true;
		return;
	}

	curr_soc = sm->param.batt_soc;
	delta_tenths = target_soc - curr_soc;
	if (delta_tenths == 0)
		return;

	if (delta_tenths > 0)
		new_soc = curr_soc + (delta_tenths > STEP_TENTHS ? STEP_TENTHS : delta_tenths);
	else
		new_soc = curr_soc - ((-delta_tenths) > STEP_TENTHS ? STEP_TENTHS : -delta_tenths);

	sm_info("soc: %d.%d, raw: %d.%d, delta_tenths: %d\n",
		new_soc / 10, new_soc % 10, raw_soc / 10, raw_soc % 10, delta_tenths);

	sm->param.batt_soc = new_soc;
}

static int fg_read_soc(struct sm_fg_chip *sm)
{
	int ret, soc;
	s32 raw_soc;
	u32 raw;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_SOC, 1000))
		return 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_SOC], &data);
	if (ret < 0) {
		sm_err("Couldn't read SOC, ret=%d\n", ret);
		return ret;
	}

	raw = data & 0x7FFFU;
	raw_soc = (raw * 10U) >> 8;

	if (data & 0x8000U) {
		sm_err("data: 0x%04x, raw_soc: %d\n", (unsigned int)data, raw_soc);
		raw_soc = -raw_soc;
	}

	soc = clamp(raw_soc, 0, 1000);

	mutex_lock(&sm->data_lock);
	sm->batt_soc = soc;
	sm->param.batt_raw_soc = soc;
	fg_read_scale_soc(sm);
	mutex_unlock(&sm->data_lock);

	return 0;
}

static int fg_get_soc_decimal(struct sm_fg_chip *sm)
{
	return sm->batt_soc % 100;
}

static int fg_get_soc_decimal_rate(struct sm_fg_chip *sm)
{
	int soc, i;

	if (!sm->dec_rate_seq || sm->dec_rate_len < 2)
		return 0;

	soc = sm->batt_soc / 10;
	if (soc < (int)sm->dec_rate_seq[0])
		return (int)sm->dec_rate_seq[1];

	for (i = 2; i < sm->dec_rate_len; i += 2) {
		if (soc < (int)sm->dec_rate_seq[i])
			return (int)sm->dec_rate_seq[i - 1];
	}

	return (int)sm->dec_rate_seq[sm->dec_rate_len - 1];
}

static unsigned int fg_read_ocv(struct sm_fg_chip *sm)
{
	int ret;
	u32 low, high;
	u16 data = 0;
	u32 ocv; /* mV */

	if (!do_update(sm, FG_UPDATE_OCV, 1000))
		return 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_OCV], &data);
	if (ret < 0) {
		sm_err("Couldn't read OCV, ret=%d\n", ret);
		return ret;
	}

	low = data & 0x0FFFU;
	high = data >> 12;
	ocv = ((low * 1000u) >> 11) + (high * 2000u);

	mutex_lock(&sm->data_lock);
	sm->batt_ocv = ocv;
	mutex_unlock(&sm->data_lock);

	return ocv;
}

#ifdef ENABLE_NTC_COMPENSATION
static int adc_to_uv(int len, s16 adc_code, const s16 *adc_table, const s32 *uv_table)
{
	int i;
	s64 num, den, tmp, res;
	s32 out_uv;

	if (adc_code < adc_table[0])
		return uv_table[0];
	if (adc_code > adc_table[len - 1])
		return uv_table[len - 1];

	for (i = 0; i < len - 1; i++) {
		if (adc_code >= adc_table[i] && adc_code <= adc_table[i + 1])
			break;
	}

	if (i >= len - 1)
		return uv_table[len - 1];
	if (adc_table[i + 1] == adc_table[i])
		return uv_table[i];

	num = (s64)(uv_table[i + 1] - uv_table[i]) * (s64)(adc_code - adc_table[i]);
	den = (s64)adc_table[i + 1] - adc_table[i];
	tmp = div_s64(num, den);
	res = (s64)uv_table[i] + tmp;

	out_uv = (s32)res;
	return out_uv;
}

static s16 uv_to_adc(int len, s32 meas_uv, const s32 *uv_table, const s16 *adc_table)
{
	int i;
	s64 num, den, tmp, res;
	s16 out_adc;

	if (meas_uv < uv_table[0])
		return adc_table[0];
	if (meas_uv > uv_table[len - 1])
		return adc_table[len - 1];

	for (i = 0; i < len - 1; i++) {
		if (meas_uv >= uv_table[i] && meas_uv <= uv_table[i + 1])
			break;
	}

	if (i >= len - 1)
		return adc_table[len - 1];
	if (uv_table[i + 1] == uv_table[i])
		return adc_table[i];

	num = (s64)(adc_table[i + 1] - adc_table[i]) * (s64)(meas_uv - uv_table[i]);
	den = (s64)uv_table[i + 1] - uv_table[i];
	tmp = div_s64(num, den);
	res = (s64)adc_table[i] + tmp;

	out_adc = (s16)res;
	return out_adc;
}

static s64 ir_drop_uv(int curr_ma, int rtrace_uohm)
{
	s64 corr = div_s64((s64)curr_ma * (s64)rtrace_uohm, 1000LL);
	return corr;
}

static int get_scale_x1000(int curr_mA)
{
	int i;

	for (i = SCALE_TABLE_LEN - 1; i >= 0; --i) {
		if (curr_mA >= scale_thresholds_ma[i])
			return scale_x1000_table[i];
	}
	return 0;
}

static s16 compensate_ntc(struct sm_fg_chip *sm, s16 raw_adc)
{
	int len = sizeof(LUT_UV) / sizeof(s32);
	int curr = sm->batt_curr;
	int rtrace = sm->rtrace;
	s16 corrected_adc;
	s32 meas_uv, corrected_uv, scale_x1000;
	s64 corr, ir_uv, corr_candidate, corr_prev, delta;
	const s64 max_step_uv = 10000LL;

	corrected_adc = raw_adc;
	meas_uv = adc_to_uv(len, raw_adc, LUT_ADC, LUT_UV);
	sm_dbg("DBG_COMP: raw_adc=0x%04x, raw_adc_dec=%d, meas_uV=%d, curr_ma=%d, rtrace_uohm=%d\n",
	       (u32)(u16)raw_adc, (s32)raw_adc, meas_uv, curr, rtrace);

	if (curr < 500) {
		sm_dbg("DBG_COMP: curr %d < 500 mA, skip compensation\n", curr);
		corrected_uv = meas_uv;
	} else {
		ir_uv = ir_drop_uv(curr, rtrace);
		scale_x1000 = get_scale_x1000(curr);
		corr_candidate = (ir_uv * (s64)scale_x1000) / 1000LL;

		corr_prev = sm->prev_corr_uv;
		delta = corr_candidate - corr_prev;
		if (delta > max_step_uv)
			corr_candidate = corr_prev + max_step_uv;
		else if (delta < -max_step_uv)
			corr_candidate = corr_prev - max_step_uv;

		sm->prev_corr_uv = corr_candidate;
		corr = corr_candidate;
		corrected_uv = (s32)((s64)meas_uv - corr);

		sm_dbg("DBG_COMP: ir_uv=%lld, scale_x1000=%d, corr_uv=%lld, temp_uV_after_corr=%d, corr_mV_approx=%lld\n",
		       (s64)ir_uv, (s32)scale_x1000, (s64)corr,
		       (s32)corrected_uv, (s64)(corr / 1000LL));
	}

	corrected_adc = uv_to_adc(len, corrected_uv, LUT_UV, LUT_ADC);
	sm_dbg("DBG_COMP: code_adc_after=0x%04x, val_for_map=0x%04x\n",
	       (u32)(u16)corrected_adc, (u32)(u16)corrected_adc);

	return corrected_adc;
}
#endif

static int __calculate_battery_temp_ex(struct sm_fg_chip *sm, u16 uval)
{
	int i, temp;
	s16 val = 0;
#ifdef ENABLE_NTC_COMPENSATION
	s16 new_adc = 0;
	int curr = sm->batt_curr;
#endif

	if (uval >= 0x8001U && uval <= 0x823BU) {
		sm_err("sp_range uval = 0x%x\n", uval);
		uval = 0x0000U;
	}

	val = (s16)uval;

#ifdef ENABLE_NTC_COMPENSATION
	sm_info("DBG_COMP: NTC_COMP=ON, batt_curr=%d, rtrace_cfg=%d\n", sm->batt_curr, sm->rtrace);
	if (curr >= 500) {
		new_adc = compensate_ntc(sm, val);
		val = new_adc;
	}
#else
	sm_info("DBG_COMP: NTC_COMP=OFF, batt_curr=%d\n", sm->batt_curr);
#endif

	if (val >= sm->battery_temp_table[0]) {
		temp = EX_TEMP_MIN;
	} else if (val <= sm->battery_temp_table[FG_TEMP_TABLE_CNT_MAX - 1]) {
		temp = EX_TEMP_MAX;
	} else {
		temp = EX_TEMP_MAX; /* malformed table guard */
		for (i = 0; i < FG_TEMP_TABLE_CNT_MAX; i++) {
			if (val >= sm->battery_temp_table[i]) {
				temp = EX_TEMP_MIN + i;
				if (temp >= 1 && val != sm->battery_temp_table[i])
					temp = temp - 1;
				break;
			}
		}
	}

#ifdef ENABLE_NTC_COMPENSATION
	sm_info("DBG_COMP: raw_uval=0x%04x, final_val=0x%04x, temp=%d\n",
		(u32)uval, (u32)(u16)val, temp);
#else
	sm_info("DBG_COMP: (NTC OFF) raw_uval=0x%04x, final_val=0x%04x, temp=%d\n",
		(u32)uval, (u32)(u16)val, temp);
#endif

	return temp;
}

#ifdef ENABLE_TEMP_AVG
#define MIN_TEMP_SAMPLE_WINDOW	3
#define MAX_TEMP_SAMPLE_WINDOW	BATT_TEMP_AVG_SAMPLES
static void calculate_average_temperature(struct sm_fg_chip *sm)
{
	int curr = sm->param.batt_temp;
	int prev = sm->param.batt_temp_prev;
	int delta, abs_delta;
	int sum = 0, count = 0;
	int i, idx;

	if (curr == -EINVAL)
		return;

	if (sm->param.batt_temp_avg == -EINVAL) {
		sm->param.batt_temp_avg = curr;
		sm->param.batt_temp_prev = curr;
		sm->param.batt_temp_samples_index = 0;
		sm->param.batt_temp_samples_num = 1;
		sm->param.batt_temp_avg_samples[0] = curr;
		return;
	}

	delta = curr - prev;
	abs_delta = abs(delta);
	sm->param.batt_temp_prev = curr;

	sm->param.batt_temp_avg_samples[sm->param.batt_temp_samples_index] = curr;
	sm->param.batt_temp_samples_index =
		(sm->param.batt_temp_samples_index + 1) % BATT_TEMP_AVG_SAMPLES;

	if (sm->param.batt_temp_samples_num < BATT_TEMP_AVG_SAMPLES)
		sm->param.batt_temp_samples_num++;

	if (sm->param.batt_temp_samples_num < MIN_TEMP_SAMPLE_WINDOW) {
		sm->param.batt_temp_avg = curr;
		return;
	}

	if (abs_delta <= 1)
		count = MIN_TEMP_SAMPLE_WINDOW;
	else
		count = MAX_TEMP_SAMPLE_WINDOW;

	if (count > sm->param.batt_temp_samples_num)
		count = sm->param.batt_temp_samples_num;

	for (i = 0; i < count; i++) {
		idx =
			(sm->param.batt_temp_samples_index - 1 - i + BATT_TEMP_AVG_SAMPLES) %
			BATT_TEMP_AVG_SAMPLES;
		sum += sm->param.batt_temp_avg_samples[idx];
	}

	sm->param.batt_temp_avg = DIV_ROUND_CLOSEST(sum, count);

	sm_log("raw_temp: %d, avg_temp: %d, delta: %d, abs: %d, count: %d\n",
	       curr, sm->param.batt_temp_avg, delta, abs_delta, count);
}
#endif

static int fg_read_temperature(struct sm_fg_chip *sm,
			       enum sm_fg_temperature_type temperature_type)
{
	int ret;
	int temp;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_TEMP, 1000))
		return 0;

	switch (temperature_type) {
	case TEMPERATURE_IN:
		ret = fg_read_word(sm, sm->regs[SM_FG_REG_TEMPERATURE_IN], &data);
		if (ret < 0) {
			sm_err("Couldn't read TEMP_IN, ret=%d\n", ret);
			return ret;
		}

		temp = data & 0x00FFU;
		if (data & 0x8000U)
			temp = -temp;

		sm_log("temp_in: %d\n", temp);
		break;
	case TEMPERATURE_EX:
		ret = fg_read_word(sm, sm->regs[SM_FG_REG_TEMPERATURE_EX], &data);
		if (ret < 0) {
			sm_err("Couldn't read TEMP_EX, ret=%d\n", ret);
			return ret;
		}

		mutex_lock(&sm->data_lock);
		temp = __calculate_battery_temp_ex(sm, data);
		mutex_unlock(&sm->data_lock);
		/* W/A >= 60°C */
		if (temp >= 60) {
			sm_err("temp >= 60 exceeded, schedule overtemp delay\n");
			if (!sm->overtemp_allow_restart) {
				temp = 60;
				if (!sm->overtemp_delay_on) {
					sm->overtemp_delay_on = true;
					/* keep 60°C for 20s */
					queue_delayed_work(sm->main_wq,
							   &sm->overtemp_delay_work,
							   msecs_to_jiffies(20000));
				}
			}
		} else if (temp < 60 && sm->overtemp_delay_on) {
			sm_err("temp is < 60, cleanup overtemp delay work\n");
			cancel_delayed_work_sync(&sm->overtemp_delay_work);
			sm->overtemp_delay_on = false;
			sm->overtemp_allow_restart = false;
		}

		sm_log("temp_ex: %d\n", temp);
		break;
	default:
		return -ENODATA;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_temp = temp;
#ifdef ENABLE_TEMP_AVG
	sm->param.batt_temp = temp;
	calculate_average_temperature(sm);
#endif
	mutex_unlock(&sm->data_lock);

	return 0;
}

static int fg_read_volt(struct sm_fg_chip *sm)
{
	int ret, volt;
	s32 scaled;
	u32 raw;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_VOLT, 1000))
		return 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_VOLTAGE], &data);
	if (ret < 0) {
		sm_err("Couldn't read VOLT, ret=%d\n", ret);
		return ret;
	}

	raw = data & 0x7FFFU;
	scaled = (raw * 94U) >> 10;

	if (data & 0x8000U)
		scaled = -scaled;

	volt = scaled + 2700;

	mutex_lock(&sm->data_lock);
	sm->batt_volt = volt;
	mutex_unlock(&sm->data_lock);

	return 0;
}

static int fg_get_cycle(struct sm_fg_chip *sm)
{
	int ret, cycle;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_CYCLE, 1000))
		return 0;

	ret = fg_read_word(sm, FG_REG_SOC_CYCLE, &data);
	if (ret < 0) {
		sm_err("Couldn't read CYCLE, ret=%d\n", ret);
		return ret;
	}

	cycle = data & 0x01FFU;

	mutex_lock(&sm->data_lock);
	sm->batt_soc_cycle = cycle;
	mutex_unlock(&sm->data_lock);

	return 0;
}

/*static int fg_read_current(struct sm_fg_chip *sm)
{
	int ret, rsns = 0;
	u16 data = 0;
	//float curr = 0.0;
	s64 temp = 0;
	int curr = 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_CURRENT], &data);
	if (ret < 0) {
		sm_err("Couldn't read current, ret=%d\n", ret);
		return ret;
	} else {
		if (sm->batt_rsns == -EINVAL) {
			sm_err("Couldn't read batt_rsns, force 10mohm\n");
			rsns = 10;
		} else {
			sm->batt_rsns == 0 ? rsns = 5 : (rsns = sm->batt_rsns * 10);
		}

		//curr = (((float)(data & 0x7FFF) * 1000 / 4088) / ((float)rsns / 10));
		temp = div_s64((data & 0x7FFF) * 1000, 4088) * (10 / rsns);
		curr = temp;
		if (data & 0x8000)
			curr *= -1;
	}

	//sm_info("curr: %d, data: %d\n", (int)curr, data);
	//return (int)curr;
	//sm_info("curr: %d, data: %d\n", curr, data);
	return curr;
}*/

#ifdef ENABLE_CURRENT_AVG
#define MIN_CURRENT_SAMPLE_WINDOW	3
#define MAX_CURRENT_SAMPLE_WINDOW	BATT_MA_AVG_SAMPLES
#define CURRENT_THRESHOLD_HIGH	3500
static void calculate_average_current(struct sm_fg_chip *sm)
{
	int curr = sm->param.batt_ma;
	int prev = sm->param.batt_ma_prev;
	int delta, abs_delta;
	int sum = 0;
	int count = 0;
	int i, idx;

	if (sm->param.batt_ma_avg == -EINVAL) {
		sm->param.batt_ma_avg = curr;
		sm->param.batt_ma_prev = curr;
		sm->param.batt_ma_samples_index = 0;
		sm->param.batt_ma_samples_num = 1;
		sm->param.batt_ma_avg_samples[0] = curr;
		return;
	}

	delta = curr - prev;
	abs_delta = abs(delta);
	sm->param.batt_ma_prev = curr;

	sm->param.batt_ma_avg_samples[sm->param.batt_ma_samples_index] = curr;
	sm->param.batt_ma_samples_index =
		(sm->param.batt_ma_samples_index + 1) % BATT_MA_AVG_SAMPLES;

	if (sm->param.batt_ma_samples_num < BATT_MA_AVG_SAMPLES)
		sm->param.batt_ma_samples_num++;

	if (sm->param.batt_ma_samples_num < MIN_CURRENT_SAMPLE_WINDOW) {
		sm->param.batt_ma_avg = curr;
		return;
	}

	if (curr <= CURRENT_THRESHOLD_HIGH)
		count = MIN_CURRENT_SAMPLE_WINDOW;
	else
		count = MAX_CURRENT_SAMPLE_WINDOW;

	if (count > sm->param.batt_ma_samples_num)
		count = sm->param.batt_ma_samples_num;

	if (count <= 0)
		return;

	for (i = 0; i < count; i++) {
		idx =
			(sm->param.batt_ma_samples_index - 1 - i + BATT_MA_AVG_SAMPLES) %
			BATT_MA_AVG_SAMPLES;
		sum += sm->param.batt_ma_avg_samples[idx];
	}

	if (sum >= 0)
		sm->param.batt_ma_avg = (sum + count / 2) / count;
	else
		sm->param.batt_ma_avg = (sum - count / 2) / count;

	sm_log("raw_ma: %d, avg_ma: %d, delta: %d, abs: %d, count: %d\n",
	       curr, sm->param.batt_ma_avg, delta, abs_delta, count);
}
#endif

static int fg_read_current(struct sm_fg_chip *sm)
{
	int ret, curr;
	unsigned int rsns;
	u16 data = 0;
	u32 raw;

	if (!do_update(sm, FG_UPDATE_CURRENT, 1000))
		return 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_CURRENT], &data);
	if (ret < 0) {
		sm_err("Couldn't read CURRENT, ret=%d\n", ret);
		return ret;
	}

	if (sm->batt_rsns == -EINVAL) {
		sm_err("Couldn't read batt_rsns, force 10 mohm\n");
		rsns = 10;
	} else {
		rsns = (sm->batt_rsns == 0) ? 5 : (unsigned int)(sm->batt_rsns * 10);
	}

	if (rsns == 0) {
		sm_err("invalid shunt (%u mohm)\n", rsns);
		return -EINVAL;
	}

	raw = data & 0x7FFFU;
	curr = (int)((((raw * 10000u) + 2048u) >> 12) / rsns);

	if (data & 0x8000U)
		curr = -curr;

	mutex_lock(&sm->data_lock);
	sm->batt_curr = curr;
#ifdef ENABLE_CURRENT_AVG
	sm->param.batt_ma = curr;
	calculate_average_current(sm);
#endif
	mutex_unlock(&sm->data_lock);

	return 0;
}

static int fg_read_fcc(struct sm_fg_chip *sm)
{
	int ret, fcc;
	u32 raw;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_FCC, 1000))
		return 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_BAT_CAP], &data);
	if (ret < 0) {
		sm_err("Couldn't read FCC, ret=%d\n", ret);
		return ret;
	}

	raw = data & 0x7FFFU;
	fcc = (raw * 125u) >> 8;

	mutex_lock(&sm->data_lock);
	sm->batt_fcc = fcc;
	mutex_unlock(&sm->data_lock);

	return 0;
}

static int fg_read_rmc(struct sm_fg_chip *sm)
{
	int ret, rmc;
	u32 raw;
	u16 data = 0;

	if (!do_update(sm, FG_UPDATE_RMC, 1000))
		return 0;

	ret = fg_read_word(sm, FG_REG_RMC, &data);
	if (ret < 0) {
		sm_err("Couldn't read RMC, ret=%d\n", ret);
		return ret;
	}

	raw = data & 0x7FFFU;
	rmc = (raw * 125u) >> 8;

	mutex_lock(&sm->data_lock);
	sm->batt_rmc = rmc;
	mutex_unlock(&sm->data_lock);

	return 0;
}

#define FG_SOFT_RESET	0xA6
static int fg_reset(struct sm_fg_chip *sm)
{
	int ret;

	ret = fg_write_word(sm, sm->regs[SM_FG_REG_RESET], FG_SOFT_RESET);
	if (ret < 0) {
		sm_err("Couldn't reset, ret=%d\n", ret);
		return ret;
	}
	msleep(600);

	return 0;
}

static int get_battery_status(struct sm_fg_chip *sm)
{
	union power_supply_propval ret = {0, };
	int rc;

	if (!sm->batt_psy)
		sm->batt_psy = power_supply_get_by_name("battery");
	if (sm->batt_psy) {
		/* if battery has been registered, use the status property */
		rc = power_supply_get_property(sm->batt_psy,
					       POWER_SUPPLY_PROP_STATUS, &ret);
		if (rc) {
			sm_err("battery does not export status (%d)\n", rc);
			return POWER_SUPPLY_STATUS_UNKNOWN;
		}
		return ret.intval;
	}

	/* Default to false if the battery power supply is not registered. */
	sm_err("battery power supply is not registered\n");
	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static bool is_battery_charging(struct sm_fg_chip *sm)
{
	return get_battery_status(sm) == POWER_SUPPLY_STATUS_CHARGING;
}

#ifdef ENABLE_TEMBASE_ZDSCON
static void fg_tembase_zdscon(struct sm_fg_chip *sm)
{
	u16 data = 0;
	int hminman_value, ret;
	int fg_temp_gap = sm->batt_temp - sm->temp_std;
	int abs_gap;
	int tmp;

	if (fg_temp_gap >= 0)
		return;

	abs_gap = -fg_temp_gap;

	ret = fg_read_word(sm, FG_REG_RS_3, &data);
	if (ret < 0) {
		sm_err("Couldn't read rs_3 reg, ret=%d\n", ret);
		return;
	}

	if (abs_gap <= ZDSCON_ACT_TEMP_GAP || sm->is_charging) {
		/* Restore rs_3 and rs_0 to DT values */
		if (data != sm->rs_value[3]) {
			ret = fg_write_word(sm, FG_REG_RS_3, sm->rs_value[3]);
			if (ret < 0) {
				sm_err("Couldn't write rs_3 reg, ret=%d\n", ret);
				return;
			}
		}

		ret = fg_read_word(sm, FG_REG_RS_0, &data);
		if (ret < 0 || data != sm->rs_value[0]) {
			ret = fg_write_word(sm, FG_REG_RS_0, sm->rs_value[0]);
			if (ret < 0)
				sm_err("Couldn't write rs_0 reg, ret=%d\n", ret);
		}

		sm_info("hminman restore to 0x%x, temp(%d)\n",
			sm->rs_value[3], sm->batt_temp);
		return;
	}

	/* Compute hminman adjustment */
	tmp = (int)sm->rs_value[3] +
		((int)(abs_gap - ZDSCON_ACT_TEMP_GAP) * HMINMAN_T_VALUE_FACT) / T_GAP_DENOM;
	tmp -= ((int)abs(sm->batt_curr) * HMINMAN_I_VALUE_FACT) / I_GAP_DENOM;

	tmp = clamp(tmp, (int)sm->rs_value[3], 0xFFFF);
	hminman_value = tmp;

	if (data == (u16)hminman_value)
		return;

	ret = fg_write_word(sm, FG_REG_RS_3, (u16)hminman_value);
	if (ret < 0) {
		sm_err("Couldn't write rs_3 reg, ret=%d\n", ret);
		return;
	}

	ret = fg_read_word(sm, FG_REG_RS_0, &data);
	if (ret < 0 || data != (u16)(hminman_value + 2)) {
		ret = fg_write_word(sm, FG_REG_RS_0, (u16)(hminman_value + 2));
		if (ret < 0)
			sm_err("Couldn't write rs_0 reg, ret=%d\n", ret);
	}

	sm_info("hminman set 0x%x -> 0x%x, temp(%d) curr(%d)\n",
		data, hminman_value, sm->batt_temp, sm->batt_curr);
}
#endif

static void fg_vbatocv_check(struct sm_fg_chip *sm)
{
	int topoff = sm->topoff;
	int topoff_margin = sm->topoff_margin;
	int high_bound, low_bound;
	int rs0 = 0, rs2 = 0;
	u16 data0 = 0, data2 = 0;
	u16 rs2_target;

	if (sm->fast_mode) {
		high_bound = topoff * 3 + topoff_margin;
		low_bound = topoff - topoff_margin;
	} else {
		high_bound = topoff + topoff_margin;
		low_bound = topoff - (topoff_margin * 2);
	}

	sm_info("fast_mode: %d, high_bound: %d, low_bound: %d\n",
		sm->fast_mode, high_bound, low_bound);

	rs0 = fg_read_word(sm, FG_REG_RS_0, &data0);
	if (rs0 < 0)
		sm_err("Couldn't read rs_0 reg (%d)\n", rs0);

	rs2 = fg_read_word(sm, FG_REG_RS_2, &data2);
	if (rs2 < 0)
		sm_err("Couldn't read rs_2 reg (%d)\n", rs2);

#ifdef ENABLE_VLCM_MODE
	if ((abs(sm->batt_curr) < 50) ||
	    (sm->is_charging &&
	    sm->batt_curr < high_bound &&
	    sm->batt_curr > low_bound &&
	    sm->batt_soc >= SM_RAW_SOC_FULL)) {
#else
	if (sm->is_charging &&
	    sm->batt_curr < high_bound &&
	    sm->batt_curr > low_bound &&
	    sm->batt_soc >= SM_RAW_SOC_FULL) {
#endif
		if (abs(sm->batt_ocv - sm->batt_volt) > 30)
			sm->iocv_error_count = min(sm->iocv_error_count + 1, 6);

		sm_info("iocv_error_count: %d\n", sm->iocv_error_count);
	} else {
		sm->iocv_error_count = 0;
	}

	if (sm->iocv_error_count > 5) {
		sm_info("p_v - v = (%d)\n", sm->p_batt_voltage - sm->batt_volt);
		if (abs(sm->p_batt_voltage - sm->batt_volt) > 15) {
			sm->iocv_error_count = 0;
			goto update_prev;
		}
		rs2_target = data0;
		sm_info("mode change to RS m mode 0x%x\n", rs2_target);
	} else if (sm->p_batt_voltage < sm->n_tempoff &&
		    sm->batt_volt < sm->n_tempoff &&
		    !sm->is_charging) {
		if (sm->p_batt_voltage < (sm->n_tempoff - sm->n_tempoff_offset) &&
		    sm->batt_volt < (sm->n_tempoff - sm->n_tempoff_offset)) {
			rs2_target = (u16)(data0 >> 1);
			sm_info("mode change to normal temp RS m mode >> 1 0x%x\n", rs2_target);
		} else {
			rs2_target = data0;
			sm_info("mode change to normal temp RS m mode 0x%x\n", rs2_target);
		}
	} else {
		rs2_target = sm->rs_value[2];
	}

	/* Write rs2_target only if the current value differs (or read failed) */
	if (rs2 < 0 || data2 != rs2_target) {
		sm_info("mode change to RS a mode\n");
		fg_write_word(sm, FG_REG_RS_2, rs2_target);
	}

update_prev:
	sm->p_batt_voltage = sm->batt_volt;
	sm->p_batt_current = sm->batt_curr;
}

static int fg_cal_carc(struct sm_fg_chip *sm)
{
	int ret = 0;
	int curr_cal = 0;
	int p_curr_cal = 0;
	int n_curr_cal = 0;
	int p_delta_cal = 0;
	int n_delta_cal = 0;
	int p_fg_delta_cal = 0;
	int n_fg_delta_cal = 0;
	int temp_curr_offset = 0;
	int temp_gap = 0;
	int fg_temp_gap = 0;
	int base = 0;
	int gap = 0;
	u16 tmp = 0;
#ifdef ENABLE_MIX_COMP
	u16 temp_aging_ctrl = 0;
#endif
	union power_supply_propval pval = {0, };

	sm->is_charging = is_battery_charging(sm);
	if (!sm->cp_psy)
		sm->cp_psy = power_supply_get_by_name("sc8551-standalone");
	if (sm->cp_psy) {
		ret = power_supply_get_property(sm->cp_psy,
						POWER_SUPPLY_PROP_CHARGING_ENABLED, &pval);
		if (ret < 0)
			sm_err("get sc8551_psy charge property error: %d\n", ret);
		else
			sm->cp_work_flag = !!pval.intval;
	} else {
		sm_err("sc8551_psy not found\n");
	}
#ifdef ENABLE_TEMBASE_ZDSCON
	fg_tembase_zdscon(sm);
#endif
	fg_vbatocv_check(sm);

	fg_temp_gap = sm->batt_temp - sm->temp_std;
	temp_curr_offset = sm->curr_offset & 0x00FF;

	base = (temp_curr_offset & 0x0080) ? -(temp_curr_offset & 0x007F) : temp_curr_offset;
	if (sm->en_high_fg_temp_offset && fg_temp_gap > 0)
		base += (fg_temp_gap / sm->high_fg_temp_offset_denom) *
			sm->high_fg_temp_offset_fact;
	else if (sm->en_low_fg_temp_offset && fg_temp_gap < 0)
		base += ((-fg_temp_gap) / sm->low_fg_temp_offset_denom) *
			sm->low_fg_temp_offset_fact;

	if (base < 0)
		base = ((-base) & 0x7F) | 0x0080;
	else
		base &= 0x00FF;

	tmp = (u16)((base & 0x00FF) | ((base & 0x00FF) << 8));
	ret = fg_write_word(sm, FG_REG_CURR_IN_OFFSET, tmp);
	if (ret < 0) {
		sm_err("Failed to write CURR_IN_OFFSET, ret=%d\n", ret);
		return ret;
	}
	sm_dbg("CURR_IN_OFFSET [0x%x] = 0x%x\n", FG_REG_CURR_IN_OFFSET, tmp);

	n_curr_cal = (sm->curr_slope & 0xFF00) >> 8;
	p_curr_cal = sm->curr_slope & 0x00FF;

	if (sm->en_high_fg_temp_cal && fg_temp_gap > 0) {
		gap = fg_temp_gap;

		p_fg_delta_cal = (gap / sm->high_fg_temp_p_cal_denom) * sm->high_fg_temp_p_cal_fact;
		n_fg_delta_cal = (gap / sm->high_fg_temp_n_cal_denom) * sm->high_fg_temp_n_cal_fact;
	} else if (sm->en_low_fg_temp_cal && fg_temp_gap < 0) {
		gap = -fg_temp_gap;

		p_fg_delta_cal = (gap / sm->low_fg_temp_p_cal_denom) * sm->low_fg_temp_p_cal_fact;
		n_fg_delta_cal = (gap / sm->low_fg_temp_n_cal_denom) * sm->low_fg_temp_n_cal_fact;
	}
	p_curr_cal += p_fg_delta_cal;
	n_curr_cal += n_fg_delta_cal;

	temp_gap = sm->batt_temp - sm->temp_std;
	if (sm->en_high_temp_cal && temp_gap > 0) {
		gap = temp_gap;

		p_delta_cal = (gap / sm->high_temp_p_cal_denom) * sm->high_temp_p_cal_fact;
		n_delta_cal = (gap / sm->high_temp_n_cal_denom) * sm->high_temp_n_cal_fact;
	} else if (sm->en_low_temp_cal && (temp_gap < 0)) {
		gap = -temp_gap;

		p_delta_cal = (gap / sm->low_temp_p_cal_denom) * sm->low_temp_p_cal_fact;
		n_delta_cal = (gap / sm->low_temp_n_cal_denom) * sm->low_temp_n_cal_fact;
	}
	p_curr_cal += p_delta_cal;
	n_curr_cal += n_delta_cal;

	if (sm->fast_mode)
		p_curr_cal += sm->fcm_offset;

	curr_cal = (n_curr_cal << 8) | (p_curr_cal & 0xFF);
	ret = fg_write_word(sm, FG_REG_CURR_IN_SLOPE, (u16)curr_cal);
	if (ret < 0) {
		sm_err("Failed to write CURR_IN_SLOPE, ret=%d\n", ret);
		return ret;
	}
	sm_dbg("CURR_IN_SLOPE [0x%x] = 0x%x\n", FG_REG_CURR_IN_SLOPE, curr_cal);

#ifdef ENABLE_MIX_COMP
	ret = fg_read_word(sm, FG_REG_AGING_CTRL, &temp_aging_ctrl);
	if (ret < 0) {
		sm_err("Couldn't read FG_REG_AGING_CTRL, ret=%d\n", ret);
		return ret;
	}
	if (sm->batt_temp < 8 && !sm->is_charging &&
	    ((sm->batt_soc < 100 && sm->batt_soc > 20) ||
	    (sm->batt_soc < 300 && sm->batt_soc > 200) ||
	    (sm->batt_soc < 500 && sm->batt_soc > 400))) {
		if (sm->aging_ctrl == temp_aging_ctrl)
			ret = fg_write_word(sm, FG_REG_AGING_CTRL, (sm->aging_ctrl & 0xFFFE));
	} else {
		if (sm->aging_ctrl != temp_aging_ctrl)
			ret = fg_write_word(sm, FG_REG_AGING_CTRL, sm->aging_ctrl);
	}
	if (ret < 0) {
		sm_err("Couldn't write FG_REG_AGING_CTRL, ret=%d\n", ret);
		return ret;
	}
	sm_dbg("FG_REG_AGING_CTRL=0x%x\n", temp_aging_ctrl);
#endif

	return 0;
}

static int fg_get_batt_status(struct sm_fg_chip *sm)
{
	bool charge_done = false;
	int soc = 0;

	if (sm->batt_fc)
		charge_done = bq2589x_is_charge_done();

	soc = clamp(DIV_ROUND_CLOSEST(sm->batt_soc, 10), 0, 100);

	if (!sm->batt_present)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	else if (sm->batt_fc && charge_done && (soc > 95))
		return POWER_SUPPLY_STATUS_FULL;
	else if (sm->batt_dsg)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	else if (sm->batt_curr > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int fg_get_batt_capacity_level(struct sm_fg_chip *sm)
{
	if (!sm->batt_present)
		return POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	else if (sm->batt_fc)
		return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (sm->batt_soc1)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (sm->batt_socp)
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

static int fg_get_batt_health(struct sm_fg_chip *sm)
{
	if (!sm->batt_present)
		return POWER_SUPPLY_HEALTH_UNKNOWN;
#ifdef ENABLE_NTC_COMPENSATION
	else if (sm->batt_temp >= OVERHEAT_TH_DEG)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (sm->batt_temp <= COLD_TH_DEG)
		return POWER_SUPPLY_HEALTH_COLD;
#else
	else if (sm->batt_ot)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (sm->batt_ut)
		return POWER_SUPPLY_HEALTH_COLD;
#endif
	else
		return POWER_SUPPLY_HEALTH_GOOD;
}

static int get_battery_id(struct sm_fg_chip *sm)
{
	int battery_id = BATTERY_VENDOR_UNKNOWN;
	union power_supply_propval pval = {0, };
	u8 p0_buf[16];
	int rc;

	if (!sm->verify_psy)
		sm->verify_psy = power_supply_get_by_name("batt_verify");
	if (!sm->verify_psy) {
		sm_err("batt_verify psy not found\n");
		return battery_id;
	}

	rc = power_supply_get_property(sm->verify_psy,
				       POWER_SUPPLY_PROP_CHIP_OK, &pval);
	if (rc < 0) {
		sm_err("get CHIP_OK error: (%d)\n", rc);
		return battery_id;
	}

	if (pval.intval) {
		rc = power_supply_get_property(sm->verify_psy,
					       POWER_SUPPLY_PROP_PAGE0_DATA, &pval);
		if (rc < 0) {
			sm_err("get PAGE0_DATA error: (%d)\n", rc);
		} else {
			memcpy(p0_buf, pval.arrayval, 16);
			sm_dbg("PAGE0 raw: %*ph\n", 16, p0_buf);
			if (p0_buf[0] == 'N') {
				battery_id = BATTERY_VENDOR_NVT;
			} else if (p0_buf[0] == 0xFF || p0_buf[0] == 'C' || p0_buf[0] == 'V') {
				battery_id = BATTERY_VENDOR_GY;
			} else if (p0_buf[0] == 'L' || p0_buf[0] == 'X' || p0_buf[0] == 'S') {
				battery_id = BATTERY_VENDOR_XWD;
			} else {
				sm_err("Unknown PAGE0 signature: 0x%02x\n", p0_buf[0]);
				battery_id = BATTERY_VENDOR_UNKNOWN;
			}
		}
	}

	sm_info("battery_id = %d\n", battery_id);
	sm->battery_id = battery_id;
	return battery_id;
}

static enum power_supply_property fg_props[] = {
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	POWER_SUPPLY_PROP_AUTHENTIC,
	POWER_SUPPLY_PROP_ROMID,
	POWER_SUPPLY_PROP_DS_STATUS,
	POWER_SUPPLY_PROP_PAGE0_DATA,
	POWER_SUPPLY_PROP_CHIP_OK,
#endif
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SHUTDOWN_DELAY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_RESISTANCE_ID,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_SOC_DECIMAL,
	POWER_SUPPLY_PROP_SOC_DECIMAL_RATE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_FASTCHARGE_MODE,
	POWER_SUPPLY_PROP_BATTERY_TYPE,
	POWER_SUPPLY_PROP_RESISTANCE,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_SOH,
};

#define SHUTDOWN_DELAY_VOL	3400
#define SHUTDOWN_VOL	3300
static int fg_get_property(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct sm_fg_chip *sm = power_supply_get_drvdata(psy);
	union power_supply_propval b_val = {0, };
	int ret;
	int vbat_mv;
	int vbat_curr;
	int vbat_soc;

	switch (psp) {
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	case POWER_SUPPLY_PROP_AUTHENTIC:
		ret = power_supply_get_property(sm->verify_psy,
						POWER_SUPPLY_PROP_AUTHEN_RESULT,
						&b_val);
		val->intval = b_val.intval;
		break;
	case POWER_SUPPLY_PROP_ROMID:
		ret = power_supply_get_property(sm->verify_psy,
						POWER_SUPPLY_PROP_ROMID,
						&b_val);
		memcpy(val->arrayval, b_val.arrayval, 8);
		break;
	case POWER_SUPPLY_PROP_DS_STATUS:
		ret = power_supply_get_property(sm->verify_psy,
						POWER_SUPPLY_PROP_DS_STATUS,
						&b_val);
		memcpy(val->arrayval, b_val.arrayval, 8);
		break;
	case POWER_SUPPLY_PROP_PAGE0_DATA:
		ret = power_supply_get_property(sm->verify_psy,
						POWER_SUPPLY_PROP_PAGE0_DATA,
						&b_val);
		memcpy(val->arrayval, b_val.arrayval, 16);
		break;
	case POWER_SUPPLY_PROP_CHIP_OK:
		ret = power_supply_get_property(sm->verify_psy,
						POWER_SUPPLY_PROP_CHIP_OK,
						&b_val);
		val->intval = b_val.intval;
		break;
#endif
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg_get_batt_status(sm);
		/*sm_info("fg POWER_SUPPLY_PROP_STATUS:%d\n", val->intval);*/
		break;
	case POWER_SUPPLY_PROP_SHUTDOWN_DELAY:
		val->intval = sm->shutdown_delay;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (sm->usb_present && sm->cp_work_flag) {
			ret = power_supply_get_property(sm->cp_psy,
							POWER_SUPPLY_PROP_SC_BATTERY_VOLTAGE,
							&b_val);
			val->intval = b_val.intval * 1000;
			break;
		}

		ret = fg_read_volt(sm);
		mutex_lock(&sm->data_lock);
		val->intval = sm->batt_volt * 1000;
		mutex_unlock(&sm->data_lock);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = sm->batt_present;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = fg_read_current(sm);
		mutex_lock(&sm->data_lock);
#ifdef ENABLE_CURRENT_AVG
		vbat_curr = sm->param.batt_ma_avg * 1000;
#else
		vbat_curr = sm->batt_curr * 1000;
#endif
		mutex_unlock(&sm->data_lock);
		val->intval = clamp(vbat_curr, -10000000, 10000000);
		//fix xts issue the current shouled be positive when charging
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (sm->fake_soc >= 0) {
			val->intval = sm->fake_soc;
			break;
		}

		ret = fg_read_soc(sm);
		mutex_lock(&sm->data_lock);
		if (sm->param.batt_soc >= 0)
			vbat_soc = sm->param.batt_soc / 10;
		else if ((ret == 0) && (sm->param.batt_soc == -EINVAL))
			vbat_soc = (sm->batt_soc >= 970) ? 100 : (sm->batt_soc * 10 + 96) / 97;
		else
			vbat_soc = 50;
		mutex_unlock(&sm->data_lock);

		// makesure always valid range
		val->intval = clamp(vbat_soc, 0, 100);

		if (sm->shutdown_delay_enable) {
			if (val->intval == 0) {
				sm->is_charging = is_battery_charging(sm);
				vbat_mv = sm->batt_volt;

				if (vbat_mv > SHUTDOWN_DELAY_VOL) {
					val->intval = 1;
					if (sm->is_charging)
						sm->shutdown_delay = false;
				} else if (vbat_mv > SHUTDOWN_VOL) {
					if (!sm->is_charging) {
						sm->shutdown_delay = true;
						val->intval = 1;
					} else {
						sm->shutdown_delay = false;
						val->intval = 1;
					}
				} else {
					sm->shutdown_delay = false;
					val->intval = 0;
				}
			} else {
				sm->shutdown_delay = false;
			}
			if (sm->last_shutdown_delay != sm->shutdown_delay) {
				sm->last_shutdown_delay = sm->shutdown_delay;
				if (sm->batt_psy)
					power_supply_changed(sm->batt_psy);
				if (sm->fg_psy)
					power_supply_changed(sm->fg_psy);
			}
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = fg_get_batt_capacity_level(sm);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (sm->fake_temp != -EINVAL) {
			val->intval = sm->fake_temp;
			break;
		}

		if (sm->en_temp_in)
			ret = fg_read_temperature(sm, TEMPERATURE_IN);
		else if (sm->en_temp_ex)
			ret = fg_read_temperature(sm, TEMPERATURE_EX);
		else
			ret = -ENODATA;

		mutex_lock(&sm->data_lock);
		if (ret == -ENODATA)
			val->intval = 25 * 10; //1.0degree = 10
#ifdef ENABLE_TEMP_AVG
		else if (sm->param.batt_temp >= EX_TEMP_MIN &&
			 sm->param.batt_temp <= EX_TEMP_MAX)
			val->intval = sm->param.batt_temp_avg * 10; //1.0degree = 10
#endif
		else
			val->intval = sm->batt_temp * 10; //1.0degree = 10
		mutex_unlock(&sm->data_lock);
		break;
	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		if (sm->battery_id == BATTERY_VENDOR_UNKNOWN)
			sm->battery_id = get_battery_id(sm);
		val->intval = sm->battery_id;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = fg_read_fcc(sm);
		mutex_lock(&sm->data_lock);
		val->intval = sm->batt_fcc * 1000;
		mutex_unlock(&sm->data_lock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = 5000000;
		break;
	case POWER_SUPPLY_PROP_SOC_DECIMAL:
		val->intval = fg_get_soc_decimal(sm);
		break;
	case POWER_SUPPLY_PROP_SOC_DECIMAL_RATE:
		val->intval = fg_get_soc_decimal_rate(sm);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = fg_get_batt_health(sm);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
		val->intval = sm->fast_mode;
		break;
	case POWER_SUPPLY_PROP_BATTERY_TYPE:
		if (sm->battery_id == BATTERY_VENDOR_UNKNOWN)
			sm->battery_id = get_battery_id(sm);
		switch (sm->battery_id) {
		case BATTERY_VENDOR_NVT:
			val->strval = "M376-NVT-5000mAh";
			break;
		case BATTERY_VENDOR_GY:
			val->strval = "M376-GuanYu-5000mAh";
			break;
		case BATTERY_VENDOR_XWD:
			val->strval = "M376-Sunwoda-5000mAh";
			break;
		default:
			val->strval = "M376-unknown-5000mAh";
			break;
		}
		break;
	case POWER_SUPPLY_PROP_RESISTANCE:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = fg_get_cycle(sm);
		mutex_lock(&sm->data_lock);
		val->intval = sm->batt_soc_cycle;
		mutex_unlock(&sm->data_lock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		ret = fg_read_rmc(sm);
		mutex_lock(&sm->data_lock);
		val->intval = sm->batt_rmc * 1000; //uAh
		mutex_unlock(&sm->data_lock);
		break;
	case POWER_SUPPLY_PROP_SOH:
		val->intval = 100;
		break;
	default:
		sm_dbg("unsupported_property psp=%d\n", psp);
		return -EINVAL;
	}

	return 0;
}

static int fg_set_property(struct power_supply *psy,
			   enum power_supply_property prop,
			   const union power_supply_propval *val)
{
	struct sm_fg_chip *sm = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
		sm->fake_temp = val->intval;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		sm->fake_soc = val->intval;
		power_supply_changed(sm->fg_psy);
		break;
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
		sm->fast_mode = !!val->intval;
		break;
	case POWER_SUPPLY_PROP_CHIP_OK:
		sm->fake_chip_ok = !!val->intval;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fg_prop_is_writeable(struct power_supply *psy,
				enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
	case POWER_SUPPLY_PROP_CHIP_OK:
		return 1;
	default:
		return 0;
	}
}

static void fg_external_power_changed(struct power_supply *psy)
{
	struct sm_fg_chip *sm = power_supply_get_drvdata(psy);

	cancel_delayed_work_sync(&sm->monitor_work);
	queue_delayed_work(sm->main_wq, &sm->monitor_work, 0);
}

static int fg_psy_register(struct sm_fg_chip *sm)
{
	struct power_supply_config fg_psy_cfg = {};

	sm->fg_psy_d.name = "bms";
	sm->fg_psy_d.type = POWER_SUPPLY_TYPE_BMS;
	sm->fg_psy_d.properties = fg_props;
	sm->fg_psy_d.num_properties = ARRAY_SIZE(fg_props);
	sm->fg_psy_d.get_property = fg_get_property;
	sm->fg_psy_d.set_property = fg_set_property;
	sm->fg_psy_d.external_power_changed = fg_external_power_changed;
	sm->fg_psy_d.property_is_writeable = fg_prop_is_writeable;

	fg_psy_cfg.drv_data = sm;
	fg_psy_cfg.of_node = sm->dev->of_node;
	fg_psy_cfg.num_supplicants = 0;

	sm->fg_psy = devm_power_supply_register(sm->dev,
						&sm->fg_psy_d, &fg_psy_cfg);
	if (IS_ERR(sm->fg_psy)) {
		sm_err("failed to register fg_psy\n");
		return PTR_ERR(sm->fg_psy);
	}

	sm_info("%s power supply register successfully\n", sm->fg_psy_d.name);

	return 0;
}

static void fg_psy_unregister(struct sm_fg_chip *sm)
{
	sm_info("start unregister\n");
}

static const u8 fg_dump_regs[] = {
	0x00, 0x01, 0x03, 0x04,
	0x05, 0x06, 0x07, 0x08,
	0x09, 0x0A, 0x0C, 0x0D,
	0x0E, 0x0F, 0x10, 0x11,
	0x12, 0x13, 0x14, 0x1A,
	0x1B, 0x1C, 0x62, 0x73,
	0x74, 0x90, 0x91, 0x95,
	0x96
};

static int fg_dump_debug(struct seq_file *m, void *data)
{
	struct sm_fg_chip *sm = m->private;
	int i;
	int ret;
	u16 val = 0;

	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		ret = fg_read_word(sm, fg_dump_regs[i], &val);
		if (!ret)
			seq_printf(m, "Reg[0x%02X] = 0x%04X\n", fg_dump_regs[i], val);
	}

	return 0;
}

static int reg_debugfs_open(struct inode *inode, struct file *file)
{
	struct sm_fg_chip *sm = inode->i_private;

	return single_open(file, fg_dump_debug, sm);
}

static const struct file_operations reg_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= reg_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void create_debugfs_entry(struct sm_fg_chip *sm)
{
	sm->debug_root = debugfs_create_dir("sm_fg", NULL);

	if (!sm->debug_root) {
		sm_err("failed to create debug dir\n");
		return;
	}

	debugfs_create_file("registers",
			    S_IFREG | 0444,
				sm->debug_root, sm,
				&reg_debugfs_ops);
	debugfs_create_x32("fake_soc",
			   S_IFREG | 0644,
				sm->debug_root,
				&sm->fake_soc);
	debugfs_create_x32("fake_temp",
			   S_IFREG | 0644,
				sm->debug_root,
				&sm->fake_temp);
	debugfs_create_x32("skip_reads",
			   S_IFREG | 0644,
				sm->debug_root,
				&sm->skip_reads);
	debugfs_create_x32("skip_writes",
			   S_IFREG | 0644,
				sm->debug_root,
				&sm->skip_writes);
}

static ssize_t registers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int i, ret, idx = 0;
	u16 val = 0;
	u8 reg;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	idx += scnprintf(buf + idx, PAGE_SIZE - idx, "FG registers:\n");

	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		reg = fg_dump_regs[i];

		ret = fg_read_word(sm, reg, &val);
		if (ret) {
			dev_warn(sm->dev, "read reg 0x%02x failed: %d\n", reg, ret);
			idx += scnprintf(buf + idx, PAGE_SIZE - idx,
					"Reg[0x%02x] = <err %d>\n", reg, ret);
		} else {
			idx += scnprintf(buf + idx, PAGE_SIZE - idx,
					"Reg[0x%02x] = 0x%04X\n", reg, val);
		}

		if (idx >= PAGE_SIZE)
			break;
	}

	return idx;
}

static ssize_t registers_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	char tmp[48];
	unsigned long reg_ul = 0, val_ul = 0;
	char *p;
	int ret, i;
	bool ok = false;

	if (IS_ERR_OR_NULL(sm))
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

	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		if (reg_ul == fg_dump_regs[i]) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		dev_err(sm->dev, "invalid reg: 0x%lx not in dump list\n", reg_ul);
		return -EINVAL;
	}

	if (val_ul > 0xffff) {
		dev_err(sm->dev, "invalid val: 0x%lx\n", val_ul);
		return -EINVAL;
	}

	ret = fg_write_word(sm, (u16)reg_ul, (u16)val_ul);
	if (ret) {
		dev_err(sm->dev, "write reg 0x%04lx failed: %d\n", reg_ul, ret);
		return ret;
	}

	dev_info(sm->dev, "wrote 0x%04lx to reg 0x%04lx\n", val_ul, reg_ul);
	return count;
}

static ssize_t batt_rmc_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int rmc;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	fg_read_rmc(sm);
	mutex_lock(&sm->data_lock);
	rmc = sm->batt_rmc;
	mutex_unlock(&sm->data_lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", rmc);
}

static ssize_t batt_fcc_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int fcc;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	fg_read_fcc(sm);
	mutex_lock(&sm->data_lock);
	fcc = sm->batt_fcc;
	mutex_unlock(&sm->data_lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", fcc);
}

static ssize_t batt_volt_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int volt;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	fg_read_volt(sm);
	mutex_lock(&sm->data_lock);
	volt = sm->batt_volt;
	mutex_unlock(&sm->data_lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", volt);
}

static ssize_t batt_curr_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int curr;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	fg_read_current(sm);
	mutex_lock(&sm->data_lock);
#ifdef ENABLE_CURRENT_AVG
	curr = sm->param.batt_ma_avg;
#else
	curr = sm->batt_curr;
#endif
	mutex_unlock(&sm->data_lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", curr);
}

static ssize_t batt_cycle_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sm_fg_chip *sm = i2c_get_clientdata(client);
	int cycle;

	if (IS_ERR_OR_NULL(sm))
		return -ENODEV;

	fg_get_cycle(sm);
	mutex_lock(&sm->data_lock);
	cycle = sm->batt_soc_cycle;
	mutex_unlock(&sm->data_lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", cycle);
}

static DEVICE_ATTR_RO(batt_rmc);
static DEVICE_ATTR_RO(batt_fcc);
static DEVICE_ATTR_RO(batt_volt);
static DEVICE_ATTR_RO(batt_curr);
static DEVICE_ATTR_RO(batt_cycle);
static DEVICE_ATTR_RW(registers);

static struct attribute *fg_attributes[] = {
	&dev_attr_batt_rmc.attr,
	&dev_attr_batt_fcc.attr,
	&dev_attr_batt_volt.attr,
	&dev_attr_batt_curr.attr,
	&dev_attr_batt_cycle.attr,
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group fg_attr_group = {
	.attrs = fg_attributes,
};

#define MONITOR_SOC_WAIT_MS		1000
#define MONITOR_SOC_WAIT_PER_MS		10000
static int fg_refresh_status(struct sm_fg_chip *sm)
{
	bool prev_batt_present, prev_batt_fc, prev_batt_ot, prev_batt_ut;
	int prev_batt_soc, prev_batt_temp;
	union power_supply_propval pval = {0,};
	int cp_vbat = 0, rc;
	int interval;

	mutex_lock(&sm->data_lock);
	prev_batt_present = sm->batt_present;
	prev_batt_fc = sm->batt_fc;
	prev_batt_ot = sm->batt_ot;
	prev_batt_ut = sm->batt_ut;
	prev_batt_soc = sm->batt_soc;
	prev_batt_temp = sm->batt_temp;
	mutex_unlock(&sm->data_lock);

	fg_read_status(sm);

	if (!sm->batt_present) {
		if (prev_batt_present) {
			sm_info("Battery removed\n");
			mutex_lock(&sm->data_lock);
			sm->batt_soc = -ENODATA;
			sm->batt_fcc = -ENODATA;
			sm->batt_volt = -ENODATA;
			sm->batt_curr = -ENODATA;
			sm->batt_temp = -ENODATA;
			mutex_unlock(&sm->data_lock);
			power_supply_changed(sm->fg_psy);
		}
		return MONITOR_WORK_10S;
	}

	if (!prev_batt_present)
		sm_info("Battery inserted\n");

	if (prev_batt_present != sm->batt_present ||
	    prev_batt_fc != sm->batt_fc ||
	    prev_batt_ot != sm->batt_ot ||
	    prev_batt_ut != sm->batt_ut)
		power_supply_changed(sm->fg_psy);

	fg_read_soc(sm);
	fg_read_ocv(sm);
	fg_read_fcc(sm);
	fg_read_volt(sm);
	fg_read_current(sm);
	fg_get_cycle(sm);
	fg_read_rmc(sm);
	if (sm->en_temp_in)
		fg_read_temperature(sm, TEMPERATURE_IN);
	else if (sm->en_temp_ex)
		fg_read_temperature(sm, TEMPERATURE_EX);

	fg_cal_carc(sm);

	if (prev_batt_soc != sm->batt_soc ||
	    prev_batt_temp != sm->batt_temp) {
		if (sm->fg_psy)
			power_supply_changed(sm->fg_psy);
		if (sm->batt_psy)
			power_supply_changed(sm->batt_psy);
	}

	if (sm->usb_present && sm->cp_work_flag && sm->cp_psy) {
		rc = power_supply_get_property(sm->cp_psy,
					       POWER_SUPPLY_PROP_SC_BATTERY_VOLTAGE, &pval);
		if (!rc)
			cp_vbat = pval.intval;
	}

	sm_log("raw_soc: %d, volt: %d, cp_volt: %d, current: %d, fast_mode: %d\n",
	       sm->batt_soc, sm->batt_volt, cp_vbat, sm->batt_curr, sm->fast_mode);
	sm_log("cycle_count: %d, charge_counter: %d, fcc: %d, ocv: %d\n",
	       sm->batt_soc_cycle, sm->batt_rmc, sm->batt_fcc, sm->batt_ocv);

	if (sm->fast_mode || (sm->usb_present && sm->batt_soc >= SM_RAW_SOC_FULL))
		interval = MONITOR_WORK_1S;
	else if (sm->usb_present)
		interval = MONITOR_WORK_5S;
	else
		interval = MONITOR_WORK_10S;

	return interval;
}

static void fg_update_low_battery(struct sm_fg_chip *sm)
{
	union power_supply_propval pval = {0,};
	bool usb_online = false;
	int rc;

	if (sm->usb_psy) {
		rc = power_supply_get_property(sm->usb_psy,
					       POWER_SUPPLY_PROP_ONLINE, &pval);
		if (!rc)
			usb_online = !!pval.intval;
		else
			sm_err("Couldn't get prop usb online!\n");
	}

	if (usb_online) {
		if (sm->start_low_battery_check) {
			cancel_delayed_work(&sm->low_battery_check_work);
			sm->start_low_battery_check = false;
			sm->low_battery_power = false;
		}
		return;
	}

	if (sm->batt_soc < 10 && sm->batt_volt < 3400 && !sm->low_battery_power) {
		if (!sm->start_low_battery_check) {
			sm->start_low_battery_check = true;
			queue_delayed_work(sm->aux_wq, &sm->low_battery_check_work, 0);
		}
	} else if (sm->start_low_battery_check &&
	    (sm->batt_soc >= 10 || sm->batt_volt >= 3400)) {
		cancel_delayed_work(&sm->low_battery_check_work);
		sm->start_low_battery_check = false;
		sm->low_battery_power = false;
	}

	sm_log("low_battery_check: %d, low_battery_power: %d\n",
	       sm->start_low_battery_check, sm->low_battery_power);
}

#define SM5602_FFC_TERM_WAM_TEMP		350
#define SM5602_COLD_TEMP_TERM			0
#define SM5602_FFC_FULL_FV				8940
#define SM5602_NOR_FULL_FV				8880
#define BAT_FULL_CHECK_TIME				1
static void fg_check_full_status(struct sm_fg_chip *sm)
{
	int full_volt;

	if (!sm->chg_dis_votable)
		sm->chg_dis_votable = find_votable("CHG_DISABLE");
	if (!sm->chg_dis_votable)
		return;

	if (!sm->fv_votable)
		sm->fv_votable = find_votable("FV");
	if (!sm->fv_votable)
		return;

	if (!sm->usb_present) {
		if (sm->batt_sw_fc) {
			vote(sm->chg_dis_votable, BMS_FC_VOTER, false, 0);
			sm->batt_sw_fc = false;
			sm_info("USB removed: reset full state\n");
		}
		sm->full_check = 0;
		return;
	}

	full_volt = get_effective_result(sm->fv_votable) - 20;

	if (sm->batt_soc >= SM_RAW_SOC_FULL &&
	    sm->batt_volt > full_volt &&
	    sm->batt_curr > 0 &&
	    !sm->batt_sw_fc) {
		if (++sm->full_check >= BAT_FULL_CHECK_TIME) {
			sm->batt_sw_fc = true;
			vote(sm->chg_dis_votable, BMS_FC_VOTER, true, 0);
			sm_info("charge termination -> batt_sw_fc=%d\n",
				sm->batt_sw_fc);
		}
	} else {
		sm->full_check = 0;
	}
}

#define BAT_WARM_TEMP	48
static void fg_check_recharge_status(struct sm_fg_chip *sm)
{
	if (!sm->chg_dis_votable)
		sm->chg_dis_votable = find_votable("CHG_DISABLE");
	if (!sm->chg_dis_votable)
		return;

	/* We don't need batt_status, use batt_sw_fc instead */
	/* Makesure on safe batt_soc & good temp */
	if ((sm->batt_soc <= SM_RECHARGE_SOC && sm->batt_temp < BAT_WARM_TEMP) &&
	    sm->batt_sw_fc) {
		sm->batt_sw_fc = false;
		vote(sm->chg_dis_votable, BMS_FC_VOTER, false, 0);
		sm_info("force recharging\n");
	}
}

static void fg_monitor_workfunc(struct work_struct *work)
{
	struct sm_fg_chip *sm = container_of(work, struct sm_fg_chip, monitor_work.work);
	int interval;

	fg_init(sm);

	interval = fg_refresh_status(sm);
	fg_update_low_battery(sm);
	fg_check_full_status(sm);
	fg_check_recharge_status(sm);

	queue_delayed_work(sm->main_wq, &sm->monitor_work, interval * HZ);
}

static void sm_kref_release(struct kref *k)
{
	struct sm_fg_chip *sm = container_of(k, struct sm_fg_chip, kref);

	sm_info("sm release %p, name=%s\n", sm, sm->name);
}

struct sm_fg_chip *fg_get_sm(const char *name)
{
	struct sm_fg_chip *sm;

	if (!name)
		return NULL;

	mutex_lock(&sm_list_lock);
	list_for_each_entry(sm, &sm_list, node) {
		if (strcmp(sm->name, name) == 0) {
			if (kref_get_unless_zero(&sm->kref)) {
				mutex_unlock(&sm_list_lock);
				return sm;
			}
			break;
		}
	}
	mutex_unlock(&sm_list_lock);
	return NULL;
}
EXPORT_SYMBOL_GPL(fg_get_sm);

void fg_put_sm(struct sm_fg_chip *sm)
{
	if (!sm)
		return;
	kref_put(&sm->kref, sm_kref_release);
}
EXPORT_SYMBOL_GPL(fg_put_sm);

void start_fg_monitor(struct sm_fg_chip *sm)
{
	if (!sm)
		return;
	queue_delayed_work(sm->main_wq, &sm->monitor_work, 0);
}
EXPORT_SYMBOL(start_fg_monitor);

void stop_fg_monitor(struct sm_fg_chip *sm)
{
	if (!sm)
		return;
	cancel_delayed_work_sync(&sm->monitor_work);
}
EXPORT_SYMBOL(stop_fg_monitor);

#define COMMON_PARAM_MASK	0xFF00
#define COMMON_PARAM_SHIFT	8
#define BATTERY_PARAM_MASK	0x00FF
static bool fg_check_reg_init_need(struct sm_fg_chip *sm)
{
	int ret;
	u16 data = 0;
	u16 param_ver = 0;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_FG_OP_STATUS], &data);
	if (ret < 0) {
		sm_err("Failed to read FG_OP_STATUS(%d)\n", ret);
		return true;
	}
	sm_dbg("FG_OP_STATUS = 0x%x\n", data);

	ret = fg_read_word(sm, FG_PARAM_VERION, &param_ver);
	if (ret < 0) {
		sm_err("Failed to read FG_PARAM_VERION(%d)\n", ret);
		return true;
	}

	sm_dbg("param_version = 0x%x, common=%u, battery=%u\n",
	       param_ver, sm->common_param_version, sm->battery_param_version);

	if (((data & INIT_CHECK_MASK) == DISABLE_RE_INIT) &&
	    (((param_ver & COMMON_PARAM_MASK) >> COMMON_PARAM_SHIFT) >= sm->common_param_version) &&
	    ((param_ver & BATTERY_PARAM_MASK) >= sm->battery_param_version)) {
		sm_dbg("FG_OP_STATUS: 0x%x, no init needed\n", data);
		return false;
	}

	sm_err("FG_OP_STATUS: 0x%x, init required\n", data);
	return true;
}

static int fg_read_v_i_sample(struct sm_fg_chip *sm, int idx, u16 *v_out, int *i_out)
{
	u16 v_ret, i_ret;
	int ret;

	ret = fg_write_word(sm, 0x8C, idx);
	if (ret < 0) {
		sm_err("Failed to write 0x8C idx=0x%x, ret=%d\n", idx, ret);
		return ret;
	}
	usleep_range(15000, 16000);
	ret = fg_read_word(sm, 0x8D, &v_ret);
	if (ret >= 0 && (v_ret & 0x8000)) {
		usleep_range(15000, 16000);
		ret = fg_read_word(sm, 0x8D, &v_ret);
	}
	if (ret < 0) {
		sm_err("Failed to read v idx=0x%x, ret=%d\n", idx, ret);
		return ret;
	}

	ret = fg_write_word(sm, 0x8C, idx + 0x10);
	if (ret < 0) {
		sm_err("Failed to write 0x8C idx=0x%x, ret=%d\n", idx + 0x10, ret);
		return ret;
	}
	usleep_range(15000, 16000);
	ret = fg_read_word(sm, 0x8D, &i_ret);
	if (ret >= 0 && (i_ret & 0x8000)) {
		usleep_range(15000, 16000);
		ret = fg_read_word(sm, 0x8D, &i_ret);
	}
	if (ret < 0) {
		sm_err("Failed to read i idx=0x%x, ret=%d\n", idx, ret);
		return ret;
	}

	*v_out = v_ret;
	*i_out = (i_ret & 0x4000) ? -(int)(i_ret & 0x3FFF) : (int)i_ret;
	return 0;
}

static int fg_build_i_offset(int i_set, bool *sign_negative)
{
	int i_offset = i_set + 4;

	if (i_offset <= 0) {
		*sign_negative = true;
#ifdef IGNORE_N_I_OFFSET
		i_offset = 0;
#else
		i_offset = -i_offset;
#endif
	} else {
		*sign_negative = false;
	}

	i_offset >>= 1;
	if (!*sign_negative)
		i_offset |= 0x0080;
	i_offset |= i_offset << 8;

	return i_offset;
}

static int fg_calculate_iocv(struct sm_fg_chip *sm)
{
	bool only_lb = false, sign_i_offset = false;
	int roop_start = 0, roop_max = 0, i = 0;
	int cb_last_index = 0, cb_pre_last_index = 0;
	int lb_v_buffer[FG_INIT_B_LEN + 1] = { };
	int lb_i_buffer[FG_INIT_B_LEN + 1] = { };
	int cb_v_buffer[FG_INIT_B_LEN + 1] = { };
	int cb_i_buffer[FG_INIT_B_LEN + 1] = { };
	int i_offset_margin = 0x14, i_vset_margin = 0x67;
	int v_max = 0, v_min = 0, v_sum = 0;
	int i_max = 0, i_min = 0, i_sum = 0;
	int lb_v_avg = 0, lb_i_avg = 0, lb_v_set = 0, lb_i_set = 0;
	int cb_v_avg = 0, cb_i_avg = 0, cb_v_set = 0, cb_i_set = 0;
	int lb_i_p_v_min = 0, lb_i_n_v_max = 0;
	int cb_i_p_v_min = 0, cb_i_n_v_max = 0;
	int i_offset = 0, ret = 0;
	u16 v_ret = 0;
	int i_ret = 0;
	u16 data = 0;

	ret = fg_read_word(sm, FG_REG_END_V_IDX, &data);
	if (ret < 0) {
		sm_err("Failed to read FG_REG_END_V_IDX, ret=%d\n", ret);
		return ret;
	}
	sm_info("iocv_status_read = addr: 0x%x, data: 0x%x\n", FG_REG_END_V_IDX, data);

	only_lb = !(data & 0x0010);

	roop_max = data & 0x000F;
	if (roop_max > FG_INIT_B_LEN)
		roop_max = FG_INIT_B_LEN;

	roop_start = FG_REG_START_LB_V;
	for (i = roop_start; i < roop_start + roop_max; i++) {
		ret = fg_read_v_i_sample(sm, i, &v_ret, &i_ret);
		if (ret < 0)
			return ret;

		lb_v_buffer[i - roop_start] = v_ret;
		lb_i_buffer[i - roop_start] = i_ret;

		if (i == roop_start) {
			v_max = v_min = v_sum = v_ret;
			i_max = i_min = i_sum = i_ret;
		} else {
			v_max = max_t(int, v_max, v_ret);
			v_min = min_t(int, v_min, v_ret);
			v_sum += v_ret;
			i_max = max_t(int, i_max, i_ret);
			i_min = min_t(int, i_min, i_ret);
			i_sum += i_ret;
		}

		if (abs(i_ret) > i_vset_margin) {
			if (i_ret > 0)
				lb_i_p_v_min = lb_i_p_v_min ?
					min(lb_i_p_v_min, (int)v_ret) : v_ret;
			else
				lb_i_n_v_max = lb_i_n_v_max ?
					max(lb_i_n_v_max, (int)v_ret) : v_ret;
		}
	}

	v_sum -= v_max + v_min;
	i_sum -= i_max + i_min;

	lb_v_avg = v_sum / (roop_max - 2);
	lb_i_avg = i_sum / (roop_max - 2);

	if (abs(lb_i_buffer[roop_max - 1]) < i_vset_margin) {
		if (abs(lb_i_buffer[roop_max - 2]) < i_vset_margin) {
			lb_v_set = max(lb_v_buffer[roop_max - 2], lb_v_buffer[roop_max - 1]);
			if (abs(lb_i_buffer[roop_max - 3]) < i_vset_margin)
				lb_v_set = max(lb_v_buffer[roop_max - 3], lb_v_set);
		} else {
			lb_v_set = lb_v_buffer[roop_max - 1];
		}
	} else {
		lb_v_set = lb_v_avg;
	}

	if (lb_i_n_v_max > 0)
		lb_v_set = max(lb_i_n_v_max, lb_v_set);

	if (roop_max > 3)
		lb_i_set = (lb_i_buffer[2] + lb_i_buffer[3]) / 2;

	if ((abs(lb_i_buffer[roop_max - 1]) < i_offset_margin) &&
	    (abs(lb_i_set) < i_offset_margin)) {
		lb_i_set = max(lb_i_buffer[roop_max - 1], lb_i_set);
	} else if (abs(lb_i_buffer[roop_max - 1]) < i_offset_margin) {
		lb_i_set = lb_i_buffer[roop_max - 1];
	} else if (abs(lb_i_set) >= i_offset_margin) {
		lb_i_set = 0;
	}

	i_offset = fg_build_i_offset(lb_i_set, &sign_i_offset);

	if (!only_lb) {
		roop_start = FG_REG_START_CB_V;
		roop_max = 6;
		for (i = roop_start; i < roop_start + roop_max; i++) {
			ret = fg_read_v_i_sample(sm, i, &v_ret, &i_ret);
			if (ret < 0)
				return ret;

			cb_v_buffer[i - roop_start] = v_ret;
			cb_i_buffer[i - roop_start] = i_ret;

			if (i == roop_start) {
				v_max = v_min = v_sum = v_ret;
				i_max = i_min = i_sum = i_ret;
			} else {
				v_max = max_t(int, v_max, v_ret);
				v_min = min_t(int, v_min, v_ret);
				v_sum += v_ret;
				i_max = max_t(int, i_max, i_ret);
				i_min = min_t(int, i_min, i_ret);
				i_sum += i_ret;
			}

			if (abs(i_ret) > i_vset_margin) {
				if (i_ret > 0)
					cb_i_p_v_min = cb_i_p_v_min ?
						min(cb_i_p_v_min, (int)v_ret) : v_ret;
				else
					cb_i_n_v_max = cb_i_n_v_max ?
						max(cb_i_n_v_max, (int)v_ret) : v_ret;
			}
		}

		v_sum -= v_max + v_min;
		i_sum -= i_max + i_min;

		cb_v_avg = v_sum / (roop_max - 2);
		cb_i_avg = i_sum / (roop_max - 2);

		cb_last_index = (data & 0x000F) - 7;
		if (cb_last_index < 0)
			cb_last_index = 5;

		for (i = roop_max; i > 0; i--) {
			if (abs(cb_i_buffer[cb_last_index]) < i_vset_margin) {
				cb_v_set = cb_v_buffer[cb_last_index];
				if (abs(cb_i_buffer[cb_last_index]) < i_offset_margin)
					cb_i_set = cb_i_buffer[cb_last_index];

				cb_pre_last_index = cb_last_index - 1;
				if (cb_pre_last_index < 0)
					cb_pre_last_index = 5;

				if (abs(cb_i_buffer[cb_pre_last_index]) < i_vset_margin) {
					cb_v_set = max(cb_v_buffer[cb_pre_last_index], cb_v_set);
					if (abs(cb_i_buffer[cb_pre_last_index]) < i_offset_margin)
						cb_i_set = max(cb_i_buffer[cb_pre_last_index], cb_i_set);
				}
				break;
			}
			cb_last_index--;
			if (cb_last_index < 0)
				cb_last_index = 5;
		}

		if (cb_v_set == 0) {
			cb_v_set = cb_v_avg;
			if (cb_i_set == 0)
				cb_i_set = cb_i_avg;
		}

		if (cb_i_n_v_max > 0)
			cb_v_set = max(cb_i_n_v_max, cb_v_set);

		if (abs(cb_i_set) < i_offset_margin && cb_i_set > lb_i_set)
			i_offset = fg_build_i_offset(cb_i_set, &sign_i_offset);
	}

	/* final set */
	if (abs(cb_i_set) > i_vset_margin || only_lb) {
		ret = max(lb_v_set, cb_i_n_v_max);
		cb_i_set = lb_i_avg;
	} else {
		ret = cb_v_set;
		cb_i_set = cb_i_avg;
	}

#ifdef ENABLE_IOCV_ADJ
	if (ret < IOCV_MAX_ADJ_LEVEL && ret > IOCV_MIN_ADJ_LEVEL &&
	    abs(cb_i_set) < IOCI_MAX_ADJ_LEVEL && abs(cb_i_set) > IOCI_MIN_ADJ_LEVEL) {
		cb_v_set = ret;
		ret = ret - (((cb_i_set * IOCV_I_SLOPE) + IOCV_I_OFFSET) / 1000);
		sm_info("first boot vbat-soc adjust 1st_v=0x%x, 2nd_v=0x%x, all_i=0x%x\n",
			cb_v_set, ret, cb_i_set);
	}
#endif

	if (ret > sm->battery_table[BATTERY_TABLE0][FG_TABLE_LEN - 1])
		ret = sm->battery_table[BATTERY_TABLE0][FG_TABLE_LEN - 1];
	else if (ret < sm->battery_table[BATTERY_TABLE0][0])
		ret = sm->battery_table[BATTERY_TABLE0][0] + 0x10;

	return ret;
}

static bool fg_reg_init(struct sm_fg_chip *sm)
{
	bool ok = false;
	int i, j, value, ret, cnt = 0;
	u8 table_reg;
	u16 data, data_int_mask;
	u16 cntl_mask, cntl_val;

	sm_info("start\n");

	mutex_lock(&sm->data_lock);

	cntl_mask = ENABLE_EN_TEMP_IN |
			ENABLE_EN_TEMP_EX |
			ENABLE_EN_BATT_DET;
	cntl_val = (sm->en_temp_in ? ENABLE_EN_TEMP_IN : 0) |
			(sm->en_temp_ex ? ENABLE_EN_TEMP_EX : 0) |
			(sm->en_batt_det ? ENABLE_EN_BATT_DET : 0);

	ret = fg_update_bits(sm, sm->regs[SM_FG_REG_CNTL], cntl_mask, cntl_val);
	if (ret < 0) {
		sm_err("Failed to update CNTL, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("CNTL[0x%02x] en_temp_in=%d en_temp_ex=%d en_batt_det=%d\n",
		sm->regs[SM_FG_REG_CNTL],
		sm->en_temp_in, sm->en_temp_ex, sm->en_batt_det);

	/* IRQ Mask */
	if (sm->fg_irq_set == -EINVAL) {
		sm_err("fg_irq_set is invalid\n");
	} else {
		ret = fg_read_word(sm, sm->regs[SM_FG_REG_INT_MASK], &data_int_mask);
		if (ret < 0) {
			sm_err("Failed to read INT_MASK, ret=%d\n", ret);
			goto unlock;
		}
		ret = fg_write_word(sm, sm->regs[SM_FG_REG_INT_MASK],
				    (u16)(0x4000 | (data_int_mask | (u16)sm->fg_irq_set)));
		if (ret < 0) {
			sm_err("Failed to write 0x4000 | INIT_MASK, ret=%d\n", ret);
			goto unlock;
		}
		ret = fg_write_word(sm, sm->regs[SM_FG_REG_INT_MASK],
				    (u16)(0x07FF & (data_int_mask | (u16)sm->fg_irq_set)));
		if (ret < 0) {
			sm_err("Failed to write INIT_MASK, ret=%d\n", ret);
			goto unlock;
		}
	}

	/* Low SOC1 */
	if (sm->low_soc1 == -EINVAL) {
		sm_err("low_soc1 is invalid\n");
	} else {
		ret = fg_update_bits(sm, sm->regs[SM_FG_REG_SOC_L_ALARM],
				     0x001F, (u16)(sm->low_soc1 & 0x001F));
		if (ret < 0) {
			sm_err("Failed to write SOC_L_ALARM (LOW_SOC1), ret=%d\n", ret);
			goto unlock;
		}
	}

	/* Low SOC2 */
	if (sm->low_soc2 == -EINVAL) {
		sm_err("low_soc2 is invalid\n");
	} else {
		ret = fg_update_bits(sm, sm->regs[SM_FG_REG_SOC_L_ALARM],
				     0x1F00, (u16)((sm->low_soc2 << 8) & 0x1F00));
		if (ret < 0) {
			sm_err("Failed to write SOC_L_ALARM (LOW_SOC2), ret=%d\n", ret);
			goto unlock;
		}
	}

	/* V L ALARM */
	if (sm->v_l_alarm == -EINVAL) {
		sm_err("v_l_alarm is invalid\n");
	} else {
		if (sm->v_l_alarm >= 2000 && sm->v_l_alarm < 3000) {
			data = (u16)(0xFEFF & (((u16)(sm->v_l_alarm / 10)) << 8));
		} else if (sm->v_l_alarm >= 3000 && sm->v_l_alarm < 4000) {
			data = (u16)(0x0100 | (((u16)(sm->v_l_alarm / 10)) << 8));
		} else {
			sm_err("Failed to calculate V_L_ALARM\n");
			goto unlock;
		}
		ret = fg_write_word(sm, sm->regs[SM_FG_REG_V_L_ALARM], data);
		if (ret < 0) {
			sm_err("Failed to write V_L_ALARM, ret=%d\n", ret);
			goto unlock;
		}
	}

	/* V H ALARM */
	if (sm->v_h_alarm == -EINVAL) {
		sm_err("v_h_alarm is invalid\n");
	} else {
		if (sm->v_h_alarm >= 3000 && sm->v_h_alarm < 4000) {
			data = (u16)(0xFEFF & (((u16)(sm->v_h_alarm / 10)) << 8));
		} else if (sm->v_h_alarm >= 4000 && sm->v_h_alarm < 5000) {
			data = (u16)(0x0100 | (((u16)(sm->v_h_alarm / 10)) << 8));
		} else {
			sm_err("Failed to calculate V_H_ALARM\n");
			goto unlock;
		}
		ret = fg_write_word(sm, sm->regs[SM_FG_REG_V_H_ALARM], data);
		if (ret < 0) {
			sm_err("Failed to write V_H_ALARM, ret=%d\n", ret);
			goto unlock;
		}
	}

	/* T IN H/L ALARM */
	if (sm->t_h_alarm_in == -EINVAL || sm->t_l_alarm_in == -EINVAL) {
		sm_err("t_h_alarm_in || sm->t_l_alarm_in is invalid\n");
	} else {
		data = 0;
		if (sm->t_h_alarm_in < 0)
			data |= (u16)(0x8000 | ((((u16)(-sm->t_h_alarm_in)) & 0x7F) << 8));
		else
			data |= (u16)(((u16)(sm->t_h_alarm_in) & 0x7F) << 8);

		if (sm->t_l_alarm_in < 0)
			data |= (u16)(0x0080 | (u16)((-sm->t_l_alarm_in) & 0x7F));
		else
			data |= (u16)(sm->t_l_alarm_in & 0x7F);

		ret = fg_write_word(sm, sm->regs[SM_FG_REG_T_IN_H_ALARM], data);
		if (ret < 0) {
			sm_err("Failed to write SM_FG_REG_T_IN_H_ALARM, ret=%d\n", ret);
			goto unlock;
		}
	}

	do {
		ret = fg_write_word(sm, sm->regs[SM_FG_REG_PARAM_CTRL],
				    FG_PARAM_UNLOCK_CODE |
				    ((sm->battery_table_num & 0x0003) << 6) |
				    (FG_TABLE_LEN - 1));
		if (ret < 0) {
			sm_err("Failed to write param_ctrl unlock, ret=%d\n", ret);
			goto unlock;
		}
		sm_info("Param Unlock\n");
		msleep(60);
		ret = fg_read_word(sm, sm->regs[SM_FG_REG_FG_OP_STATUS], &data);
		if (ret < 0)
			sm_err("Failed to read FG_OP_STATUS, ret = %d\n", ret);
		else
			sm_info("FG_OP_STATUS = 0x%x\n", data);
		cnt++;
	} while (((data & 0x03) != 0x03) && cnt <= 3);

	/* VIT_PERIOD write */
	ret = fg_write_word(sm, sm->regs[SM_FG_REG_VIT_PERIOD], sm->vit_period);
	if (ret < 0) {
		sm_err("Failed to write VIT PERIOD, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("Write VIT_PERIOD = 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_VIT_PERIOD], sm->vit_period);

	/* Aging ctrl write */
	ret = fg_write_word(sm, FG_REG_AGING_CTRL, sm->aging_ctrl);
	if (ret < 0) {
		sm_err("Failed to write FG_REG_AGING_CTRL, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("Write FG_REG_AGING_CTRL = 0x%x:0x%x\n",
		FG_REG_AGING_CTRL, sm->aging_ctrl);

	/* SOC Cycle ctrl write */
	ret = fg_write_word(sm, FG_REG_SOC_CYCLE_CFG, sm->cycle_cfg);
	if (ret < 0) {
		sm_err("Failed to write FG_REG_SOC_CYCLE_CFG, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("Write FG_REG_SOC_CYCLE_CFG = 0x%x:0x%x\n",
		FG_REG_SOC_CYCLE_CFG, sm->cycle_cfg);

	/* RSNS write */
	ret = fg_write_word(sm, sm->regs[SM_FG_REG_RSNS_SEL], sm->batt_rsns);
	if (ret < 0) {
		sm_err("Failed to write SM_FG_REG_RSNS_SEL, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("Write SM_FG_REG_RSNS_SEL = 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_RSNS_SEL], sm->batt_rsns);

	/* Battery_Table write */
	for (i = BATTERY_TABLE0; i < BATTERY_TABLE2; i++) {
		table_reg = 0xA0 + (i * FG_TABLE_LEN);
		for (j = 0; j < FG_TABLE_LEN; j++) {
			ret = fg_write_word(sm, (table_reg + j), sm->battery_table[i][j]);
			if (ret < 0) {
				sm_err("Failed to write Battery Table, ret=%d\n", ret);
				goto unlock;
			}
		}
	}

	for (j = 0; j < FG_ADD_TABLE_LEN; j++) {
		table_reg = 0xD0 + j;
		ret = fg_write_word(sm, table_reg, sm->battery_table[i][j]);
		if (ret < 0) {
			sm_err("Failed to write Battery Table, ret=%d\n", ret);
			goto unlock;
		}
	}

	/* RS write */
	ret = fg_write_word(sm, FG_REG_RS, sm->rs);
	if (ret < 0) {
		sm_err("Failed to write RS, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("RS = 0x%x:0x%x\n", FG_REG_RS, sm->rs);

	/* alpha write */
	ret = fg_write_word(sm, FG_REG_ALPHA, sm->alpha);
	if (ret < 0) {
		sm_err("Failed to write FG_REG_ALPHA, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("ALPHA = 0x%x:0x%x\n", FG_REG_ALPHA, sm->alpha);

	/* beta write */
	ret = fg_write_word(sm, FG_REG_BETA, sm->beta);
	if (ret < 0) {
		sm_err("Failed to write FG_REG_BETA, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("BETA = 0x%x:0x%x\n", FG_REG_BETA, sm->beta);

	/* RS_* write */
	ret = fg_write_word(sm, FG_REG_RS_0, sm->rs_value[0]);
	if (ret < 0) {
		sm_err("Failed to write RS_0, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("RS = 0x%x:0x%x\n", FG_REG_RS_0, sm->rs_value[0]);

	ret = fg_write_word(sm, FG_REG_RS_1, sm->rs_value[1]);
	if (ret < 0) {
		sm_err("Failed to write RS_1, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("RS_1 = 0x%x:0x%x\n", FG_REG_RS_1, sm->rs_value[1]);

	ret = fg_write_word(sm, FG_REG_RS_2, sm->rs_value[2]);
	if (ret < 0) {
		sm_err("Failed to write RS_2, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("RS_2 = 0x%x:0x%x\n", FG_REG_RS_2, sm->rs_value[2]);

	ret = fg_write_word(sm, FG_REG_RS_3, sm->rs_value[3]);
	if (ret < 0) {
		sm_err("Failed to write RS_3, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("RS_3 = 0x%x:0x%x\n", FG_REG_RS_3, sm->rs_value[3]);

	ret = fg_write_word(sm, sm->regs[SM_FG_REG_CURRENT_RATE], sm->mix_value);
	if (ret < 0) {
		sm_err("Failed to write CURRENT_RATE, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("CURRENT_RATE = 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_CURRENT_RATE], sm->mix_value);

	sm_info("RS_0 = 0x%x, RS_1 = 0x%x, RS_2 = 0x%x, RS_3 = 0x%x, CURRENT_RATE = 0x%x\n",
		sm->rs_value[0], sm->rs_value[1],
		sm->rs_value[2], sm->rs_value[3],
		sm->mix_value);

	/* VOLT_CAL write*/
	ret = fg_write_word(sm, FG_REG_VOLT_CAL, sm->volt_cal);
	if (ret < 0) {
		sm_err("Failed to write FG_REG_VOLT_CAL, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("FG_REG_VOLT_CAL = 0x%x:0x%x\n",
		FG_REG_VOLT_CAL, sm->volt_cal);

	/* CAL write */
	ret = fg_write_word(sm, FG_REG_CURR_IN_OFFSET, sm->curr_offset);
	if (ret < 0) {
		sm_err("Failed to write CURR_IN_OFFSET, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("CURR_IN_OFFSET = 0x%x:0x%x\n",
		FG_REG_CURR_IN_OFFSET, sm->curr_offset);

	ret = fg_write_word(sm, FG_REG_CURR_IN_SLOPE, sm->curr_slope);
	if (ret < 0) {
		sm_err("Failed to write CURR_IN_SLOPE, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("CURR_IN_SLOPE = 0x%x:0x%x\n",
		FG_REG_CURR_IN_SLOPE, sm->curr_slope);

	/* BAT CAP write */
	ret = fg_write_word(sm, sm->regs[SM_FG_REG_BAT_CAP], sm->cap);
	if (ret < 0) {
		sm_err("Failed to write BAT_CAP, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("BAT_CAP = 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_BAT_CAP], sm->cap);

	/* MISC write */
	ret = fg_write_word(sm, sm->regs[SM_FG_REG_MISC], sm->misc);
	if (ret < 0) {
		sm_err("Failed to write REG_MISC, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("REG_MISC 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_MISC], sm->misc);

	/* TOPOFF SOC */
	ret = fg_write_word(sm, sm->regs[SM_FG_REG_TOPOFFSOC], sm->topoff_soc);
	if (ret < 0) {
		sm_err("Failed to write SM_FG_REG_TOPOFFSOC, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("SM_REG_TOPOFFSOC 0x%x:0x%x\n",
		sm->regs[SM_FG_REG_TOPOFFSOC], sm->topoff_soc);

	/* IOCV manual-mode */
	ret = fg_update_bits(sm, sm->regs[SM_FG_REG_CNTL],
			     ENABLE_IOCV_MAN_MODE,
			     sm->iocv_man_mode ? ENABLE_IOCV_MAN_MODE : 0);
	if (ret < 0) {
		sm_err("Failed to update CNTL (iocv_man_mode), ret=%d\n", ret);
		goto unlock;
	}
	sm_info("CNTL[0x%02x] iocv_man_mode=%d\n",
		sm->regs[SM_FG_REG_CNTL], sm->iocv_man_mode);

	/* Parameter Version [COMMON(0~255) | BATTERY(0~255)] */
	ret = fg_write_word(sm, FG_PARAM_VERION,
			    (sm->common_param_version << 8) | sm->battery_param_version);
	if (ret < 0) {
		sm_err("Failed to write FG_PARAM_VERION, ret=%d\n", ret);
		goto unlock;
	}

	/* T EX L ALARM */
	if (sm->t_l_alarm_ex == -EINVAL) {
		sm_err("t_l_alarm_ex is invalid\n");
	} else {
		data = (sm->t_l_alarm_ex) >> 1; /* NTC Value/2 */
		ret = fg_write_word(sm, FG_REG_SWADDR, 0x6A);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWADDR, ret=%d\n", ret);
			goto unlock;
		}
		ret = fg_write_word(sm, FG_REG_SWDATA, data);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWDATA (T_EX_L), ret=%d\n", ret);
			goto unlock;
		}
		sm_info("write to T_EX_L_ALARM = 0x%x\n", data);
	}

	/* T EX H ALARM */
	if (sm->t_h_alarm_ex == -EINVAL) {
		sm_err("t_h_alarm_ex is invalid\n");
	} else {
		data = (sm->t_h_alarm_ex) >> 1; /* NTC Value/2 */
		ret = fg_write_word(sm, FG_REG_SWADDR, 0x6B);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWADDR, ret=%d\n", ret);
			goto unlock;
		}
		ret = fg_write_word(sm, FG_REG_SWDATA, data);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWDATA (T_EX_H), ret=%d\n", ret);
			goto unlock;
		}
		sm_info("write to T_EX_H_ALARM = 0x%x\n", data);
	}

	if (sm->iocv_man_mode) {
		value = fg_calculate_iocv(sm);
		usleep_range(10000, 11000);
		ret = fg_write_word(sm, FG_REG_SWADDR, 0x75);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWADDR, ret=%d\n", ret);
			goto unlock;
		}
		ret = fg_write_word(sm, FG_REG_SWDATA, value);
		if (ret < 0) {
			sm_err("Failed to write FG_REG_SWDATA (IOCV), ret=%d\n", ret);
			goto unlock;
		}
		sm_info("IOCV_MAN = 0x%x\n", value);
	}
	usleep_range(20000, 21000);

	ret = fg_write_word(sm, sm->regs[SM_FG_REG_PARAM_CTRL],
			    (FG_PARAM_LOCK_CODE |
			    (sm->battery_table_num & 0x0003) << 6) |
			    (FG_TABLE_LEN - 1));
	if (ret < 0) {
		sm_err("Failed to write param_ctrl lock, ret=%d\n", ret);
		goto unlock;
	}
	sm_info("Param Lock\n");

	if (sm->en_temp_ex)
		msleep(300);
	else
		msleep(160);

	ok = true;
unlock:
	mutex_unlock(&sm->data_lock);
	return ok;
}

static int fg_get_device_id(struct sm_fg_chip *sm)
{
	int ret;
	u16 data;

	ret = fg_read_word(sm, sm->regs[SM_FG_REG_DEVICE_ID], &data);
	if (ret < 0) {
		sm_err("Failed to read DEVICE_ID, ret=%d\n", ret);
		return ret;
	}

	sm_info("revision_id: 0x%x\n", (data & 0x000f));
	sm_info("device_id: 0x%x\n", (data & 0x00f0) >> 4);

	return (int)data;
}

static bool fg_check_device_id(struct sm_fg_chip *sm)
{
	int id = fg_get_device_id(sm);

	if (id < 0)
		return false;

	return ((id & 0x00f0) >> 4) == 0x0001;
}

static bool fg_init(struct sm_fg_chip *sm)
{
	int ret;
	bool ok = false;

	if (atomic_read(&sm->init_running))
		return true;

	atomic_set(&sm->init_running, 1);

	ret = fg_get_device_id(sm);
	if (ret < 0) {
		sm_err("get DEVICE_ID field failed(%d)\n", ret);
		goto out;
	}

	if (!fg_check_reg_init_need(sm)) {
		sm_dbg("skip fg_reg_init (no init needed)\n");
		ok = true;
		goto out;
	}

	ret = fg_reset(sm);
	if (ret < 0) {
		sm_err("fg_reset failed (%d)\n", ret);
		goto out;
	}

	sm_info("performing fg_reg_init\n");
	if (!fg_reg_init(sm)) {
		sm_err("fg_reg_init failed\n");
		goto out;
	}

	ok = true;

out:
	atomic_set(&sm->init_running, 0);
	return ok;
}

#define PROPERTY_NAME_SIZE 128
static int fg_common_parse_dt(struct sm_fg_chip *sm)
{
	struct device *dev = &sm->client->dev;
	struct device_node *np = dev->of_node;
	int rc, count;

	if (!np) {
		sm_err("No common device tree node found\n");
		return -ENODEV;
	}

	/* EN TEMP EX/IN */
	sm->en_temp_ex = of_property_read_bool(np, "sm,en_temp_ex");
	sm_info("Temperature EX enabled = %d\n", sm->en_temp_ex);

	sm->en_temp_in = of_property_read_bool(np, "sm,en_temp_in");
	sm_info("Temperature IN enabled = %d\n", sm->en_temp_in);

	/* EN BATT DET */
	sm->en_batt_det = of_property_read_bool(np, "sm,en_batt_det");
	sm_info("Batt Det enabled = %d\n", sm->en_batt_det);

	/* MISC */
	rc = of_property_read_u32(np, "sm,misc", &sm->misc);
	if (rc < 0)
		sm->misc = 0x0800;

	/* IOCV MAN MODE */
	sm->iocv_man_mode = of_property_read_bool(np, "sm,iocv_man_mode");
	sm_info("IOCV_MAN_MODE = %d\n", sm->iocv_man_mode);

	/* Aging */
	rc = of_property_read_u32(np, "sm,aging_ctrl", &sm->aging_ctrl);
	if (rc < 0)
		sm->aging_ctrl = -EINVAL;

	/* decimal rate: expect pairs threshold,rate,threshold,rate,... */
	count = of_property_count_u32_elems(np, "sm,soc_decimal_rate");
	if (count <= 0) {
		sm_err("decimal_rate missing or empty, (count=%d)\n", count);
		sm->dec_rate_seq = NULL;
		sm->dec_rate_len = 0;
	} else if (count < 2 || (count & 1)) {
		sm_err("decimal_rate invalid element count, (%d)\n", count);
		sm->dec_rate_seq = NULL;
		sm->dec_rate_len = 0;
	} else {
		sm->dec_rate_seq = devm_kcalloc(dev, count,
						sizeof(u32), GFP_KERNEL);
		if (!sm->dec_rate_seq) {
			sm->dec_rate_len = 0;
		} else {
			rc = of_property_read_u32_array(np,
							"sm,soc_decimal_rate",
							sm->dec_rate_seq, count);
			if (rc) {
				sm_err("read decimal_rate failed: (%d)\n", rc);
				sm->dec_rate_seq = NULL;
				sm->dec_rate_len = 0;
			} else {
				sm->dec_rate_len = count;
				sm_info("decimal_rate = %d\n",
					sm->dec_rate_len);
			}
		}
	}

	/* SOC Cycle cfg */
	rc = of_property_read_u32(np, "sm,cycle_cfg",
				  &sm->cycle_cfg);
	if (rc < 0)
		sm->cycle_cfg = -EINVAL;

	/* RSNS */
	rc = of_property_read_u32(np, "sm,rsns",
				  &sm->batt_rsns);
	if (rc < 0)
		sm->batt_rsns = -EINVAL;

	/* IRQ Mask */
	rc = of_property_read_u32(np, "sm,fg_irq_set",
				  &sm->fg_irq_set);
	if (rc < 0)
		sm->fg_irq_set = -EINVAL;

	/* LOW SOC1/2 */
	rc = of_property_read_u32(np, "sm,low_soc1",
				  &sm->low_soc1);
	if (rc < 0)
		sm->low_soc1 = -EINVAL;
	sm_info("low_soc1 = %d\n", sm->low_soc1);

	rc = of_property_read_u32(np, "sm,low_soc2",
				  &sm->low_soc2);
	if (rc < 0)
		sm->low_soc2 = -EINVAL;
	sm_info("low_soc2 = %d\n", sm->low_soc2);

	/* V_L/H_ALARM */
	rc = of_property_read_u32(np, "sm,v_l_alarm",
				  &sm->v_l_alarm);
	if (rc < 0)
		sm->v_l_alarm = -EINVAL;
	sm_info("v_l_alarm = %d\n", sm->v_l_alarm);

	rc = of_property_read_u32(np, "sm,v_h_alarm",
				  &sm->v_h_alarm);
	if (rc < 0)
		sm->v_h_alarm = -EINVAL;
	sm_info("v_h_alarm = %d\n", sm->v_h_alarm);

	/* T_IN_H/L_ALARM */
	rc = of_property_read_u32(np, "sm,t_l_alarm_in",
				  &sm->t_l_alarm_in);
	if (rc < 0)
		sm->t_l_alarm_in = -EINVAL;
	sm_info("t_l_alarm_in = %d\n", sm->t_l_alarm_in);

	rc = of_property_read_u32(np, "sm,t_h_alarm_in",
				  &sm->t_h_alarm_in);
	if (rc < 0)
		sm->t_h_alarm_in = -EINVAL;
	sm_info("t_h_alarm_in = %d\n", sm->t_h_alarm_in);

	/* T_EX_H/L_ALARM */
	rc = of_property_read_u32(np, "sm,t_l_alarm_ex",
				  &sm->t_l_alarm_ex);
	if (rc < 0)
		sm->t_l_alarm_ex = -EINVAL;
	sm_info("t_l_alarm_ex = %d\n", sm->t_l_alarm_ex);

	rc = of_property_read_u32(np, "sm,t_h_alarm_ex",
				  &sm->t_h_alarm_ex);
	if (rc < 0)
		sm->t_h_alarm_ex = -EINVAL;
	sm_info("t_h_alarm_ex = %d\n", sm->t_h_alarm_ex);

	/* Battery Table Number */
	rc = of_property_read_u32(np, "sm,battery_table_num",
				  &sm->battery_table_num);
	if (rc < 0)
		sm->battery_table_num = -EINVAL;

	/* Parameter Number */
	rc = of_property_read_u32(np, "sm,param_version",
				  &sm->common_param_version);
	if (rc < 0)
		sm->common_param_version = -EINVAL;

	/* Shutdown feature */
	sm->shutdown_delay_enable = of_property_read_bool(np,
							  "sm,shutdown-delay-enable");

#ifdef ENABLE_NTC_COMPENSATION
	/* Rtrace */
	rc = of_property_read_u32(np, "sm,rtrace", &sm->rtrace);
	if (rc < 0)
		sm->rtrace = 0;
#endif

	return 0;
}

static int fg_battery_parse_dt(struct sm_fg_chip *sm)
{
	struct device *dev = &sm->client->dev;
	struct device_node *root = dev->of_node;
	struct device_node *bp = NULL;
	char prop_name[PROPERTY_NAME_SIZE];
	int battery_id = BATTERY_VENDOR_UNKNOWN;
	int battery_temp_table[FG_TEMP_TABLE_CNT_MAX] = {0};
	int table[FG_TABLE_LEN] = {0};
	int rs_value[4] = {0};
	int topoff_soc[3] = {0};
	int temp_offset[6] = {0};
	int temp_cal[10] = {0};
	int ext_temp_cal[10] = {0};
	int battery_type[3] = {0};
	int set_tempoff[2] = {0};
	int ret = 0;
	int i, j;

	if (!root) {
		sm_err("No battery device tree node found\n");
		return -ENODEV;
	}

	bp = of_get_child_by_name(root, "battery_params");
	if (!bp) {
		sm_err("couldn't find child node \"battery_params\"\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(bp, "battery,id", &battery_id);
	if (ret < 0)
		sm_dbg("no battery_id property, get from authen\n");

	/* get battery_id from batt_verify */
	for (i = 0; i < 3; i++) {
		battery_id = get_battery_id(sm);

		if (battery_id != BATTERY_VENDOR_UNKNOWN)
			break;

		sm_dbg("battery_id unknown (%d), retrying...\n", i + 1);
		usleep_range(5000, 6000);
	}

	if (battery_id == BATTERY_VENDOR_UNKNOWN) {
		sm_err("battery id still unknown, use fallback\n");
		battery_id = 0;
	} else {
		sm_info("battery id = %d\n", battery_id);
	}

	/* battery_table 0..1 (FG_TABLE_LEN) */
	for (i = BATTERY_TABLE0; i < BATTERY_TABLE2; i++) {
		snprintf(prop_name, PROPERTY_NAME_SIZE,
			 "battery%d,%s%d", battery_id, "battery_table", i);
		ret = of_property_read_u32_array(bp, prop_name, table, FG_TABLE_LEN);
		if (ret < 0) {
			sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
			continue;
		}
		for (j = 0; j < FG_TABLE_LEN; j++)
			sm->battery_table[i][j] = table[j];
	}

	/* battery_table2 (FG_ADD_TABLE_LEN) */
	i = BATTERY_TABLE2;
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s%d", battery_id, "battery_table", i);
	ret = of_property_read_u32_array(bp, prop_name, table, FG_ADD_TABLE_LEN);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		for (j = 0; j < FG_ADD_TABLE_LEN; j++)
			sm->battery_table[i][j] = table[j];
	}

	/* rs */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "rs");
	ret = of_property_read_u32_array(bp, prop_name, &sm->rs, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->rs);

	/* alpha */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "alpha");
	ret = of_property_read_u32_array(bp, prop_name, &sm->alpha, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->alpha);

	/* beta */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "beta");
	ret = of_property_read_u32_array(bp, prop_name, &sm->beta, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->beta);

	/* rs_value (4 elemen) */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "rs_value");
	ret = of_property_read_u32_array(bp, prop_name, rs_value, 4);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		for (i = 0; i < 4; i++)
			sm->rs_value[i] = rs_value[i];
		sm_info("%s = <0x%x 0x%x 0x%x 0x%x>\n",
			prop_name, rs_value[0], rs_value[1], rs_value[2], rs_value[3]);
	}

	/* vit_period */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "vit_period");
	ret = of_property_read_u32_array(bp, prop_name, &sm->vit_period, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->vit_period);

	/* battery_type */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "battery_type");
	ret = of_property_read_u32_array(bp, prop_name, battery_type, 3);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->batt_v_max = battery_type[0];
		sm->min_cap = battery_type[1];
		sm->cap = battery_type[2];
		sm_info("%s = <%d %d %d>\n", prop_name,
			sm->batt_v_max, sm->min_cap, sm->cap);
	}

	/* tempoff level */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "tempoff");
	ret = of_property_read_u32_array(bp, prop_name, set_tempoff, 2);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->n_tempoff = set_tempoff[0];
		sm->n_tempoff_offset = set_tempoff[1];
		sm_info("%s = <%d %d>\n",
			prop_name, sm->n_tempoff, sm->n_tempoff_offset);
	}

	/* max-voltage */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "max_voltage_uv");
	ret = of_property_read_u32(bp, prop_name, &sm->batt_max_voltage_uv);
	if (ret < 0)
		sm_err("couldn't find battery max voltage\n");

	/* TOPOFF SOC */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "topoff_soc");
	ret = of_property_read_u32_array(bp, prop_name, topoff_soc, 3);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->topoff_soc = topoff_soc[0];
		sm->topoff = topoff_soc[1];
		sm->topoff_margin = topoff_soc[2];
		sm_info("%s = <%d %d %d>\n",
			prop_name, sm->topoff_soc, sm->topoff, sm->topoff_margin);
	}

	/* mix_value */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "mix_value");
	ret = of_property_read_u32_array(bp, prop_name, &sm->mix_value, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <%d>\n", prop_name, sm->mix_value);

	/* volt_cal */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "volt_cal");
	ret = of_property_read_u32_array(bp, prop_name, &sm->volt_cal, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->volt_cal);

	/* curr_offset */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "curr_offset");
	ret = of_property_read_u32_array(bp, prop_name, &sm->curr_offset, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->curr_offset);

	/* curr_slope */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "curr_slope");
	ret = of_property_read_u32_array(bp, prop_name, &sm->curr_slope, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->curr_slope);

	/* temp_std */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "temp_std");
	ret = of_property_read_u32_array(bp, prop_name, &sm->temp_std, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <%d>\n", prop_name, sm->temp_std);

	/* temp_offset (6) */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "temp_offset");
	ret = of_property_read_u32_array(bp, prop_name, temp_offset, 6);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->en_high_fg_temp_offset = temp_offset[0];
		sm->high_fg_temp_offset_denom = temp_offset[1];
		sm->high_fg_temp_offset_fact = temp_offset[2];
		sm->en_low_fg_temp_offset = temp_offset[3];
		sm->low_fg_temp_offset_denom = temp_offset[4];
		sm->low_fg_temp_offset_fact = temp_offset[5];
		sm_info("%s = <%d, %d, %d, %d, %d, %d>\n", prop_name,
			sm->en_high_fg_temp_offset,
			sm->high_fg_temp_offset_denom, sm->high_fg_temp_offset_fact,
			sm->en_low_fg_temp_offset,
			sm->low_fg_temp_offset_denom, sm->low_fg_temp_offset_fact);
	}

	/* temp_cal (10) */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "temp_cal");
	ret = of_property_read_u32_array(bp, prop_name, temp_cal, 10);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->en_high_fg_temp_cal = temp_cal[0];
		sm->high_fg_temp_p_cal_denom = temp_cal[1];
		sm->high_fg_temp_p_cal_fact = temp_cal[2];
		sm->high_fg_temp_n_cal_denom = temp_cal[3];
		sm->high_fg_temp_n_cal_fact = temp_cal[4];
		sm->en_low_fg_temp_cal = temp_cal[5];
		sm->low_fg_temp_p_cal_denom = temp_cal[6];
		sm->low_fg_temp_p_cal_fact = temp_cal[7];
		sm->low_fg_temp_n_cal_denom = temp_cal[8];
		sm->low_fg_temp_n_cal_fact = temp_cal[9];
		sm_info("%s = <%d, %d, %d, %d, %d, %d, %d, %d, %d, %d>\n", prop_name,
			sm->en_high_fg_temp_cal,
			sm->high_fg_temp_p_cal_denom, sm->high_fg_temp_p_cal_fact,
			sm->high_fg_temp_n_cal_denom, sm->high_fg_temp_n_cal_fact,
			sm->en_low_fg_temp_cal,
			sm->low_fg_temp_p_cal_denom, sm->low_fg_temp_p_cal_fact,
			sm->low_fg_temp_n_cal_denom, sm->low_fg_temp_n_cal_fact);
	}

	/* ext_temp_cal (10) */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "ext_temp_cal");
	ret = of_property_read_u32_array(bp, prop_name, ext_temp_cal, 10);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		sm->en_high_temp_cal = ext_temp_cal[0];
		sm->high_temp_p_cal_denom = ext_temp_cal[1];
		sm->high_temp_p_cal_fact = ext_temp_cal[2];
		sm->high_temp_n_cal_denom = ext_temp_cal[3];
		sm->high_temp_n_cal_fact = ext_temp_cal[4];
		sm->en_low_temp_cal = ext_temp_cal[5];
		sm->low_temp_p_cal_denom = ext_temp_cal[6];
		sm->low_temp_p_cal_fact = ext_temp_cal[7];
		sm->low_temp_n_cal_denom = ext_temp_cal[8];
		sm->low_temp_n_cal_fact = ext_temp_cal[9];
		sm_info("%s = <%d, %d, %d, %d, %d, %d, %d, %d, %d, %d>\n", prop_name,
			sm->en_high_temp_cal,
			sm->high_temp_p_cal_denom, sm->high_temp_p_cal_fact,
			sm->high_temp_n_cal_denom, sm->high_temp_n_cal_fact,
			sm->en_low_temp_cal,
			sm->low_temp_p_cal_denom, sm->low_temp_p_cal_fact,
			sm->low_temp_n_cal_denom, sm->low_temp_n_cal_fact);
	}

	/* fcm_offset */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "fcm_offset");
	ret = of_property_read_u32_array(bp, prop_name, &sm->fcm_offset, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->fcm_offset);

	/* thermal_table */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "thermal_table");
	ret = of_property_read_u32_array(bp, prop_name,
					 battery_temp_table, FG_TEMP_TABLE_CNT_MAX);
	if (ret < 0) {
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	} else {
		for (i = 0; i < FG_TEMP_TABLE_CNT_MAX; i++)
			sm->battery_temp_table[i] = (s16)battery_temp_table[i];
	}

	/* param_version */
	snprintf(prop_name, PROPERTY_NAME_SIZE,
		 "battery%d,%s", battery_id, "param_version");
	ret = of_property_read_u32_array(bp, prop_name, &sm->battery_param_version, 1);
	if (ret < 0)
		sm_err("couldn't get prop %s (%d)\n", prop_name, ret);
	else
		sm_info("%s = <0x%x>\n", prop_name, sm->battery_param_version);

	/* release child node ref */
	of_node_put(bp);
	return 0;
}

static bool hal_fg_init(struct sm_fg_chip *sm)
{
	int rc;
	bool ok;

	sm_info("start\n");
	if (sm->client->dev.of_node) {
		rc = fg_common_parse_dt(sm);
		if (rc) {
			sm_err("fg_common_parse_dt failed: %d\n", rc);
			goto out;
		}

		rc = fg_battery_parse_dt(sm);
		if (rc) {
			sm_err("fg_battery_parse_dt failed: %d\n", rc);
			goto out;
		}
	}

	ok = fg_init(sm);
	if (!ok) {
		sm_err("fg_init failed\n");
		goto out;
	}

	sm_info("hal_init done\n");
	return true;
out:
	sm_info("hal_init failed\n");
	return false;
}

static int sm5602_get_psy(struct sm_fg_chip *sm)
{
	if (sm->usb_psy && sm->batt_psy)
		return 0;

	sm->usb_psy = power_supply_get_by_name("usb");
	if (!sm->usb_psy) {
		sm_err("usb psy not found, force probe\n");
		return -EINVAL;
	}

	sm->batt_psy = power_supply_get_by_name("battery");
	if (!sm->batt_psy) {
		sm_err("batt psy not found, force probe\n");
		return -EINVAL;
	}

	return 0;
}

static void sm5602_put_psy(struct sm_fg_chip *sm)
{
	if (sm->batt_psy)
		power_supply_put(sm->batt_psy);
	if (sm->usb_psy)
		power_supply_put(sm->usb_psy);
	if (sm->cp_psy)
		power_supply_put(sm->cp_psy);
	if (sm->bbc_psy)
		power_supply_put(sm->bbc_psy);
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	if (sm->verify_psy)
		power_supply_put(sm->verify_psy);
#endif
}

//20220108 : W/A for over 60degree
static void overtemp_delay_workfunc(struct work_struct *work)
{
	struct sm_fg_chip *sm = container_of(work, struct sm_fg_chip, overtemp_delay_work.work);

	sm->overtemp_allow_restart = true;
}

/*if rawsoc less than 1% and vbat less than 3.4V then force UI_SOC update to 0%.*/
static void low_battery_check_workfunc(struct work_struct *work)
{
	struct sm_fg_chip *sm = container_of(work, struct sm_fg_chip, low_battery_check_work.work);
	int i, low_soc_count = 0;

	for (i = 0; i < 3; i++) {
		fg_read_volt(sm);
		fg_read_soc(sm);

		if (sm->batt_soc < 10 && sm->batt_volt < 3400)
			low_soc_count++;

		if (i < 2)
			msleep(1000);
	}

	if (low_soc_count == 3) {
		sm->low_battery_power = true;
		mutex_lock(&sm->data_lock);
		sm->param.batt_soc = 0;
		sm->batt_soc = 0;
		mutex_unlock(&sm->data_lock);
	} else {
		sm->low_battery_power = false;
	}

	sm->start_low_battery_check = false;
}

static int sm5602_notifier_call(struct notifier_block *nb,
				unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct sm_fg_chip *sm = container_of(nb, struct sm_fg_chip, nb);
	union power_supply_propval pval = {0, };
	bool prev_present;
	int rc;

	if (ev != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_DONE;

	rc = sm5602_get_psy(sm);
	if (rc < 0)
		return NOTIFY_DONE;

	if (psy != sm->usb_psy)
		return NOTIFY_DONE;

	rc = power_supply_get_property(sm->usb_psy,
				       POWER_SUPPLY_PROP_PRESENT, &pval);
	if (rc < 0) {
		sm_err("failed get usb present\n");
		return NOTIFY_DONE;
	}

	prev_present = sm->usb_present;
	sm->usb_present = !!pval.intval;

	if (sm->usb_present && !prev_present) {
		sm_info("USB connected\n");
	} else if (!sm->usb_present && prev_present) {
		sm->batt_sw_fc = false;
		sm_info("USB disconnected\n");
	}

	return NOTIFY_OK;
}

static int sm_fg_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	int i, ret;
	struct sm_fg_chip *sm;
	const u8 *regs;

	sm_info("sm fuel gauge probe enter\n");
	sm = devm_kzalloc(&client->dev, sizeof(*sm), GFP_KERNEL);
	if (!sm) {
		sm_err("out of memory!!\n");
		return -ENOMEM;
	}

	sm->dev = &client->dev;
	sm->client = client;
	i2c_set_clientdata(client, sm);

	sm->regmap = devm_regmap_init_i2c(sm->client, &sm5602_regmap_config);
	if (IS_ERR(sm->regmap)) {
		ret = PTR_ERR(sm->regmap);
		sm_err("failed to init regmap: %d\n", ret);
		return ret;
	}

	sm->chip = id->driver_data;
	if (sm->chip == SM5602) {
		regs = sm5602_regs;
	} else {
		sm_err("unexpected fuel gauge: %d\n", sm->chip);
		regs = sm5602_regs;
	}

	memcpy(sm->regs, regs, ARRAY_SIZE(sm->regs) * sizeof(sm->regs[0]));
	mutex_init(&sm->data_lock);

#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	sm->verify_psy = power_supply_get_by_name("batt_verify");
	if (!sm->verify_psy) {
		sm_err("batt_verify psy not ready, defer probe\n");
		ret = -EPROBE_DEFER;
		goto err_free;
	}
#endif

	sm->bbc_psy = power_supply_get_by_name("bbc");
	if (!sm->bbc_psy) {
		sm_err("bbc_psy not ready, defer probe\n");
		ret = -EPROBE_DEFER;
		goto err_init;
	}

	sm->cp_psy = power_supply_get_by_name("sc8551-standalone");
	if (!sm->cp_psy) {
		sm_err("cp_psy not ready, defer probe\n");
		ret = -EPROBE_DEFER;
		goto err_init;
	}

	sm->batt_soc	= -ENODATA;
	sm->batt_fcc	= -ENODATA;
	//sm->batt_dc	= -ENODATA;
	sm->batt_volt	= -ENODATA;
	sm->batt_temp	= -ENODATA;
	sm->batt_curr	= -ENODATA;
	sm->fake_soc	= -EINVAL;
	sm->fake_temp	= -EINVAL;
	sm->param.batt_raw_soc	= -EINVAL;
	sm->param.batt_soc	= -EINVAL;
#ifdef ENABLE_CURRENT_AVG
	sm->param.batt_ma = -EINVAL;
	sm->param.batt_ma_prev = -EINVAL;
	sm->param.batt_ma_avg = -EINVAL;
#endif
#ifdef ENABLE_TEMP_AVG
	sm->param.batt_temp = -EINVAL;
	sm->param.batt_temp_prev = -EINVAL;
	sm->param.batt_temp_avg = -EINVAL;
#endif
	//20220108 : W/A for over 60degree
	sm->overtemp_delay_on = false;
	sm->overtemp_allow_restart = false;
	sm->low_battery_power = false;
	sm->start_low_battery_check = false;
	sm->soc_smooth_initialized = false;
	sm->cp_work_flag = false;
	sm->fast_mode = false;
	atomic_set(&sm->init_running, 0);

	for (i = 0; i < SM_RL_SLOTS; i++) {
		ratelimit_state_init(&sm->rl_slots[i], SM_RL_INTERVAL, SM_RL_BURST);
		sm->rl_slots[i].flags |= RATELIMIT_MSG_ON_RELEASE;
	}

	if (!fg_check_device_id(sm)) {
		sm_err("device ID check failed\n");
		ret = -ENODEV;
		goto err_init;
	}

	if (!hal_fg_init(sm)) {
		sm_err("Failed to Initialize Fuelgauge\n");
		ret = -EIO;
		goto err_init;
	}

	ret = fg_psy_register(sm);
	if (ret) {
		sm_err("fg_psy_register failed!!!\n");
		goto err_init;
	}

	kref_init(&sm->kref);
	INIT_LIST_HEAD(&sm->node);
	strscpy(sm->name, "sm-bms", sizeof(sm->name));

	INIT_DELAYED_WORK(&sm->monitor_work, fg_monitor_workfunc);
	INIT_DELAYED_WORK(&sm->overtemp_delay_work, overtemp_delay_workfunc);
	INIT_DELAYED_WORK(&sm->low_battery_check_work, low_battery_check_workfunc);

	sm->main_wq = alloc_workqueue("sm5602_main_wq",
				      WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!sm->main_wq) {
		sm_err("failed to creat main_wq\n");
		ret = -ENOMEM;
		goto err_wq;
	}

	sm->aux_wq = alloc_workqueue("sm5602_aux_wq", WQ_MEM_RECLAIM, 1);
	if (!sm->aux_wq) {
		sm_err("failed to creat aux_wq\n");
		ret = -ENOMEM;
		goto err_wq;
	}
	sm_info("overtemp_delay_on: %d\n", sm->overtemp_delay_on);

	// find votable
	sm->fv_votable = find_votable("FV");
	sm->chg_dis_votable = find_votable("CHG_DISABLE");

	ret = sysfs_create_group(&sm->dev->kobj, &fg_attr_group);
	if (ret) {
		sm_err("Failed to register sysfs: %d\n", ret);
		goto err_reg;
	}

	create_debugfs_entry(sm);

	sm->nb.notifier_call = sm5602_notifier_call;
	ret = power_supply_reg_notifier(&sm->nb);
	if (ret < 0) {
		sm_err("Couldn't register psy notifier, rc=%d\n", ret);
		goto err_sysfs;
	}

	mutex_lock(&sm_list_lock);
	list_add_tail(&sm->node, &sm_list);
	mutex_unlock(&sm_list_lock);

	queue_delayed_work(sm->main_wq, &sm->monitor_work, 10 * HZ);
	sm_info("sm fuel gauge probe successfully, %s\n", device2str[sm->chip]);

	return 0;

err_sysfs:
	if (!IS_ERR_OR_NULL(sm->debug_root))
		debugfs_remove_recursive(sm->debug_root);
	sysfs_remove_group(&sm->dev->kobj, &fg_attr_group);
err_reg:
	fg_psy_unregister(sm);
err_wq:
	if (sm->aux_wq) {
		destroy_workqueue(sm->aux_wq);
		sm->aux_wq = NULL;
	}
	if (sm->main_wq) {
		destroy_workqueue(sm->main_wq);
		sm->main_wq = NULL;
	}
err_init:
	sm5602_put_psy(sm);
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
err_free:
#endif
	mutex_destroy(&sm->data_lock);
	i2c_set_clientdata(client, NULL);

	sm_err("sm fuel gauge probe failed\n");
	return ret;
}

static int sm_fg_remove(struct i2c_client *client)
{
	struct sm_fg_chip *sm = i2c_get_clientdata(client);

	if (!sm)
		return 0;

	power_supply_unreg_notifier(&sm->nb);

	cancel_delayed_work_sync(&sm->monitor_work);
	cancel_delayed_work_sync(&sm->overtemp_delay_work);
	cancel_delayed_work_sync(&sm->low_battery_check_work);

	if (sm->aux_wq) {
		destroy_workqueue(sm->aux_wq);
		sm->aux_wq = NULL;
	}
	if (sm->main_wq) {
		destroy_workqueue(sm->main_wq);
		sm->main_wq = NULL;
	}

	if (!IS_ERR_OR_NULL(sm->debug_root))
		debugfs_remove_recursive(sm->debug_root);

	sysfs_remove_group(&sm->dev->kobj, &fg_attr_group);

	mutex_lock(&sm_list_lock);
	list_del_init(&sm->node);
	mutex_unlock(&sm_list_lock);
	fg_put_sm(sm);

	fg_psy_unregister(sm);
	sm5602_put_psy(sm);

	mutex_destroy(&sm->data_lock);
	i2c_set_clientdata(client, NULL);

	return 0;
}

static void sm_fg_shutdown(struct i2c_client *client)
{
	sm_info("sm fuel gauge driver shutdown!\n");
}

static const struct of_device_id sm_fg_match_table[] = {
	{ .compatible = "sm,sm5602", },
	{ },
};
MODULE_DEVICE_TABLE(of, sm_fg_match_table);

static const struct i2c_device_id sm_fg_id[] = {
	{ "sm5602", SM5602 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm_fg_id);

static struct i2c_driver sm_fg_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "sm5602",
		.of_match_table	= of_match_ptr(sm_fg_match_table),
	},
	.id_table	= sm_fg_id,
	.probe		= sm_fg_probe,
	.remove	= sm_fg_remove,
	.shutdown	= sm_fg_shutdown,
};

module_i2c_driver(sm_fg_driver);

MODULE_DESCRIPTION("SM SM5602 Gauge Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Siliconmitus");
