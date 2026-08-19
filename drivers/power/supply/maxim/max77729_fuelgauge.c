/*
 * max77729_fuelgauge.c
 * Copyrights (C) 2021 Maxim Integrated Products, Inc.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt)	"[max77729_fg]: %s: " fmt, __func__

#define DEBUG
/* #define BATTERY_LOG_MESSAGE */

#include <linux/kernel.h>
#include <linux/mfd/max77729-private.h>
#include <linux/of_gpio.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/power_supply.h>
#include "max77729_fuelgauge.h"

/* extern unsigned int lpcharge; */

static enum power_supply_property max77729_fuelgauge_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_RESISTANCE,
	POWER_SUPPLY_PROP_RESISTANCE_ID,
	POWER_SUPPLY_PROP_FASTCHARGE_MODE,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_SOH,
	POWER_SUPPLY_PROP_SOC_DECIMAL,
	POWER_SUPPLY_PROP_SOC_DECIMAL_RATE,
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	POWER_SUPPLY_PROP_AUTHENTIC,
	POWER_SUPPLY_PROP_ROMID,
	POWER_SUPPLY_PROP_DS_STATUS,
	POWER_SUPPLY_PROP_PAGE0_DATA,
	POWER_SUPPLY_PROP_CHIP_OK,
#endif
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_BATTERY_TYPE,
	POWER_SUPPLY_PROP_SHUTDOWN_DELAY,
};

int max77729_fuelgauge_prop_is_writeable(struct power_supply *psy,
			enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
	case POWER_SUPPLY_PROP_TEMP:
		return 1;
	default:
		break;
	}
	return 0;
}

bool max77729_fg_fuelalert_init(struct max77729_fuelgauge_data *fuelgauge,
			int soc, int volt);
static void max77729_fg_periodic_read_power(struct max77729_fuelgauge_data *fuelgauge);
static u8 fgauge_get_battery_id(void);

static struct device_attribute max77729_fg_attrs[] = {
	MAX77729_FG_ATTR(fg_data),
};

static void max77729_fg_model_load(struct max77729_fuelgauge_data *fg)
{
	int i = 0, retry = 0;
	struct verify_reg *fg_verify = fg->verify_selected_reg;
	u16 reg_data;
	bool force_update = false;

	reg_data = max77729_read_word(fg->i2c, LEARN_CFG_REG);
	if ((reg_data & 0x0004) == 0) {
		reg_data = reg_data | 0x0004;
	}
	max77729_write_word(fg->i2c, LEARN_CFG_REG, reg_data);

	if (!fg_verify)
		return;

	if (max77729_read_word(fg->i2c, VEMPTY_REG) != fg->battery_data->V_empty)
		force_update = true;

	if (max77729_read_word(fg->i2c, DESIGNCAP_REG) != fg->battery_data->Capacity)
		force_update = true;

	if (fg->battery_data->battery_id == BATTERY_VENDOR_UNKNOWN)
		force_update = false;

	if (max77729_read_word(fg->i2c, STATUS_REG) & BIT(1) || force_update) {
		pr_info("fg reset\n");
		if (max77729_read_word(fg->i2c, FSTAT_REG) & BIT(0)) {
			msleep(10);
			max77729_read_word(fg->i2c, FSTAT_REG);
		}

		while (retry < 10 && i < FG_MODEL_DATA_COUNT) {
			max77729_write_word(fg->i2c, 0x62, 0x0059);
			max77729_write_word(fg->i2c, 0x63, 0x00C4);

			msleep(10);

			for (i = 0; i < FG_MODEL_DATA_COUNT; i++) {
				max77729_write_word(fg->i2c, 0x80 + i, fg->fg_model_data[i]);
			}

			for (i = 0; i < FG_MODEL_DATA_COUNT; i++) {
				if (max77729_read_word(fg->i2c, 0x80 + i) != fg->fg_model_data[i])
					break;
			}

			retry++;
			pr_info("fg verified data number is %d, data %x, retry %d\n", i, fg->fg_model_data[i], retry);
		}
		max77729_write_word(fg->i2c, 0x62, 0x0000);
		max77729_write_word(fg->i2c, 0x63, 0x0000);
		msleep(350);

		for (i = 0; i < fg->verify_selected_reg_length; i++) {
			max77729_write_word(fg->i2c, fg_verify[i].addr, fg_verify[i].data);
			if (max77729_read_word(fg->i2c, fg_verify[i].addr) != fg_verify[i].data) {
				pr_err("fg verified customer data error, reg %x, data %x\n", fg_verify[i].addr, fg_verify[i].data);
			}
		}

		reg_data = max77729_read_word(fg->i2c, 0xBB) | 0x0020;
		max77729_write_word(fg->i2c, 0xBB, reg_data);

		retry = 0;
		while (((max77729_read_word(fg->i2c, 0xBB) & 0x0020) == 0x0020) && retry < 10) {
			msleep(100);
			retry++;
		}

		pr_info("fg verified end, retry %d\n", retry);
		reg_data = max77729_read_word(fg->i2c, STATUS_REG) & 0xFFFD;
		max77729_write_word(fg->i2c, STATUS_REG, reg_data);
	} else {
		pr_err("fg alive\n");
	}

	max77729_write_word(fg->i2c, 0x60, 0x0080);
	max77729_write_word(fg->i2c, 0x49, 0x3141);
	max77729_write_word(fg->i2c, 0x60, 0x0000);
	max77729_write_word(fg->i2c, 0x2B, 0x38F0);
	max77729_write_word(fg->i2c, FILTER_CFG_REG, 0xCEA6);
}

#if !defined(CONFIG_SEC_FACTORY)
static void max77729_fg_adaptation_wa(struct max77729_fuelgauge_data *fuelgauge)
{
	u32 rcomp0;
	u32 fullcapnom;
	u32 temp;
	u8 data[2];
	struct fg_reset_wa *fg_reset_data = fuelgauge->fg_reset_data;

	if (!fg_reset_data)
		return;

	/* check RCOMP0 */
	rcomp0 = max77729_read_word(fuelgauge->i2c, RCOMP_REG);
	if ((rcomp0 > (fg_reset_data->rcomp0 * 14 / 10)) || (rcomp0 < (fg_reset_data->rcomp0 * 7 / 10))) {
		pr_err("abnormal RCOMP0 (0x%x / 0x%x)\n", rcomp0, fg_reset_data->rcomp0);
		goto set_default_value;
	}

	/* check TEMPCO */
	if (max77729_bulk_read(fuelgauge->i2c, TEMPCO_REG, 2, data) < 0) {
		pr_err("Failed to read TEMPCO\n");
		return;
	}

	/* tempcohot = data[1]; tempcocold = data[0]; */
	temp = (fg_reset_data->tempco & 0xFF00) >> 8;
	if ((data[1] > (temp * 14 / 10)) || (data[1] < (temp * 7 / 10))) {
		pr_err("abnormal TempCoHot (0x%x / 0x%x)\n", data[1], temp);
		goto set_default_value;
	}

	temp = fg_reset_data->tempco & 0x00FF;
	if ((data[0] > (temp * 14 / 10)) || (data[0] < (temp * 7 / 10))) {
		pr_err("abnormal TempCoCold (0x%x / 0x%x)\n", data[0], temp);
		goto set_default_value;
	}

	/* check FULLCAPNOM */
	fullcapnom = max77729_read_word(fuelgauge->i2c, FULLCAP_NOM_REG);
	temp = max77729_read_word(fuelgauge->i2c, DESIGNCAP_REG);
	if (fullcapnom > (temp * 11 / 10)) {
		pr_err("abnormal fullcapnom (0x%x / 0x%x)\n", fullcapnom, temp);
		goto re_calculation;
	}

	return;

set_default_value:
	pr_err("enter set_default_value\n");
	max77729_write_word(fuelgauge->i2c, RCOMP_REG, fg_reset_data->rcomp0);
	max77729_write_word(fuelgauge->i2c, TEMPCO_REG, fg_reset_data->tempco);
re_calculation:
	pr_err("enter re_calculation\n");
	max77729_write_word(fuelgauge->i2c, DPACC_REG, fg_reset_data->dPacc);
	max77729_write_word(fuelgauge->i2c, DQACC_REG, fg_reset_data->dQacc);
	max77729_write_word(fuelgauge->i2c, FULLCAP_NOM_REG, fg_reset_data->fullcapnom);
	temp = max77729_read_word(fuelgauge->i2c, LEARN_CFG_REG);
	temp &= 0xFF0F;
	max77729_write_word(fuelgauge->i2c, LEARN_CFG_REG, temp);
	max77729_write_word(fuelgauge->i2c, CYCLES_REG, 0);

	return;
}

static void max77729_fg_periodic_read(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 reg;
	int i, data[0x10];
	char *str = NULL;
	unsigned int v_cnt = 0; /* verify count */
	unsigned int err_cnt = 0;
	struct verify_reg *fg_verify = fuelgauge->verify_selected_reg;

	str = kzalloc(sizeof(char) * 1024, GFP_KERNEL);
	if (!str)
		return;

	for (i = 0; i < 16; i++) {
		if (i == 5)
			i = 11;
		else if (i == 12)
			i = 13;
		for (reg = 0; reg < 0x10; reg++) {
			data[reg] = max77729_read_word(fuelgauge->i2c, reg + i * 0x10);

			if (data[reg] < 0) {
				kfree(str);
				return;
			}

			/* verify fg reg */
			if (!fuelgauge->skip_fg_verify && fg_verify && v_cnt < fuelgauge->verify_selected_reg_length) {
				if (fg_verify[v_cnt].addr == reg + i * 0x10) {
					if (fg_verify[v_cnt].data != data[reg]) {
						pr_err("[%d] addr(0x%x 0x%x) data(0x%x 0x%x)\n",
								v_cnt,
								fg_verify[v_cnt].addr, reg + i * 0x10,
								fg_verify[v_cnt].data, data[reg]);
						err_cnt++;
					}
					v_cnt++;
				}
			}
		}
		sprintf(str + strlen(str),
			"%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,",
			data[0x00], data[0x01], data[0x02], data[0x03],
			data[0x04], data[0x05], data[0x06], data[0x07]);
		sprintf(str + strlen(str),
			"%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,%04xh,",
			data[0x08], data[0x09], data[0x0a], data[0x0b],
			data[0x0c], data[0x0d], data[0x0e], data[0x0f]);

		if (!fuelgauge->initial_update_of_soc)
			msleep(1);
	}

	err_cnt = 0; /* blocking panic test code */

	if (err_cnt > 0) {
		max77729_write_word(fuelgauge->i2c, DESIGNCAP_REG,
				fuelgauge->battery_data->Capacity + 5);
		panic("Find broken FG REG!!!");
	}
	err_cnt = 0;

	max77729_fg_adaptation_wa(fuelgauge);

	kfree(str);
}
#endif

static int max77729_fg_read_vcell(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 vcell, temp;
	u16 w_data;

	if (max77729_bulk_read(fuelgauge->i2c, VCELL_REG, 2, data) < 0) {
		pr_err("Failed to read VCELL_REG\n");
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	temp = (w_data & 0xFFF) * 78125;
	vcell = temp / 1000000;

	temp = ((w_data & 0xF000) >> 4) * 78125;
	temp /= 1000000;
	vcell += temp << 4;

	if (!(fuelgauge->info.pr_cnt++ % PRINT_COUNT)) {
		fuelgauge->info.pr_cnt = 1;
		/* pr_info("%s: VCELL(%d)mV, data(0x%04x)\n", */
			/* __func__, vcell, (data[1] << 8) | data[0]); */
	}
#if 0
	if ((fuelgauge->vempty_mode == VEMPTY_MODE_SW_VALERT) &&
			(vcell >= fuelgauge->battery_data->sw_v_empty_recover_vol)) {
		fuelgauge->vempty_mode = VEMPTY_MODE_SW_RECOVERY;
		max77729_fg_fuelalert_init(fuelgauge, fuelgauge->pdata->fuel_alert_soc);
		pr_info("Recoverd from SW V EMPTY Activation\n");
#if defined(CONFIG_BATTERY_CISD)
		if (fuelgauge->valert_count_flag) {
			pr_info("Vcell(%d) release CISD VALERT COUNT check\n", vcell);
			fuelgauge->valert_count_flag = false;
		}
#endif
	}
#endif

	return vcell;
}

static int max77729_fg_read_vfocv(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 vfocv = 0, temp;
	u16 w_data;

	if (max77729_bulk_read(fuelgauge->i2c, VFOCV_REG, 2, data) < 0) {
		pr_err("Failed to read VFOCV_REG\n");
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	temp = (w_data & 0xFFF) * 78125;
	vfocv = temp / 1000000;

	temp = ((w_data & 0xF000) >> 4) * 78125;
	temp /= 1000000;
	vfocv += (temp << 4);

#if !defined(CONFIG_SEC_FACTORY)
	/* max77729_fg_periodic_read(fuelgauge); */
#endif
	max77729_fg_periodic_read_power(fuelgauge);

	return vfocv;
}

static int max77729_fg_read_avg_vcell(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 avg_vcell = 0, temp;
	u16 w_data;

	if (max77729_bulk_read(fuelgauge->i2c, AVR_VCELL_REG, 2, data) < 0) {
		pr_err("Failed to read AVR_VCELL_REG\n");
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	temp = (w_data & 0xFFF) * 78125;
	avg_vcell = temp / 1000000;

	temp = ((w_data & 0xF000) >> 4) * 78125;
	temp /= 1000000;
	avg_vcell += (temp << 4);

	return avg_vcell;
}

static int max77729_fg_check_battery_present(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 status_data[2];
	int ret = 1;

	if (max77729_bulk_read(fuelgauge->i2c, STATUS_REG, 2, status_data) < 0) {
		pr_err("Failed to read STATUS_REG\n");
		return 0;
	}

	if (status_data[0] & (0x1 << 3)) {
		pr_info("addr(0x00), data(0x%04x)\n", (status_data[1] << 8) | status_data[0]);
		pr_info("battery is absent!!\n");
		ret = 0;
	}

	return ret;
}

static void max77729_fg_set_vempty(struct max77729_fuelgauge_data *fuelgauge, int vempty_mode)
{
	u16 data = 0;
	u8 valrt_data[2] = {0, };

	if (!fuelgauge->using_temp_compensation) {
		pr_info("does not use temp compensation, default hw vempty\n");
		vempty_mode = VEMPTY_MODE_HW;
	}

	fuelgauge->vempty_mode = vempty_mode;

	switch (vempty_mode) {
	case VEMPTY_MODE_SW:
		/* HW Vempty Disable */
		max77729_write_word(fuelgauge->i2c, VEMPTY_REG,
				fuelgauge->battery_data->V_empty_origin);
		/* Reset VALRT Threshold setting (enable) */
		valrt_data[1] = 0xFF;
		valrt_data[0] = fuelgauge->battery_data->sw_v_empty_vol / 20;
		if (max77729_bulk_write(fuelgauge->i2c, VALRT_THRESHOLD_REG, 2, valrt_data) < 0) {
			pr_err("Failed to write VALRT_THRESHOLD_REG\n");
			return;
		}
		data = max77729_read_word(fuelgauge->i2c, VALRT_THRESHOLD_REG);
		pr_info("HW V EMPTY Disable, SW V EMPTY Enable with %d mV (%d)\n",
			fuelgauge->battery_data->sw_v_empty_vol, (data & 0x00ff) * 20);
		break;
	default:
		/* HW Vempty Enable */
		max77729_write_word(fuelgauge->i2c, VEMPTY_REG,
				fuelgauge->battery_data->V_empty);
		/* Reset VALRT Threshold setting (disable) */
		valrt_data[1] = 0xFF;
		valrt_data[0] = fuelgauge->battery_data->sw_v_empty_vol_cisd / 20;
		if (max77729_bulk_write(fuelgauge->i2c, VALRT_THRESHOLD_REG, 2, valrt_data) < 0) {
			pr_err("Failed to write VALRT_THRESHOLD_REG\n");
			return;
		}
		data = max77729_read_word(fuelgauge->i2c, VALRT_THRESHOLD_REG);
		pr_info("HW V EMPTY Enable, SW V EMPTY Disable %d mV (%d)\n", 0, (data & 0x00ff) * 20);
		break;
	}

	return;
}

static int max77729_fg_write_temp(struct max77729_fuelgauge_data *fuelgauge, int temperature)
{
	u8 data[2];

	data[0] = (temperature % 10) * 1000 / 39;
	data[1] = temperature / 10;
	max77729_bulk_write(fuelgauge->i2c, TEMPERATURE_REG, 2, data);

	/* pr_debug("%s: temperature to (%d, 0x%02x%02x)\n", */
		/* __func__, temperature, data[1], data[0]); */

	fuelgauge->temperature = temperature;
	if (!fuelgauge->vempty_init_flag)
		fuelgauge->vempty_init_flag = true;

	return temperature;
}

static int max77729_fg_read_temp(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[4] = { 0, 0, 0, 0 };
	int temper = 0;
	static int tempRangeIdOld; // 3 ranges for QRtable: 1: >-5; 2: -5~-15; 3:<-15.
	int tempRangeIdNew = 0;

	if (max77729_fg_check_battery_present(fuelgauge)) {
		if (max77729_bulk_read(fuelgauge->i2c, TEMPERATURE_REG, 2, data) < 0) {
			pr_err("Failed to read TEMPERATURE_REG\n");
			return -1;
		}

		if (data[1] & (0x1 << 7)) {
			temper = ((~(data[1])) & 0xFF) + 1;
			temper *= (-1000);
			temper -= ((~((int)data[0])) + 1) * 39 / 10;
		} else {
			temper = data[1] & 0x7f;
			temper *= 1000;
			temper += data[0] * 39 / 10;
		}
	} else {
		temper = fuelgauge->temperature * 100;
	}

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_info("%s: TEMPERATURE(%d), data(0x%04x)\n", */
			/* __func__, temper, (data[1] << 8) | data[0]); */

	fuelgauge->temperature = temper / 100;

	//check 3 temperature ranges for QRtable: 1: >-5; 2: -5~-15; 3:<-15.
	if (fuelgauge->temperature > 0) {
		tempRangeIdNew = 1;
	} else if (fuelgauge->temperature <= 0 && fuelgauge->temperature > -100) {
		tempRangeIdNew = 2;
	} else if (fuelgauge->temperature <=  -100) {
		tempRangeIdNew = 3;
	}

	//update QRtable registers if temperature range is changed
	if (tempRangeIdNew != tempRangeIdOld) {
		switch (tempRangeIdNew) {
		case 1:
			 /* if (fuelgauge->battery_data->battery_id == 1) { */
			max77729_write_word(fuelgauge->i2c, 0x12, 0x5000);
			max77729_write_word(fuelgauge->i2c, 0x22, 0x2280);
			/* } else { */
				/* max77729_write_word(fuelgauge->i2c, 0x12, 0x3e00); */
				/* max77729_write_word(fuelgauge->i2c, 0x22, 0x1f80); */
			/* } */
			break;
		case 2:
			/* if (fuelgauge->battery_data->battery_id == 1) { */
			max77729_write_word(fuelgauge->i2c, 0x12, 0x5808);
			max77729_write_word(fuelgauge->i2c, 0x22, 0x2800);
			/* } else { */
				/* max77729_write_word(fuelgauge->i2c, 0x12, 0x4000); */
				/* max77729_write_word(fuelgauge->i2c, 0x22, 0x2000); */
			/* } */
			break;
		case 3:
			 /* if (fuelgauge->battery_data->battery_id == 1) { */
			max77729_write_word(fuelgauge->i2c, 0x12, 0x600f);
			max77729_write_word(fuelgauge->i2c, 0x22, 0x3000);
			/* } else { */
				/* max77729_write_word(fuelgauge->i2c, 0x12, 0x4609); */
				/* max77729_write_word(fuelgauge->i2c, 0x22, 0x2200); */
			/* } */
			break;
		default:
			break;
		}


		if (max77729_bulk_read(fuelgauge->i2c, 0x12, 2, data) < 0) {
			pr_err("Failed to read Reg 0x12\n");
			return -1;
		}
		if (max77729_bulk_read(fuelgauge->i2c, 0x22, 2, &data[2]) < 0) {
			pr_err("Failed to read Reg 0x22\n");
			return -1;
		}
		/* pr_err("%s: temperature %d, Reg 0x12: %04x, Reg 0x22: %04x\n", __func__, fuelgauge->temperature, (data[1] << 8) | data[0], (data[3] << 8) | data[2]); */
		tempRangeIdOld = tempRangeIdNew;
	}

	return fuelgauge->temperature;
}

static int max77729_fg_read_vfsoc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int soc;

	if (max77729_bulk_read(fuelgauge->i2c, VFSOC_REG, 2, data) < 0) {
		pr_err("Failed to read VFSOC_REG\n");
		return -1;
	}
	soc = ((data[1] * 100) + (data[0] * 100 / 256)) / 10;

	return min(soc, 1000);
}

static int max77729_fg_read_qh_vfsoc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int soc;

	if (max77729_bulk_read(fuelgauge->i2c, VFSOC_REG, 2, data) < 0) {
		pr_err("Failed to read VFSOC_REG\n");
		return -1;
	}
	soc = ((data[1] * 10000) + (data[0] * 10000 / 256)) / 10;

	return soc;
}

static int max77729_fg_read_qh(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 temp, sign;
	s32 qh;

	if (max77729_bulk_read(fuelgauge->i2c, QH_REG, 2, data) < 0) {
		pr_err("Failed to read QH_REG\n");
		return -1;
	}

	temp = ((data[1] << 8) | data[0]) & 0xFFFF;
	if (temp & (0x1 << 15)) {
		sign = NEGATIVE;
		temp = (~temp & 0xFFFF) + 1;
	} else {
		sign = POSITIVE;
	}

	qh = temp * 1000 * fuelgauge->fg_resistor / 2;

	if (sign)
		qh *= -1;

	/* pr_info("%s : QH(%d)\n", __func__, qh); */

	return qh;
}

static int max77729_fg_read_avsoc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int soc;

	if (max77729_bulk_read(fuelgauge->i2c, SOCAV_REG, 2, data) < 0) {
		pr_err("Failed to read SOCAV_REG\n");
		return -1;
	}
	soc = ((data[1] * 100) + (data[0] * 100 / 256)) / 10;

	return min(soc, 1000);
}

static int max77729_fg_read_soc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int soc;

	if (max77729_bulk_read(fuelgauge->i2c, SOCREP_REG, 2, data) < 0) {
		pr_err("Failed to read SOCREP_REG\n");
		return -1;
	}
	soc = ((data[1] * 100) + (data[0] * 100 / 256)) / 10;

#ifdef BATTERY_LOG_MESSAGE
	/* pr_debug("%s: raw capacity (%d)\n", __func__, soc); */

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) { */
		/* pr_debug("%s: raw capacity (%d), data(0x%04x)\n", */
			 /* __func__, soc, (data[1] << 8) | data[0]); */
		/* pr_debug("%s: REPSOC (%d), VFSOC (%d), data(0x%04x)\n", */
			 /* __func__, soc / 10, */
			 /* max77729_fg_read_vfsoc(fuelgauge) / 10, */
			 /* (data[1] << 8) | data[0]); */
	/* } */
#endif

	return min(soc, 1000);
}

/* soc should be 0.01% unit */
static int max77729_fg_read_rawsoc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int soc;

	if (max77729_bulk_read(fuelgauge->i2c, SOCREP_REG, 2, data) < 0) {
		pr_err("Failed to read SOCREP_REG\n");
		return -1;
	}
	soc = (data[1] * 100) + (data[0] * 100 / 256);

	/* pr_debug("%s: raw capacity (0.01%%) (%d)\n", __func__, soc); */

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_debug("%s: raw capacity (%d), data(0x%04x)\n", */
			 /* __func__, soc, (data[1] << 8) | data[0]); */

	return min(soc, 10000);
}

static int max77729_fg_read_fullcap(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, FULLCAP_REG, 2, data) < 0) {
		pr_err("Failed to read FULLCAP_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2 * 1000;
}

static int max77729_fg_read_fullcaprep(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, FULLCAP_REP_REG, 2, data) < 0) {
		pr_err("Failed to read FULLCAP_REP_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2;
}

static int max77729_fg_read_fullcapnom(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, FULLCAP_NOM_REG, 2, data) < 0) {
		pr_err("Failed to read FULLCAP_NOM_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2;
}

static int max77729_fg_read_mixcap(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, REMCAP_MIX_REG, 2, data) < 0) {
		pr_err("Failed to read REMCAP_MIX_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2;
}

static int max77729_fg_read_avcap(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, REMCAP_AV_REG, 2, data) < 0) {
		pr_err("Failed to read REMCAP_AV_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2;
}

static int max77729_fg_read_repcap(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, REMCAP_REP_REG, 2, data) < 0) {
		pr_err("Failed to read REMCAP_REP_REG\n");
		return -1;
	}
	ret = (data[1] << 8) + data[0];

	return ret * fuelgauge->fg_resistor / 2;
}

static int max77729_fg_read_current(struct max77729_fuelgauge_data *fuelgauge, int unit)
{
	u8 data1[2];
	u32 temp, sign;
	s32 i_current;

	if (max77729_bulk_read(fuelgauge->i2c, CURRENT_REG, 2, data1) < 0) {
		pr_err("Failed to read CURRENT_REG\n");
		return -1;
	}

	temp = ((data1[1] << 8) | data1[0]) & 0xFFFF;
	/* Debug log for abnormal current case */
	if (temp & (0x1 << 15)) {
		sign = NEGATIVE;
		temp = (~temp & 0xFFFF) + 1;
	} else {
		sign = POSITIVE;
	}

	switch (unit) {
	case SEC_BATTERY_CURRENT_UA:
		i_current = temp * 15625 * fuelgauge->fg_resistor / 100;
		break;
	case SEC_BATTERY_CURRENT_MA:
	default:
		i_current = temp * 15625 * fuelgauge->fg_resistor / 100000;
		break;
	}

	if (sign)
		i_current *= -1;

	/* pr_debug("%s: current=%d%s\n", __func__, i_current, */
		/* (unit == SEC_BATTERY_CURRENT_UA)? "uA" : "mA"); */

	return i_current;
}

static int max77729_fg_read_avg_current(struct max77729_fuelgauge_data *fuelgauge, int unit)
{
	static int cnt;
	u8 data2[2];
	u32 temp, sign;
	s32 avg_current;
	int vcell;

	if (max77729_bulk_read(fuelgauge->i2c, AVG_CURRENT_REG, 2, data2) < 0) {
		pr_err("Failed to read AVG_CURRENT_REG\n");
		return -1;
	}

	temp = ((data2[1] << 8) | data2[0]) & 0xFFFF;
	if (temp & (0x1 << 15)) {
		sign = NEGATIVE;
		temp = (~temp & 0xFFFF) + 1;
	} else {
		sign = POSITIVE;
	}

	switch (unit) {
	case SEC_BATTERY_CURRENT_UA:
		avg_current = temp * 15625 * fuelgauge->fg_resistor / 100;
		break;
	case SEC_BATTERY_CURRENT_MA:
	default:
		avg_current = temp * 15625 * fuelgauge->fg_resistor / 100000;
		break;
	}

	if (sign)
		avg_current *= -1;

	vcell = max77729_fg_read_vcell(fuelgauge);
	if ((vcell < 3500) && (cnt < 10) && (avg_current < 0) && fuelgauge->is_charging) {
		avg_current = 1;
		cnt++;
	}

	/* pr_debug("%s: avg_current=%d%s\n", __func__, avg_current, */
		/* (unit == SEC_BATTERY_CURRENT_UA)? "uA" : "mA"); */

	return avg_current;
}

static int max77729_fg_read_isys(struct max77729_fuelgauge_data *fuelgauge, int unit)
{
	u8 data1[2];
	u32 temp = 0;
	s32 i_current = 0;
	s32 inow = 0, inow_comp = 0;
	u32 unit_type = 0;

	if (max77729_bulk_read(fuelgauge->i2c, ISYS_REG, 2, data1) < 0) {
		pr_err("Failed to read ISYS_REG\n");
		return -1;
	}
	temp = ((data1[1] << 8) | data1[0]) & 0xFFFF;

	if (fuelgauge->fg_resistor != 2) {
		inow = max77729_fg_read_current(fuelgauge, unit);
	}

	if (unit == SEC_BATTERY_CURRENT_UA)
		unit_type = 1;
	else
		unit_type = 1000;

	i_current = temp * 3125 / 10 / unit_type;

	if (fuelgauge->fg_resistor != 2 && i_current != 0) {
		inow_comp = (int)((fuelgauge->fg_resistor - 2) * 10 / fuelgauge->fg_resistor) * inow / 10;
		i_current = i_current - inow_comp;
	}
	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_info("%s: isys_current=%d%s\n", __func__, i_current, */
			/* (unit == SEC_BATTERY_CURRENT_UA)? "uA" : "mA"); */

	return i_current;
}

static int max77729_fg_read_isys_avg(struct max77729_fuelgauge_data *fuelgauge, int unit)
{
	u8 data2[2];
	u32 temp = 0;
	s32 avg_current = 0;
	s32 avg_inow = 0, avg_inow_comp = 0;
	u32 unit_type = 0;

	if (max77729_bulk_read(fuelgauge->i2c, AVGISYS_REG, 2, data2) < 0) {
		pr_err("Failed to read AVGISYS_REG\n");
		return -1;
	}
	temp = ((data2[1] << 8) | data2[0]) & 0xFFFF;

	if (fuelgauge->fg_resistor != 2) {
		avg_inow = max77729_fg_read_avg_current(fuelgauge, unit);
	}

	if (unit == SEC_BATTERY_CURRENT_UA)
		unit_type = 1;
	else
		unit_type = 1000;

	avg_current = temp * 3125 / 10 / unit_type;

	if (fuelgauge->fg_resistor != 2 && avg_current != 0) {
		avg_inow_comp = (int)((fuelgauge->fg_resistor - 2) * 10 / fuelgauge->fg_resistor) * avg_inow / 10;
		avg_current = avg_current - avg_inow_comp;
	}
	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_info("%s: isys_avg_current=%d%s\n", __func__, avg_current, */
			/* (unit == SEC_BATTERY_CURRENT_UA)? "uA" : "mA"); */

	return avg_current;
}

static int max77729_fg_read_iin(struct max77729_fuelgauge_data *fuelgauge, int unit)
{
	u8 data1[2];
	u32 temp;
	s32 i_current;

	if (max77729_bulk_read(fuelgauge->i2c, IIN_REG, 2, data1) < 0) {
		pr_err("Failed to read IIN_REG\n");
		return -1;
	}

	temp = ((data1[1] << 8) | data1[0]) & 0xFFFF;

	switch (unit) {
	case SEC_BATTERY_CURRENT_UA:
		i_current = temp * 125;
		break;
	case SEC_BATTERY_CURRENT_MA:
	default:
		i_current = temp * 125 / 1000;
		break;
	}

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_debug("%s: iin_current=%d%s\n", __func__, i_current, */
			/* (unit == SEC_BATTERY_CURRENT_UA)? "uA" : "mA"); */

	return i_current;
}

static int max77729_fg_read_vbyp(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 vbyp, temp;
	u16 w_data;

	if (max77729_bulk_read(fuelgauge->i2c, VBYP_REG, 2, data) < 0) {
		pr_err("Failed to read VBYP_REG\n");
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	temp = (w_data & 0xFFF) * 427246;
	vbyp = temp / 1000000;

	temp = ((w_data & 0xF000) >> 4) * 427246;
	temp /= 1000000;
	vbyp += (temp << 4);

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_info("%s: VBYP(%d), data(0x%04x)\n", */
			/* __func__, vbyp, (data[1] << 8) | data[0]); */

	return vbyp;
}

static int max77729_fg_read_vsys(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 vsys, temp;
	u16 w_data;

	if (max77729_bulk_read(fuelgauge->i2c, VSYS_REG, 2, data) < 0) {
		pr_err("Failed to read VSYS_REG\n");
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	temp = (w_data & 0xFFF) * 15625;
	vsys = temp / 100000;

	temp = ((w_data & 0xF000) >> 4) * 15625;
	temp /= 100000;
	vsys += (temp << 4);

	/* if (!(fuelgauge->info.pr_cnt % PRINT_COUNT)) */
		/* pr_info("%s: VSYS(%d), data(0x%04x)\n", */
			/* __func__, vsys, (data[1] << 8) | data[0]); */

	return vsys;
}

static int max77729_fg_read_SoH(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, AGES_REG, 2, data) < 0) {
		pr_err("Failed to read AGES_REG\n");
		return -1;
	}

	ret = data[1];
	if (ret > 100) {
		ret = 100;
	}
	/* pr_err("%s: AGES_REG data(0x%04x), SoH return %d\n", */
		/* __func__, (data[1] << 8) | data[0], ret); */

	return ret;
}

static int max77729_fg_read_cycle(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int ret;

	if (max77729_bulk_read(fuelgauge->i2c, CYCLES_REG, 2, data) < 0) {
		pr_err("Failed to read CYCLES_REG\n");
		return -1;
	}
	ret = ((data[1] << 8) + data[0]) / 100;
	/* pr_err("%s: CYCLES_REG data(0x%04x), cycle_count return %d\n", */
		/* __func__, (data[1] << 8) | data[0], ret); */

	return ret;
}

static bool max77729_check_jig_status(struct max77729_fuelgauge_data *fuelgauge)
{
	bool ret = false;

	if (fuelgauge->pdata->jig_gpio) {
		if (fuelgauge->pdata->jig_low_active)
			ret = !gpio_get_value(fuelgauge->pdata->jig_gpio);
		else
			ret = gpio_get_value(fuelgauge->pdata->jig_gpio);
	}
	/* pr_info("%s: ret(%d)\n", __func__, ret); */

	return ret;
}

int max77729_fg_reset_soc(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	int vfocv, fullcap;

	msleep(500);

	pr_info("Before quick-start - VCELL(%d), VFOCV(%d), VfSOC(%d), RepSOC(%d)\n",
		max77729_fg_read_vcell(fuelgauge),
		max77729_fg_read_vfocv(fuelgauge),
		max77729_fg_read_vfsoc(fuelgauge),
		max77729_fg_read_soc(fuelgauge));
	pr_info("Before quick-start - current(%d), avg current(%d)\n",
		max77729_fg_read_current(fuelgauge, SEC_BATTERY_CURRENT_MA),
		max77729_fg_read_avg_current(fuelgauge, SEC_BATTERY_CURRENT_MA));

	if (!max77729_check_jig_status(fuelgauge)) {
		pr_info("Return by No JIG_ON signal\n");
		return 0;
	}

	max77729_write_word(fuelgauge->i2c, CYCLES_REG, 0);

	if (max77729_bulk_read(fuelgauge->i2c, MISCCFG_REG, 2, data) < 0) {
		pr_err("Failed to read MISCCFG_REG\n");
		return -1;
	}

	data[1] |= (0x1 << 2);
	if (max77729_bulk_write(fuelgauge->i2c, MISCCFG_REG, 2, data) < 0) {
		pr_info("Failed to write MISCCFG_REG\n");
		return -1;
	}

	msleep(250);
	max77729_write_word(fuelgauge->i2c, FULLCAP_REG,
			fuelgauge->battery_data->Capacity);
	max77729_write_word(fuelgauge->i2c, FULLCAP_REP_REG,
			fuelgauge->battery_data->Capacity);
	msleep(500);

	pr_info("After quick-start - VCELL(%d), VFOCV(%d), VfSOC(%d), RepSOC(%d)\n",
		max77729_fg_read_vcell(fuelgauge),
		max77729_fg_read_vfocv(fuelgauge),
		max77729_fg_read_vfsoc(fuelgauge),
		max77729_fg_read_soc(fuelgauge));
	pr_info("After quick-start - current(%d), avg current(%d)\n",
		max77729_fg_read_current(fuelgauge, SEC_BATTERY_CURRENT_MA),
		max77729_fg_read_avg_current(fuelgauge, SEC_BATTERY_CURRENT_MA));

	max77729_write_word(fuelgauge->i2c, CYCLES_REG, 0x00a0);

	vfocv = max77729_fg_read_vfocv(fuelgauge);
	if (vfocv < POWER_OFF_VOLTAGE_LOW_MARGIN) {
		pr_info("Power off condition(%d)\n", vfocv);
		fullcap = max77729_read_word(fuelgauge->i2c, FULLCAP_REG);

		max77729_write_word(fuelgauge->i2c, REMCAP_REP_REG,
				(u16)(fullcap * 9 / 1000));
		msleep(200);
		pr_info("new soc=%d, vfocv=%d\n",
			max77729_fg_read_soc(fuelgauge), vfocv);
	}

	pr_info("Additional step - VfOCV(%d), VfSOC(%d), RepSOC(%d)\n",
		max77729_fg_read_vfocv(fuelgauge),
		max77729_fg_read_vfsoc(fuelgauge),
		max77729_fg_read_soc(fuelgauge));

	return 0;
}

int max77729_fg_reset_capacity_by_jig_connection(struct max77729_fuelgauge_data *fuelgauge)
{
	union power_supply_propval val;

	/* val.intval = SEC_BAT_FGSRC_SWITCHING_VSYS; */
	/* psy_do_property("max77729-charger", set, */
			/* POWER_SUPPLY_EXT_PROP_FGSRC_SWITCHING, val); */

	val.intval = 1;
	psy_do_property("battery", set, POWER_SUPPLY_PROP_ENERGY_NOW, val);
	pr_info("DesignCap = Capacity - 1 (Jig Connection)\n");

	return max77729_write_word(fuelgauge->i2c, DESIGNCAP_REG, fuelgauge->battery_data->Capacity - 1);
}

static int max77729_fg_check_status_reg(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 status_data[2];
	int ret = 0;

	if (max77729_bulk_read(fuelgauge->i2c, STATUS_REG, 2, status_data) < 0) {
		pr_err("Failed to read STATUS_REG\n");
		return -1;
	}

/* #ifdef BATTERY_LOG_MESSAGE */
	/* pr_info("%s: addr(0x00), data(0x%04x)\n", __func__, */
		/* (status_data[1] << 8) | status_data[0]); */
/* #endif */

	if (status_data[1] & (0x1 << 2))
		ret = 1;

	status_data[1] = 0;
	if (max77729_bulk_write(fuelgauge->i2c, STATUS_REG, 2, status_data) < 0) {
		pr_info("Failed to write STATUS_REG\n");
		return -1;
	}

	return ret;
}

int max77729_get_fuelgauge_value(struct max77729_fuelgauge_data *fuelgauge, int data)
{
	int ret;

	switch (data) {
	case FG_LEVEL:
		ret = max77729_fg_read_soc(fuelgauge);
		break;
	case FG_TEMPERATURE:
		ret = max77729_fg_read_temp(fuelgauge);
		break;
	case FG_VOLTAGE:
		ret = max77729_fg_read_vcell(fuelgauge);
		break;
	case FG_CURRENT:
		ret = max77729_fg_read_current(fuelgauge, SEC_BATTERY_CURRENT_MA);
		break;
	case FG_CURRENT_AVG:
		ret = max77729_fg_read_avg_current(fuelgauge, SEC_BATTERY_CURRENT_MA);
		break;
	case FG_CHECK_STATUS:
		ret = max77729_fg_check_status_reg(fuelgauge);
		break;
	case FG_RAW_SOC:
		ret = max77729_fg_read_rawsoc(fuelgauge);
		break;
	case FG_VF_SOC:
		ret = max77729_fg_read_vfsoc(fuelgauge);
		break;
	case FG_AV_SOC:
		ret = max77729_fg_read_avsoc(fuelgauge);
		break;
	case FG_FULLCAP:
		ret = max77729_fg_read_fullcap(fuelgauge);
		if (ret == -1)
			ret = max77729_fg_read_fullcap(fuelgauge);
		break;
	case FG_FULLCAPNOM:
		ret = max77729_fg_read_fullcapnom(fuelgauge);
		if (ret == -1)
			ret = max77729_fg_read_fullcapnom(fuelgauge);
		break;
	case FG_FULLCAPREP:
		ret = max77729_fg_read_fullcaprep(fuelgauge);
		if (ret == -1)
			ret = max77729_fg_read_fullcaprep(fuelgauge);
		break;
	case FG_MIXCAP:
		ret = max77729_fg_read_mixcap(fuelgauge);
		break;
	case FG_AVCAP:
		ret = max77729_fg_read_avcap(fuelgauge);
		break;
	case FG_REPCAP:
		ret = max77729_fg_read_repcap(fuelgauge);
		break;
	case FG_CYCLE:
		ret = max77729_fg_read_cycle(fuelgauge);
		break;
	case FG_QH:
		ret = max77729_fg_read_qh(fuelgauge);
		break;
	case FG_QH_VF_SOC:
		ret = max77729_fg_read_qh_vfsoc(fuelgauge);
		break;
	case FG_ISYS:
		ret = max77729_fg_read_isys(fuelgauge, SEC_BATTERY_CURRENT_MA);
		break;
	case FG_ISYS_AVG:
		ret = max77729_fg_read_isys_avg(fuelgauge, SEC_BATTERY_CURRENT_MA);
		break;
	case FG_VSYS:
		ret = max77729_fg_read_vsys(fuelgauge);
		break;
	case FG_IIN:
		ret = max77729_fg_read_iin(fuelgauge, SEC_BATTERY_CURRENT_MA);
		break;
	case FG_VBYP:
		ret = max77729_fg_read_vbyp(fuelgauge);
		break;
	default:
		ret = -1;
		break;
	}

	return ret;
}

static void max77729_fg_periodic_read_power(struct max77729_fuelgauge_data *fuelgauge)
{
	int isys, isys_avg, vsys, iin, vbyp, qh, vfsoc, fullcaprep, vcell, avgvcell, fgcurrent, avgcurrent, remcap, mixcap, avcap, capnom, fullcap;
	u16 vempty, designcap, ichgterm, misccfg, fullsocthr, learncfg, ain0, tgain, toff, curve, qres, filtercfg, convgcfg;

	ichgterm = max77729_read_word(fuelgauge->i2c, ICHGTERM_REG);
	misccfg = max77729_read_word(fuelgauge->i2c, MISCCFG_REG);
	fullsocthr = max77729_read_word(fuelgauge->i2c, FULLSOCTHR_REG);


	ain0 = max77729_read_word(fuelgauge->i2c, 0x27);
	tgain = max77729_read_word(fuelgauge->i2c, 0x2c);
	toff = max77729_read_word(fuelgauge->i2c, 0x2d);
	curve = max77729_read_word(fuelgauge->i2c, 0xb9);

	misccfg = max77729_read_word(fuelgauge->i2c, MISCCFG_REG);
	isys = max77729_get_fuelgauge_value(fuelgauge, FG_ISYS);
	isys_avg = max77729_get_fuelgauge_value(fuelgauge, FG_ISYS_AVG);
	vsys = max77729_get_fuelgauge_value(fuelgauge, FG_VSYS);
	iin = max77729_get_fuelgauge_value(fuelgauge, FG_IIN);
	vbyp = max77729_get_fuelgauge_value(fuelgauge, FG_VBYP);
	qh = max77729_get_fuelgauge_value(fuelgauge, FG_QH);
	vfsoc = max77729_fg_read_vfsoc(fuelgauge);
	fullcaprep = max77729_fg_read_fullcaprep(fuelgauge);

	vempty = max77729_read_word(fuelgauge->i2c, VEMPTY_REG);
	designcap = max77729_read_word(fuelgauge->i2c, DESIGNCAP_REG);

	learncfg = max77729_read_word(fuelgauge->i2c, LEARN_CFG_REG);
	/* fullcaprep = max77729_read_word(fuelgauge->i2c, FULLCAP_REP_REG); */

	vcell = max77729_fg_read_vcell(fuelgauge);
	avgvcell = max77729_fg_read_avg_vcell(fuelgauge);
	fgcurrent = max77729_fg_read_current(fuelgauge, SEC_BATTERY_CURRENT_MA);
	avgcurrent = max77729_fg_read_avg_current(fuelgauge, SEC_BATTERY_CURRENT_MA);
	remcap = max77729_fg_read_repcap(fuelgauge);

	qres = max77729_read_word(fuelgauge->i2c, 0x0C);
	filtercfg = max77729_read_word(fuelgauge->i2c, FILTER_CFG_REG);
	convgcfg = max77729_read_word(fuelgauge->i2c, 0x49);
	mixcap = max77729_fg_read_mixcap(fuelgauge);
	fullcap = max77729_fg_read_fullcap(fuelgauge);
	avcap = max77729_fg_read_avcap(fuelgauge);
	capnom = max77729_fg_read_fullcapnom(fuelgauge);

	pr_debug("[FG periodic] ISYS(%dmA),ISYSAVG(%dmA),VSYS(%dmV),IIN(%dmA),VBYP(%dmV) QH(%d uah) VFSOC(%d) FULLCAPREP(%d) ICHGTERM(0x%x), MISC(0x%x), FULLSOCTHR(0x%x), VEMPTY(0x%x), DESIGNCAP(0x%x), LEARNCFG(0x%x) AIN0(0x%x) TGAIN(0x%x) TOFF(0x%x) CURVE(0x%x)\n",
		isys, isys_avg, vsys, iin, vbyp, qh, vfsoc, fullcaprep, ichgterm, misccfg, fullsocthr, vempty, designcap, learncfg, ain0, tgain, toff, curve);

	pr_debug("[FG periodic] VCELL(%dmV) AVGVCELL(%dmV) CURRENT(%dmA) AVGCURRENT(%dmA) REPCAP(%d), QRES(0x%x), FILTERCFG(0x%x), CONVGCFG(0x%x), MIXCAP(%d), FULLCAP(%d), AVCAP(%d), CAPNOM(%d)\n",
			vcell, avgvcell, fgcurrent, avgcurrent, remcap, qres, filtercfg, convgcfg, mixcap, fullcap, avcap, capnom);
}

static void max77729_fg_read_power_log(struct max77729_fuelgauge_data *fuelgauge)
{
	int vnow, inow;

	vnow = max77729_get_fuelgauge_value(fuelgauge, FG_VOLTAGE);
	inow = max77729_get_fuelgauge_value(fuelgauge, FG_CURRENT);

	/* pr_info("[FG info] VNOW(%dmV),INOW(%dmA)\n", vnow, inow); */
}

int max77729_fg_alert_init(struct max77729_fuelgauge_data *fuelgauge, int soc, int low_volt_thres)
{
	u8 misccgf_data[2], salrt_data[2], config_data[2], talrt_data[2];
	u16 read_data = 0;

	fuelgauge->is_fuel_alerted = false;

	/* Using RepSOC */
	if (max77729_bulk_read(fuelgauge->i2c, MISCCFG_REG, 2, misccgf_data) < 0) {
		pr_err("Failed to read MISCCFG_REG\n");
		return -1;
	}
	misccgf_data[0] = misccgf_data[0] & ~(0x03);

	if (max77729_bulk_write(fuelgauge->i2c, MISCCFG_REG, 2, misccgf_data) < 0) {
		pr_info("Failed to write MISCCFG_REG\n");
		return -1;
	}

	/* Reset VALRT Threshold setting (enable) */
	salrt_data[1] = 0xFF;
	salrt_data[0] = (low_volt_thres / 20);
	if (max77729_bulk_write(fuelgauge->i2c, VALRT_THRESHOLD_REG, 2, salrt_data) < 0) {
		pr_info("Failed to write VALRT_THRESHOLD_REG\n");
	}
	read_data = max77729_read_word(fuelgauge->i2c, VALRT_THRESHOLD_REG);
	/* pr_info("%s: HW V EMPTY Disable, SW V EMPTY Enable with %d mV (%d)\n", */
			/* __func__, low_volt_thres, (read_data & 0x00ff) * 20); */

	/* SALRT Threshold setting */
	salrt_data[1] = 0xff;
	salrt_data[0] = soc;
	if (max77729_bulk_write(fuelgauge->i2c, SALRT_THRESHOLD_REG, 2, salrt_data) < 0) {
		pr_info("Failed to write SALRT_THRESHOLD_REG\n");
		return -1;
	}

	/* Reset TALRT Threshold setting (disable) */
	talrt_data[1] = 0x7F;
	talrt_data[0] = 0x80;
	if (max77729_bulk_write(fuelgauge->i2c, TALRT_THRESHOLD_REG, 2, talrt_data) < 0) {
		pr_info("Failed to write TALRT_THRESHOLD_REG\n");
		return -1;
	}

	read_data = max77729_read_word(fuelgauge->i2c, TALRT_THRESHOLD_REG);
	if (read_data != 0x7f80)
		pr_err("TALRT_THRESHOLD_REG is not valid (0x%x)\n", read_data);


	/* Enable SOC alerts */
	if (max77729_bulk_read(fuelgauge->i2c, CONFIG_REG, 2, config_data) < 0) {
		pr_err("Failed to read CONFIG_REG\n");
		return -1;
	}
	config_data[0] = config_data[0] | (0x1 << 2);

	if (max77729_bulk_write(fuelgauge->i2c, CONFIG_REG, 2, config_data) < 0) {
		pr_info("Failed to write CONFIG_REG\n");
		return -1;
	}

	max77729_update_reg(fuelgauge->pmic, MAX77729_PMIC_REG_INTSRC_MASK, ~MAX77729_IRQSRC_FG, MAX77729_IRQSRC_FG);

	/* pr_info("[%s] SALRT(0x%02x%02x), CONFIG(0x%02x%02x)\n",	__func__, */
		/* salrt_data[1], salrt_data[0], config_data[1], config_data[0]); */

	return 1;
}

static int max77729_get_fuelgauge_soc(struct max77729_fuelgauge_data *fuelgauge)
{
	int fg_soc = 0;

	fg_soc = max77729_get_fuelgauge_value(fuelgauge, FG_RAW_SOC);
	if (fg_soc < 0) {
		pr_err("Can't read soc!!!\n");
		fg_soc = fuelgauge->info.soc;
	}

	return (fg_soc / 10);
}

static irqreturn_t max77729_jig_irq_thread(int irq, void *irq_data)
{
	struct max77729_fuelgauge_data *fuelgauge = irq_data;


	if (max77729_check_jig_status(fuelgauge))
		max77729_fg_reset_capacity_by_jig_connection(fuelgauge);
	else
		pr_info("jig removed\n");

	return IRQ_HANDLED;
}

bool max77729_fg_init(struct max77729_fuelgauge_data *fuelgauge)
{
	ktime_t current_time;
	struct timespec ts;
	u8 data[2] = { 0, 0 };

#if defined(ANDROID_ALARM_ACTIVATED)
	current_time = alarm_get_elapsed_realtime();
#else
	current_time = ktime_get_boottime();
#endif
	ts = ktime_to_timespec(ktime_get_boottime());

	fuelgauge->info.fullcap_check_interval = ts.tv_sec;
	fuelgauge->info.is_first_check = true;

	if (max77729_bulk_read(fuelgauge->i2c, CONFIG2_REG, 2, data) < 0) {
		pr_err("Failed to read CONFIG2_REG\n");
	} else if ((data[0] & 0x0F) != 0x05) {
		data[0] &= ~0x2F;
		data[0] |= (0x5 & 0xF);
		/* max77729_bulk_write(fuelgauge->i2c, CONFIG2_REG, 2, data); */
	}

	data[0] |= 0x80;
	max77729_bulk_write(fuelgauge->i2c, CONFIG2_REG, 2, data);

	if (max77729_read_word(fuelgauge->i2c, 0xB2) != fuelgauge->data_ver) {
		pr_err("fg data_ver missmatch. skip verify fg reg\n");
		fuelgauge->skip_fg_verify = true;
	} else {
		pr_err("fg data_ver match!(0x%x)\n", fuelgauge->data_ver);
		fuelgauge->skip_fg_verify = false;
	}

	if (fuelgauge->pdata->jig_gpio) {
		int ret;

		ret = request_threaded_irq(fuelgauge->pdata->jig_irq, NULL,
					max77729_jig_irq_thread,
					(fuelgauge->pdata->jig_low_active ?
					IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING)
					| IRQF_ONESHOT,
					"jig-irq", fuelgauge);
		if (ret)
			pr_info("Failed to Request JIG IRQ\n");

		/* initial check for the JIG */
		if (max77729_check_jig_status(fuelgauge))
			max77729_fg_reset_capacity_by_jig_connection(fuelgauge);
	}

	if (fuelgauge->pdata->thermal_source != SEC_BATTERY_THERMAL_SOURCE_FG) {
		if (max77729_bulk_read(fuelgauge->i2c, CONFIG_REG, 2, data) < 0) {
			pr_err("Failed to read CONFIG_REG\n");
			return false;
		}
		data[1] |= 0x1;

		if (max77729_bulk_write(fuelgauge->i2c, CONFIG_REG, 2, data) < 0) {
			pr_info("Failed to write CONFIG_REG\n");
			return false;
		}
	} else {
		if (max77729_bulk_read(fuelgauge->i2c, CONFIG_REG, 2, data) < 0) {
			pr_err("Failed to read CONFIG_REG\n");
			return false;
		}
		data[1] &= 0xFE;
		data[0] |= 0x10;

		if (max77729_bulk_write(fuelgauge->i2c, CONFIG_REG, 2, data) < 0) {
			pr_info("Failed to write CONFIG_REG\n");
			return false;
		}
	}

	return true;
}

bool max77729_fg_fuelalert_init(struct max77729_fuelgauge_data *fuelgauge, int soc, int low_volt_thres)
{
	/* 1. Set max77729 alert configuration. */
	if (max77729_fg_alert_init(fuelgauge, soc, low_volt_thres) > 0)
		return true;
	else
		return false;
}

void max77729_fg_fuelalert_set(struct max77729_fuelgauge_data *fuelgauge, int enable)
{
	u8 config_data[2], status_data[2];

	if (max77729_bulk_read(fuelgauge->i2c, CONFIG_REG, 2, config_data) < 0)
		pr_err("Failed to read CONFIG_REG\n");

	if (enable)
		config_data[0] |= ALERT_EN;
	else
		config_data[0] &= ~ALERT_EN;

	/* pr_info("%s: CONFIG(0x%02x%02x)\n", __func__, config_data[1], config_data[0]); */

	if (max77729_bulk_write(fuelgauge->i2c, CONFIG_REG, 2, config_data) < 0)
		pr_info("Failed to write CONFIG_REG\n");

	if (max77729_bulk_read(fuelgauge->i2c, STATUS_REG, 2, status_data) < 0)
		pr_err("Failed to read STATUS_REG\n");

	if (status_data[0] & 0x80) {
		/* pr_info("%s: harry soc is changed (%d)\n", */
				/* __func__, status_data[0]); */
		max77729_write_word(fuelgauge->i2c, STATUS_REG, 0xFF7F);
		if (fuelgauge->psy_fg)
			power_supply_changed(fuelgauge->psy_fg);
		if (!fuelgauge->psy_batt)
			fuelgauge->psy_batt = power_supply_get_by_name("battery");
		if (fuelgauge->psy_batt)
			power_supply_changed(fuelgauge->psy_batt);

	}

	if ((status_data[1] & 0x01) && !fuelgauge->is_charging) {
		/* pr_info("%s: harry Battery Voltage is Very Low!! V EMPTY(%d)\n", */
			/* __func__, fuelgauge->vempty_mode); */

		if (fuelgauge->psy_fg)
			power_supply_changed(fuelgauge->psy_fg);

		if (!fuelgauge->psy_batt)
			fuelgauge->psy_batt = power_supply_get_by_name("battery");
		if (fuelgauge->psy_batt)
			power_supply_changed(fuelgauge->psy_batt);
#if 0
		if (fuelgauge->vempty_mode != VEMPTY_MODE_HW)
			fuelgauge->vempty_mode = VEMPTY_MODE_SW_VALERT;
#if defined(CONFIG_BATTERY_CISD)
		else {
			if (!fuelgauge->valert_count_flag) {
				union power_supply_propval value;

				value.intval = fuelgauge->vempty_mode;
				psy_do_property("battery", set,
						POWER_SUPPLY_PROP_VOLTAGE_MIN, value);
				fuelgauge->valert_count_flag = true;
			}
		}
#endif
#endif

	}
}

bool max77729_fg_fuelalert_process(void *irq_data)
{
	struct max77729_fuelgauge_data *fuelgauge = (struct max77729_fuelgauge_data *)irq_data;

	max77729_fg_fuelalert_set(fuelgauge, 0);

	return true;
}

bool max77729_fg_reset(struct max77729_fuelgauge_data *fuelgauge)
{
	if (!max77729_fg_reset_soc(fuelgauge))
		return true;
	else
		return false;
}

static int max77729_fg_check_capacity_max(struct max77729_fuelgauge_data *fuelgauge, int capacity_max)
{
	int cap_max, cap_min;

	cap_max = fuelgauge->pdata->capacity_max;
	cap_min =
		(fuelgauge->pdata->capacity_max - fuelgauge->pdata->capacity_max_margin);

	return (capacity_max < cap_min) ? cap_min :
		((capacity_max >= cap_max) ? cap_max : capacity_max);
}

static int max77729_fg_get_soc_decimal(struct max77729_fuelgauge_data *fuelgauge)
{
	int raw_soc;

	raw_soc = max77729_get_fuelgauge_value(fuelgauge, FG_RAW_SOC);
	return raw_soc % 100;
}

static int max77729_fg_get_soc_decimal_rate(struct max77729_fuelgauge_data *fuelgauge)
{
	int soc, i;

	if (fuelgauge->dec_rate_len <= 0)
		return 0;

	soc = max77729_get_fuelgauge_soc(fuelgauge);
	soc /= 10;

	for (i = 0; i < fuelgauge->dec_rate_len; i += 2) {
		if (soc < fuelgauge->dec_rate_seq[i]) {
			return fuelgauge->dec_rate_seq[i - 1];
		}
	}

	return fuelgauge->dec_rate_seq[fuelgauge->dec_rate_len - 1];
}

static int max77729_fg_calculate_dynamic_scale(struct max77729_fuelgauge_data *fuelgauge, int capacity, bool scale_by_full)
{
	union power_supply_propval raw_soc_val;

	return 0; //force to return to disable dynamic scale in any time.

	raw_soc_val.intval = max77729_get_fuelgauge_value(fuelgauge, FG_RAW_SOC) / 10;

	if (raw_soc_val.intval <
		fuelgauge->pdata->capacity_max - fuelgauge->pdata->capacity_max_margin) {
		pr_info("raw soc(%d) is very low, skip routine\n", raw_soc_val.intval);
	} else if (fuelgauge->capacity_max_conv) {
		pr_info("skip dynamic scale routine\n");
	} else {
		fuelgauge->capacity_max = (raw_soc_val.intval * 100 / (capacity + 1));
		fuelgauge->capacity_old = capacity;

		fuelgauge->capacity_max =
			max77729_fg_check_capacity_max(fuelgauge, fuelgauge->capacity_max);

		pr_info("%d is used for capacity_max, capacity(%d)\n", fuelgauge->capacity_max, capacity);
		if ((capacity == 100) && !fuelgauge->capacity_max_conv && scale_by_full) {
			fuelgauge->capacity_max_conv = true;
			fuelgauge->g_capacity_max = raw_soc_val.intval;
			pr_info("Goal capacity max %d\n", fuelgauge->g_capacity_max);
		}
	}

	return fuelgauge->capacity_max;
}

static void max77729_lost_soc_reset(struct max77729_fuelgauge_data *fuelgauge)
{
	fuelgauge->lost_soc.ing = false;
	fuelgauge->lost_soc.prev_raw_soc = -1;
	fuelgauge->lost_soc.prev_remcap = 0;
	fuelgauge->lost_soc.prev_qh = 0;
	fuelgauge->lost_soc.lost_cap = 0;
	fuelgauge->lost_soc.weight = 0;
}

static void max77729_lost_soc_check_trigger_cond(struct max77729_fuelgauge_data *fuelgauge,
			int raw_soc, int d_raw_soc, int d_remcap, int d_qh)
{
	if (fuelgauge->lost_soc.prev_raw_soc >= fuelgauge->lost_soc.trig_soc ||
			d_raw_soc <= 0 || d_qh <= 0)
		return;

	/*
	 * raw soc is jumped over gap_soc
	 * and remcap is decreased more than trig_scale of qh
	 */
	if (d_raw_soc >= fuelgauge->lost_soc.trig_d_soc &&
			d_remcap >= (d_qh * fuelgauge->lost_soc.trig_scale)) {
		fuelgauge->lost_soc.ing = true;
		fuelgauge->lost_soc.lost_cap += d_remcap;

		/* calc weight */
		if (d_raw_soc >= fuelgauge->lost_soc.guarantee_soc)
			fuelgauge->lost_soc.weight += d_raw_soc / fuelgauge->lost_soc.guarantee_soc;
		else
			fuelgauge->lost_soc.weight += 1;

		if (fuelgauge->lost_soc.weight < 2)
			fuelgauge->lost_soc.weight = 2;

		pr_info("trigger: (unit:0.1%%) raw_soc(%d->%d), d_raw_soc(%d), d_remcap(%d), d_qh(%d), weight(%d)\n",
			fuelgauge->lost_soc.prev_raw_soc, raw_soc,
			d_raw_soc, d_remcap, d_qh, fuelgauge->lost_soc.weight);
	}
}

static int max77729_lost_soc_calc_soc(struct max77729_fuelgauge_data *fuelgauge,
		int request_soc, int d_qh, int d_remcap)
{
	int lost_soc = 0, gap_cap = 0;
	int vavg = 0, fullcaprep = 0, onecap = 0;

	vavg = max77729_fg_read_avg_vcell(fuelgauge);
	fullcaprep = max77729_fg_read_fullcaprep(fuelgauge);
	if (fullcaprep < 0) {
		fullcaprep = fuelgauge->battery_data->Capacity * fuelgauge->fg_resistor / 2;
		pr_info("ing: fullcaprep is replaced\n");
	}
	onecap = (fullcaprep / 100) + 1;

	if (d_qh < 0) {
		/* charging status, recover capacity is delta of remcap */
		if (d_remcap < 0)
			gap_cap = d_remcap * (-1);
		else
			gap_cap = d_remcap;
	} else if (d_qh == 0) {
		gap_cap = 1;
	} else {
		gap_cap = (d_qh * fuelgauge->lost_soc.weight);
	}

	if ((vavg < fuelgauge->lost_soc.min_vol) && (vavg > 0) && (gap_cap < onecap)) {
		gap_cap = onecap; /* reduce 1% */
		pr_info("ing: vavg(%d) is under min_vol(%d), reduce cap more(%d)\n",
			vavg, fuelgauge->lost_soc.min_vol, (fullcaprep / 100));
	}

	fuelgauge->lost_soc.lost_cap -= gap_cap;

	if (fuelgauge->lost_soc.lost_cap > 0) {
		lost_soc = (fuelgauge->lost_soc.lost_cap * 1000) / fullcaprep;
		pr_info("ing: (unit:0.1%%) calc_soc(%d), lost_soc(%d), lost_cap(%d), d_qh(%d), weight(%d)\n",
			request_soc + lost_soc, lost_soc, fuelgauge->lost_soc.lost_cap,
			d_qh, fuelgauge->lost_soc.weight);
	} else {
		lost_soc = 0;
		max77729_lost_soc_reset(fuelgauge);
		pr_info("done: (unit:0.1%%) request_soc(%d), lost_soc(%d), lost_cap(%d)\n",
			request_soc, lost_soc, fuelgauge->lost_soc.lost_cap);
	}

	return lost_soc;
}

static int max77729_lost_soc_get(struct max77729_fuelgauge_data *fuelgauge, int request_soc)
{
	int raw_soc, remcap, qh; /* now values */
	int d_raw_soc, d_remcap, d_qh; /* delta between prev values */
	int report_soc;

	/* get current values */
	raw_soc = max77729_get_fuelgauge_value(fuelgauge, FG_RAW_SOC) / 10;
	remcap = max77729_get_fuelgauge_value(fuelgauge, FG_REPCAP);
	qh = max77729_get_fuelgauge_value(fuelgauge, FG_QH) / 1000;

	if (fuelgauge->lost_soc.prev_raw_soc < 0) {
		fuelgauge->lost_soc.prev_raw_soc = raw_soc;
		fuelgauge->lost_soc.prev_remcap = remcap;
		fuelgauge->lost_soc.prev_qh = qh;
		fuelgauge->lost_soc.lost_cap = 0;
		pr_info("init: raw_soc(%d), remcap(%d), qh(%d)\n",
			raw_soc, remcap, qh);

		return request_soc;
	}

	/* get diff values with prev */
	d_raw_soc = fuelgauge->lost_soc.prev_raw_soc - raw_soc;
	d_remcap = fuelgauge->lost_soc.prev_remcap - remcap;
	d_qh = fuelgauge->lost_soc.prev_qh - qh;

	max77729_lost_soc_check_trigger_cond(fuelgauge, raw_soc, d_raw_soc, d_remcap, d_qh);

	/* backup prev values */
	fuelgauge->lost_soc.prev_raw_soc = raw_soc;
	fuelgauge->lost_soc.prev_remcap = remcap;
	fuelgauge->lost_soc.prev_qh = qh;

	if (!fuelgauge->lost_soc.ing)
		return request_soc;

	report_soc = request_soc + max77729_lost_soc_calc_soc(fuelgauge, request_soc, d_qh, d_remcap);

	if (report_soc > 1000)
		report_soc = 1000;
	if (report_soc < 0)
		report_soc = 0;

	return report_soc;
}


#if defined(CONFIG_EN_OOPS)
static void max77729_set_full_value(struct max77729_fuelgauge_data *fuelgauge, int cable_type)
{
	u16 ichgterm, misccfg, fullsocthr;

	if (is_hv_wireless_type(cable_type) || is_hv_wire_type(cable_type)) {
		ichgterm = fuelgauge->battery_data->ichgterm_2nd;
		misccfg = fuelgauge->battery_data->misccfg_2nd;
		fullsocthr = fuelgauge->battery_data->fullsocthr_2nd;
	} else {
		ichgterm = fuelgauge->battery_data->ichgterm;
		misccfg = fuelgauge->battery_data->misccfg;
		fullsocthr = fuelgauge->battery_data->fullsocthr;
	}

	max77729_write_word(fuelgauge->i2c, ICHGTERM_REG, ichgterm);
	max77729_write_word(fuelgauge->i2c, MISCCFG_REG, misccfg);
	max77729_write_word(fuelgauge->i2c, FULLSOCTHR_REG, fullsocthr);

	pr_info("ICHGTERM(0x%04x) FULLSOCTHR(0x%04x), MISCCFG(0x%04x)\n",
		ichgterm, misccfg, fullsocthr);
}
#endif

static int max77729_fg_check_initialization_result(struct max77729_fuelgauge_data *fuelgauge)
{
	u8 data[2];

	if (max77729_bulk_read(fuelgauge->i2c, FG_INIT_RESULT_REG, 2, data) < 0) {
		pr_err("Failed to read FG_INIT_RESULT_REG\n");
		return SEC_BAT_ERROR_CAUSE_I2C_FAIL;
	}

	if (data[1] == 0xFF) {
		pr_info("initialization is failed.(0x%02X:0x%04X)\n",
			FG_INIT_RESULT_REG, data[1] << 8 | data[0]);
		return SEC_BAT_ERROR_CAUSE_FG_INIT_FAIL;
	} else {
		pr_info("initialization is succeed.(0x%02X:0x%04X)\n",
			FG_INIT_RESULT_REG, data[1] << 8 | data[0]);
	}

	return SEC_BAT_ERROR_CAUSE_NONE;
}

static int max77729_fg_create_attrs(struct device *dev)
{
	int i, rc;

	for (i = 0; i < (int)ARRAY_SIZE(max77729_fg_attrs); i++) {
		rc = device_create_file(dev, &max77729_fg_attrs[i]);
		if (rc)
			goto create_attrs_failed;
	}
	return rc;

create_attrs_failed:
	pr_err("failed (%d)\n", rc);
	while (i--)
		device_remove_file(dev, &max77729_fg_attrs[i]);
	return rc;
}

ssize_t max77729_fg_show_attrs(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct max77729_fuelgauge_data *fuelgauge = power_supply_get_drvdata(psy);
	const ptrdiff_t offset = attr - max77729_fg_attrs;
	int i = 0, j = 0;
	u8 addr = 0;
	u16 data = 0;

	pr_info("(%ld)\n", offset);

	switch (offset) {
	case FG_DATA:
		for (j = 0; j <= 0x0F; j++) {
			for (addr = 0; addr < 0x10; addr++) {
				data = max77729_read_word(fuelgauge->i2c, addr + j * 0x10);
				i += scnprintf(buf + i, PAGE_SIZE - i,
						"0x%02x:\t0x%04x\n", addr + j * 0x10, data);
			}
			if (j == 5)
				j = 0x0A;
		}
		break;
	default:
		return -EINVAL;
	}
	return i;
}

ssize_t max77729_fg_store_attrs(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct max77729_fuelgauge_data *fuelgauge = power_supply_get_drvdata(psy);
	const ptrdiff_t offset = attr - max77729_fg_attrs;
	int ret = 0, x, y;

	pr_info("(%ld)\n", offset);

	switch (offset) {
	case FG_DATA:
		if (sscanf(buf, "0x%8x 0x%8x", &x, &y) == 2) {
			if (x >= VALRT_THRESHOLD_REG && x <= VFSOC_REG) {
				u8 addr = x;
				u16 data = y;

				if (max77729_write_word(fuelgauge->i2c, addr, data) < 0)
					pr_info("addr: 0x%x write fail\n", addr);
			} else {
				pr_info("addr: 0x%x is wrong\n", x);
			}
		}
		ret = count;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

#define SHUTDOWN_DELAY_VOL	3400
#define SHUTDOWN_VOL	3300
static int max77729_fg_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	struct max77729_fuelgauge_data *fuelgauge = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property)psp;
	union power_supply_propval usb_online_val = {0, };
	u8 data[2] = { 0, 0 };
	int vbat_mv;
	int vbat_curr;
	int vbat_soc;
	static bool last_shutdown_delay;

#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	union power_supply_propval b_val = {0,};

	if (fuelgauge->verify_psy == NULL)
		fuelgauge->verify_psy = power_supply_get_by_name("batt_verify");
	if ((psp == POWER_SUPPLY_PROP_AUTHENTIC) ||
			(psp == POWER_SUPPLY_PROP_ROMID) ||
			(psp == POWER_SUPPLY_PROP_DS_STATUS) ||
			(psp == POWER_SUPPLY_PROP_PAGE0_DATA) ||
			(psp == POWER_SUPPLY_PROP_CHIP_OK)) {
		if (fuelgauge->verify_psy == NULL) {
			pr_err("verify_psy is NULL\n");
			return -ENODATA;
		}
	}
#endif

	switch ((int)psp) {
		/* Cell voltage (VCELL, mV) */
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	case POWER_SUPPLY_PROP_AUTHENTIC:
		power_supply_get_property(fuelgauge->verify_psy,
				POWER_SUPPLY_PROP_AUTHEN_RESULT, &b_val);
		val->intval = b_val.intval;
		break;
	case POWER_SUPPLY_PROP_ROMID:
		power_supply_get_property(fuelgauge->verify_psy,
				POWER_SUPPLY_PROP_ROMID, &b_val);
		memcpy(val->arrayval, b_val.arrayval, 8);
		break;
	case POWER_SUPPLY_PROP_DS_STATUS:
		power_supply_get_property(fuelgauge->verify_psy,
				POWER_SUPPLY_PROP_DS_STATUS, &b_val);
		memcpy(val->arrayval, b_val.arrayval, 8);
		break;
	case POWER_SUPPLY_PROP_PAGE0_DATA:
		power_supply_get_property(fuelgauge->verify_psy,
				POWER_SUPPLY_PROP_PAGE0_DATA, &b_val);
		memcpy(val->arrayval, b_val.arrayval, 16);
		break;
	case POWER_SUPPLY_PROP_CHIP_OK:
		power_supply_get_property(fuelgauge->verify_psy,
				POWER_SUPPLY_PROP_CHIP_OK, &b_val);
		val->intval = b_val.intval;
		break;
#endif
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = fuelgauge->cable_type;
		break;
	case POWER_SUPPLY_PROP_RESISTANCE:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		if (fuelgauge->battery_data->battery_id == BATTERY_VENDOR_UNKNOWN) {
			fuelgauge->battery_data->battery_id = fgauge_get_battery_id();
		}
		val->intval = fuelgauge->battery_data->battery_id;
		break;
	case POWER_SUPPLY_PROP_SHUTDOWN_DELAY:
		val->intval = fuelgauge->shutdown_delay;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_VOLTAGE);
		val->intval *= 1000;
		/* pr_err("%s: VOLTAGE_NOW (%d)\n",__func__, val->intval); */
		break;
		/* Additional Voltage Information (mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		switch (val->intval) {
		case SEC_BATTERY_VOLTAGE_OCV:
			val->intval = max77729_fg_read_vfocv(fuelgauge);
			break;
		case SEC_BATTERY_VOLTAGE_AVERAGE:
		default:
			val->intval = max77729_fg_read_avg_vcell(fuelgauge);
			break;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = 5000000;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO; //"Li-poly"
		break;
	case POWER_SUPPLY_PROP_BATTERY_TYPE:
		switch (fgauge_get_battery_id()) {
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

		/* Current */
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		vbat_curr = max77729_fg_read_current(fuelgauge, SEC_BATTERY_CURRENT_UA); //match onpmi & factory test cit apk
		val->intval = clamp(vbat_curr, -10000000, 10000000); //fix xts issue the current shouled be positive when charging
		/* pr_err("%s: CURRENT_NOW (%d)\n",__func__, val->intval); */
		break;
		/* Average Current */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		switch (val->intval) {
		case SEC_BATTERY_CURRENT_UA:
			val->intval = max77729_fg_read_avg_current(fuelgauge,
					SEC_BATTERY_CURRENT_UA);
			break;
		case SEC_BATTERY_CURRENT_MA:
		default:
			fuelgauge->current_avg = val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_CURRENT_AVG);
			break;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_FULLCAP);
		break;
		/* Full Capacity */
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		switch (val->intval) {
		case SEC_BATTERY_CAPACITY_DESIGNED:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_FULLCAP);
			break;
		case SEC_BATTERY_CAPACITY_ABSOLUTE:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_MIXCAP);
			break;
		case SEC_BATTERY_CAPACITY_TEMPERARY:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_AVCAP);
			break;
		case SEC_BATTERY_CAPACITY_CURRENT:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_REPCAP);
			break;
		case SEC_BATTERY_CAPACITY_AGEDCELL:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_FULLCAPNOM);
			break;
		case SEC_BATTERY_CAPACITY_CYCLE:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_CYCLE);
			break;
		case SEC_BATTERY_CAPACITY_FULL:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_FULLCAPREP);
			break;
		case SEC_BATTERY_CAPACITY_QH:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_QH);
			break;
		case SEC_BATTERY_CAPACITY_VFSOC:
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_QH_VF_SOC);
			break;
		}
		break;
		/* SOC (%) */
	case POWER_SUPPLY_PROP_CAPACITY:
		vbat_soc = max77729_get_fuelgauge_soc(fuelgauge);
		vbat_soc = clamp(vbat_soc, 0, 1000);

		fuelgauge->raw_capacity = vbat_soc;

		if (fuelgauge->pdata->capacity_calculation_type &
				SEC_FUELGAUGE_CAPACITY_TYPE_LOST_SOC)
			vbat_soc = max77729_lost_soc_get(fuelgauge, fuelgauge->raw_capacity);

		val->intval = clamp(vbat_soc * 100 / 990, 0, 100);

		if (fuelgauge->is_fuel_alerted) {
			if (fuelgauge->info.soc <= 100)
				max77729_fg_fuelalert_init(fuelgauge,
						fuelgauge->pdata->fuel_alert_soc, 3400);
			else
				max77729_fg_fuelalert_init(fuelgauge,
						fuelgauge->pdata->fuel_alert_soc, 2800);

		}
		max77729_get_fuelgauge_value(fuelgauge, FG_TEMPERATURE);

		if (fuelgauge->shutdown_delay_enable) {
			if (val->intval == 0) {
				vbat_mv = max77729_fg_read_vcell(fuelgauge);
				psy_do_property("usb", get, POWER_SUPPLY_PROP_ONLINE, usb_online_val);
				if (vbat_mv > SHUTDOWN_DELAY_VOL) {
					val->intval = 1;
					if (usb_online_val.intval)
						fuelgauge->shutdown_delay = false;
				} else if (vbat_mv > SHUTDOWN_VOL) {
					if (!usb_online_val.intval) {
						fuelgauge->shutdown_delay = true;
						val->intval = 1;
					} else {
						fuelgauge->shutdown_delay = false;
						val->intval = 1;
					}
				} else {
					fuelgauge->shutdown_delay = false;
					val->intval = 0;
				}
			} else {
				fuelgauge->shutdown_delay = false;
			}
			if (last_shutdown_delay != fuelgauge->shutdown_delay) {
				last_shutdown_delay = fuelgauge->shutdown_delay;
				if (fuelgauge->psy_fg)
					power_supply_changed(fuelgauge->psy_fg);

				//pr_info("%s: bms set battery work\n", __func__);
				schedule_delayed_work(&fuelgauge->shutdown_delay_work, msecs_to_jiffies(1500));
			}
		}
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_TEMPERATURE);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (fuelgauge->fake_temp == -EINVAL) {
			val->intval = max77729_get_fuelgauge_value(fuelgauge, FG_TEMPERATURE);
		} else {
			val->intval = fuelgauge->fake_temp;
		}
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		{
			int fullcap = max77729_get_fuelgauge_value(fuelgauge, FG_FULLCAPNOM);

			val->intval = fullcap * 100 /
				(fuelgauge->battery_data->Capacity * fuelgauge->fg_resistor / 2);
			/* pr_info("%s: asoc(%d), fullcap(%d)\n", __func__, */
				/* val->intval, fullcap); */
#if !defined(CONFIG_SEC_FACTORY)
			max77729_fg_periodic_read(fuelgauge);
#endif
		}
		break;
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
		val->intval = fuelgauge->is_fastcharge;
		break;
	case POWER_SUPPLY_PROP_SOC_DECIMAL:
		val->intval = max77729_fg_get_soc_decimal(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_SOC_DECIMAL_RATE:
		val->intval = max77729_fg_get_soc_decimal_rate(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = fuelgauge->capacity_max;
		break;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		return -ENODATA;
#endif
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = fuelgauge->raw_capacity *
			(fuelgauge->battery_data->Capacity * fuelgauge->fg_resistor / 2);
		//val->intval = val->intval / 1000;
		break;
	case POWER_SUPPLY_PROP_SOH:
		val->intval = max77729_fg_read_SoH(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = max77729_fg_read_cycle(fuelgauge);
		break;
	case POWER_SUPPLY_EXT_PROP_MIN ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_MONITOR_WORK:
			max77729_fg_read_power_log(fuelgauge);
			break;
		case POWER_SUPPLY_EXT_PROP_ERROR_CAUSE:
			val->intval = max77729_fg_check_initialization_result(fuelgauge);
			break;
		case POWER_SUPPLY_EXT_PROP_MEASURE_SYS:
			switch (val->intval) {
			case SEC_BATTERY_VSYS:
				val->intval = max77729_fg_read_vsys(fuelgauge);
				break;
			case SEC_BATTERY_ISYS_AVG_UA:
				val->intval = max77729_fg_read_isys_avg(fuelgauge,
						SEC_BATTERY_CURRENT_UA);
				break;
			case SEC_BATTERY_ISYS_AVG_MA:
				val->intval = max77729_fg_read_isys_avg(fuelgauge,
						SEC_BATTERY_CURRENT_MA);
				break;
			case SEC_BATTERY_ISYS_UA:
				val->intval = max77729_fg_read_isys(fuelgauge,
						SEC_BATTERY_CURRENT_UA);
				break;
			case SEC_BATTERY_ISYS_MA:
			default:
				val->intval = max77729_fg_read_isys(fuelgauge,
						SEC_BATTERY_CURRENT_MA);
				break;
			}
			break;
		case POWER_SUPPLY_EXT_PROP_MEASURE_INPUT:
			switch (val->intval) {
			case SEC_BATTERY_VBYP:
				val->intval = max77729_fg_read_vbyp(fuelgauge);
				break;
			case SEC_BATTERY_IIN_UA:
				val->intval = max77729_fg_read_iin(fuelgauge,
						SEC_BATTERY_CURRENT_UA);
				break;
			case SEC_BATTERY_IIN_MA:
			default:
				val->intval = max77729_fg_read_iin(fuelgauge,
						SEC_BATTERY_CURRENT_MA);
				break;
			}
			break;
		case POWER_SUPPLY_EXT_PROP_JIG_GPIO:
			if (fuelgauge->pdata->jig_gpio)
				val->intval = gpio_get_value(fuelgauge->pdata->jig_gpio);
			else
				val->intval = -1;
			/* pr_info("%s: jig gpio = %d\n", __func__, val->intval); */
			break;
		case POWER_SUPPLY_EXT_PROP_FILTER_CFG:
			max77729_bulk_read(fuelgauge->i2c, FILTER_CFG_REG, 2, data);
			val->intval = data[1] << 8 | data[0];
			/* pr_debug("%s: FilterCFG=0x%04X\n", __func__, data[1] << 8 | data[0]); */
			break;
		case POWER_SUPPLY_EXT_PROP_CHARGING_ENABLED:
			val->intval = fuelgauge->is_charging;
			break;
		case POWER_SUPPLY_EXT_PROP_BATTERY_ID:
			val->intval = fuelgauge->battery_data->battery_id;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#if defined(CONFIG_UPDATE_BATTERY_DATA)
static int max77729_fuelgauge_parse_dt(struct max77729_fuelgauge_data *fuelgauge);
#endif
static int max77729_fg_set_property(struct power_supply *psy,
			enum power_supply_property psp,
			const union power_supply_propval *val)
{
	struct max77729_fuelgauge_data *fuelgauge = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property)psp;
	static bool low_temp_wa;
	u8 data[2] = { 0, 0 };
	u16 reg_data;

	switch ((int)psp) {
	case POWER_SUPPLY_PROP_STATUS:
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (fuelgauge->pdata->capacity_calculation_type &
				SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE)
			max77729_fg_calculate_dynamic_scale(fuelgauge, val->intval, true);
		break;
#if defined(CONFIG_EN_OOPS)
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		max77729_set_full_value(fuelgauge, val->intval);
		break;
#endif
	case POWER_SUPPLY_PROP_ONLINE:
		fuelgauge->cable_type = val->intval;
		if (!is_nocharge_type(fuelgauge->cable_type)) {
			/* enable alert */
			/* if (fuelgauge->vempty_mode >= VEMPTY_MODE_SW_VALERT) { */
				/* max77729_fg_set_vempty(fuelgauge, VEMPTY_MODE_HW); */
				/* fuelgauge->initial_update_of_soc = true; */
				max77729_fg_fuelalert_init(fuelgauge,
					fuelgauge->pdata->fuel_alert_soc, 2800);
			/* } */
		}
		break;
		/* Battery Temperature */
	case POWER_SUPPLY_PROP_CAPACITY:
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RESET) {
			if (!max77729_fg_reset(fuelgauge))
				return -EINVAL;
			fuelgauge->initial_update_of_soc = true;
			if (fuelgauge->pdata->capacity_calculation_type &
				SEC_FUELGAUGE_CAPACITY_TYPE_LOST_SOC)
				max77729_lost_soc_reset(fuelgauge);
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (true) {
			fuelgauge->fake_temp = val->intval;
		} else {
			if (val->intval < 0) {
				reg_data = max77729_read_word(fuelgauge->i2c, DESIGNCAP_REG);
				if (reg_data == fuelgauge->battery_data->Capacity) {
					max77729_write_word(fuelgauge->i2c, DESIGNCAP_REG,
							fuelgauge->battery_data->Capacity + 3);
					pr_info("set the low temp reset! temp: %d, capacity: 0x%x, original capacity: 0x%x\n",
							val->intval, reg_data, fuelgauge->battery_data->Capacity);
				}
			}

			if (val->intval < 0 && !low_temp_wa) {
				low_temp_wa = true;
				max77729_write_word(fuelgauge->i2c, 0x29, 0xCEA7);
				pr_info("FilterCFG(0x%0x)\n",
						max77729_read_word(fuelgauge->i2c, 0x29));
			} else if (val->intval > 30 && low_temp_wa) {
				low_temp_wa = false;
				max77729_write_word(fuelgauge->i2c, 0x29, 0xCEA4);
				pr_info("FilterCFG(0x%0x)\n",
						max77729_read_word(fuelgauge->i2c, 0x29));
			}
			max77729_fg_write_temp(fuelgauge, val->intval);
		}
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		break;
	case POWER_SUPPLY_PROP_FASTCHARGE_MODE:
		fuelgauge->is_fastcharge = val->intval;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		/* pr_info("%s: POWER_SUPPLY_PROP_ENERGY_NOW\n", __func__); */
		max77729_fg_reset_capacity_by_jig_connection(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		/* pr_info("%s: capacity_max changed, %d -> %d\n", */
			/* __func__, fuelgauge->capacity_max, val->intval); */
		fuelgauge->capacity_max =
			max77729_fg_check_capacity_max(fuelgauge, val->intval);
		fuelgauge->initial_update_of_soc = true;
		break;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		{
			u16 reg_fullsocthr;
			int val_soc = val->intval;

			if (val->intval > fuelgauge->pdata->full_condition_soc
					|| val->intval <= (fuelgauge->pdata->full_condition_soc - 10)) {
				/* pr_info("%s: abnormal value(%d). so thr is changed to default(%d)\n", */
					 /* __func__, val->intval, fuelgauge->pdata->full_condition_soc); */
				val_soc = fuelgauge->pdata->full_condition_soc;
			}

			reg_fullsocthr = val_soc << 8;
			if (max77729_write_word(fuelgauge->i2c, FULLSOCTHR_REG,
					reg_fullsocthr) < 0) {
				pr_info("Failed to write FULLSOCTHR_REG\n");
			} else {
				reg_fullsocthr =
					max77729_read_word(fuelgauge->i2c, FULLSOCTHR_REG);
				pr_info("FullSOCThr %d%%(0x%04X)\n",
					val_soc, reg_fullsocthr);
			}
		}
		break;
#endif
	case POWER_SUPPLY_EXT_PROP_MIN ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_FILTER_CFG:
			/* Set FilterCFG */
			max77729_bulk_read(fuelgauge->i2c, FILTER_CFG_REG, 2, data);
			/* pr_debug("%s: FilterCFG=0x%04X\n", __func__, data[1] << 8 | data[0]); */
			data[0] &= ~0xF;
			data[0] |= (val->intval & 0xF);
			max77729_bulk_write(fuelgauge->i2c, FILTER_CFG_REG, 2, data);

			max77729_bulk_read(fuelgauge->i2c, FILTER_CFG_REG, 2, data);
			/* pr_debug("%s: FilterCFG=0x%04X\n", __func__, data[1] << 8 | data[0]); */
			break;
		case POWER_SUPPLY_EXT_PROP_CHARGING_ENABLED:
			switch (val->intval) {
			case SEC_BAT_CHG_MODE_BUCK_OFF:
			case SEC_BAT_CHG_MODE_CHARGING_OFF:
				fuelgauge->is_charging = false;
				break;
			case SEC_BAT_CHG_MODE_CHARGING:
				fuelgauge->is_charging = true;
				break;
			};
			break;
#if defined(CONFIG_UPDATE_BATTERY_DATA)
		case POWER_SUPPLY_EXT_PROP_POWER_DESIGN:
			max77729_fuelgauge_parse_dt(fuelgauge);
			break;
#endif
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static u8 fgauge_get_battery_id(void)
{
	struct power_supply *verify_psy;
	static u8 get_battery_id = BATTERY_VENDOR_UNKNOWN;
	union power_supply_propval pval = {0, };
	int rc;

	verify_psy = power_supply_get_by_name("batt_verify");
	if (verify_psy != NULL) {
		rc = power_supply_get_property(verify_psy,
				POWER_SUPPLY_PROP_CHIP_OK, &pval);
		if (rc < 0)
			pr_err("get romid error\n");

		if (pval.intval) {
			rc = power_supply_get_property(verify_psy,
					POWER_SUPPLY_PROP_PAGE0_DATA, &pval);
			if (rc < 0) {
				pr_err("get page0 error\n");
			} else {
				if (pval.arrayval[0] == 'N') {
					get_battery_id = BATTERY_VENDOR_NVT;
				} else if (pval.arrayval[0] == 'C' || pval.arrayval[0] == 'V') {
					get_battery_id = BATTERY_VENDOR_GY;
				} else if (pval.arrayval[0] == 'L' || pval.arrayval[0] == 'X' || pval.arrayval[0] == 'S') {
					get_battery_id = BATTERY_VENDOR_XWD;
				} else {
					get_battery_id = BATTERY_VENDOR_UNKNOWN;
				}
			}
		}
		power_supply_put(verify_psy);
	}

	pr_info("battery_id = %d\n", get_battery_id);
	return get_battery_id;
}

static void max77729_fg_isr_work(struct work_struct *work)
{
	struct max77729_fuelgauge_data *fuelgauge = container_of(work, struct max77729_fuelgauge_data, isr_work.work);

	/* process for fuel gauge chip */
	max77729_fg_fuelalert_process(fuelgauge);

	__pm_relax(fuelgauge->fuel_alert_ws);
}

static void max77729_shutdown_delay_work(struct work_struct *work)
{
	struct max77729_fuelgauge_data *fuelgauge = container_of(work, struct max77729_fuelgauge_data, shutdown_delay_work.work);
	union power_supply_propval shutdown_val;

	shutdown_val.intval = fuelgauge->shutdown_delay;
	pr_info("shutdown_delay: %d\n", shutdown_val.intval);
	psy_do_property("battery", set, POWER_SUPPLY_PROP_SHUTDOWN_DELAY, shutdown_val);
}

static irqreturn_t max77729_fg_irq_thread(int irq, void *irq_data)
{
	struct max77729_fuelgauge_data *fuelgauge = irq_data;
	u16 reg_data;

	reg_data = max77729_read_word(fuelgauge->i2c, STATUS_REG);
	/* pr_info("%s, state [%x]\n", __func__, reg_data); */

	if (reg_data == 0x0)
		return IRQ_HANDLED; // workaround, if unknow or wrong fg interrupt, ignore it!

	max77729_update_reg(fuelgauge->pmic, MAX77729_PMIC_REG_INTSRC_MASK,
			MAX77729_IRQSRC_FG, MAX77729_IRQSRC_FG);
	if (fuelgauge->is_fuel_alerted)
		return IRQ_HANDLED;

	__pm_stay_awake(fuelgauge->fuel_alert_ws);
	fuelgauge->is_fuel_alerted = true;
	schedule_delayed_work(&fuelgauge->isr_work, msecs_to_jiffies(0));

	return IRQ_HANDLED;
}

#ifdef CONFIG_OF
#define PROPERTY_NAME_SIZE 128
static void max77729_fuelgauge_parse_dt_lost_soc(struct max77729_fuelgauge_data *fuelgauge, struct device_node *np)
{
	int ret;

	ret = of_property_read_u32(np, "fuelgauge,lost_soc_trig_soc",
			&fuelgauge->lost_soc.trig_soc);
	if (ret < 0)
		fuelgauge->lost_soc.trig_soc = 1000; /* 100% */

	ret = of_property_read_u32(np, "fuelgauge,lost_soc_trig_d_soc",
			&fuelgauge->lost_soc.trig_d_soc);
	if (ret < 0)
		fuelgauge->lost_soc.trig_d_soc = 20; /* 2% */

	ret = of_property_read_u32(np, "fuelgauge,lost_soc_trig_scale",
			&fuelgauge->lost_soc.trig_scale);
	if (ret < 0)
		fuelgauge->lost_soc.trig_scale = 2; /* 2x */

	ret = of_property_read_u32(np, "fuelgauge,lost_soc_guarantee_soc",
			&fuelgauge->lost_soc.guarantee_soc);
	if (ret < 0)
		fuelgauge->lost_soc.guarantee_soc = 20; /* 2% */

	ret = of_property_read_u32(np, "fuelgauge,lost_soc_min_vol",
			&fuelgauge->lost_soc.min_vol);
	if (ret < 0)
		fuelgauge->lost_soc.min_vol = 3200; /* 3.2V */

	pr_info("trigger soc(%d), d_soc(%d), scale(%d), guarantee_soc(%d), min_vol(%d)\n",
		fuelgauge->lost_soc.trig_soc, fuelgauge->lost_soc.trig_d_soc,
		fuelgauge->lost_soc.trig_scale, fuelgauge->lost_soc.guarantee_soc,
		fuelgauge->lost_soc.min_vol);
}

static int max77729_fuelgauge_parse_dt(struct max77729_fuelgauge_data *fuelgauge)
{
	struct device_node *np = of_find_node_by_name(NULL, "max77729-fuelgauge");
	max77729_fuelgauge_platform_data_t *pdata = fuelgauge->pdata;
	int ret, len;
	u8 battery_id = BATTERY_VENDOR_UNKNOWN;
	const u32 *p;

	/* reset, irq gpio info */
	if (np == NULL) {
		pr_err("np NULL\n");
	} else {
		ret = of_property_read_u32(np, "fuelgauge,capacity_max",
				&pdata->capacity_max);
		if (ret < 0)
			pr_err("error reading capacity_max (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_max_margin",
				&pdata->capacity_max_margin);
		if (ret < 0) {
			pr_err("error reading capacity_max_margin (%d)\n", ret);
			pdata->capacity_max_margin = 300;
		}

		ret = of_property_read_u32(np, "fuelgauge,capacity_min",
				&pdata->capacity_min);
		if (ret < 0)
			pr_err("error reading capacity_min (%d)\n", ret);

		pdata->capacity_min = 0;
		ret = of_property_read_u32(np, "fuelgauge,capacity_calculation_type",
				&pdata->capacity_calculation_type);
		if (ret < 0)
			pr_err("error reading capacity_calculation_type (%d)\n", ret);

		pdata->capacity_calculation_type = 0x02;
		ret = of_property_read_u32(np, "fuelgauge,fuel_alert_soc",
				&pdata->fuel_alert_soc);
		if (ret < 0)
			pr_err("error reading pdata->fuel_alert_soc (%d)\n", ret);

		pdata->fuel_alert_soc = 0;
		pdata->repeated_fuelalert = of_property_read_bool(np, "fuelgauge,repeated_fuelalert");
		fuelgauge->using_temp_compensation = of_property_read_bool(np, "fuelgauge,using_temp_compensation");
		if (fuelgauge->using_temp_compensation) {
			ret = of_property_read_u32(np, "fuelgauge,low_temp_limit",
					&fuelgauge->low_temp_limit);
			if (ret < 0) {
				pr_err("error reading low temp limit (%d)\n", ret);
				fuelgauge->low_temp_limit = 0; /* Default: 0'C */
			}

			ret = of_property_read_u32(np, "fuelgauge,vempty_recover_time",
					&fuelgauge->vempty_recover_time);
			if (ret < 0) {
				pr_err("error reading vempty recover time (%d)\n", ret);
				fuelgauge->vempty_recover_time = 0; /* Default: 0 */
			}
		}

		fuelgauge->using_hw_vempty = of_property_read_bool(np, "fuelgauge,using_hw_vempty");
		if (fuelgauge->using_hw_vempty) {
			ret = of_property_read_u32(np, "fuelgauge,sw_v_empty_voltage",
					&fuelgauge->battery_data->sw_v_empty_vol);
			if (ret < 0)
				pr_err("error reading sw_v_empty_default_vol (%d)\n", ret);

			ret = of_property_read_u32(np, "fuelgauge,sw_v_empty_voltage_cisd",
					&fuelgauge->battery_data->sw_v_empty_vol_cisd);
			if (ret < 0) {
				pr_err("error reading sw_v_empty_default_vol_cise (%d)\n", ret);
				fuelgauge->battery_data->sw_v_empty_vol_cisd = 3100;
			}

			ret = of_property_read_u32(np, "fuelgauge,sw_v_empty_recover_voltage",
					&fuelgauge->battery_data->sw_v_empty_recover_vol);
			if (ret < 0)
				pr_err("error reading sw_v_empty_recover_vol (%d)\n", ret);

			/* pr_info("%s : SW V Empty (%d)mV, SW V Empty recover (%d)mV\n", */
				/* __func__, fuelgauge->battery_data->sw_v_empty_vol, */
				/* fuelgauge->battery_data->sw_v_empty_recover_vol); */
		}

		pdata->jig_gpio = of_get_named_gpio(np, "fuelgauge,jig_gpio", 0);
		if (pdata->jig_gpio >= 0) {
			pdata->jig_irq = gpio_to_irq(pdata->jig_gpio);
			pdata->jig_low_active = of_property_read_bool(np, "fuelgauge,jig_low_active");
		} else {
			/* pr_err("%s: error reading jig_gpio = %d\n", */
				/* __func__, pdata->jig_gpio); */
			pdata->jig_gpio = 0;
		}

		ret = of_property_read_u32(np, "fuelgauge,fg_resistor",
				&fuelgauge->fg_resistor);
		if (ret < 0) {
			pr_err("error reading fg_resistor (%d)\n", ret);
			fuelgauge->fg_resistor = 1;
		}
		fuelgauge->fg_resistor = 5;
#if defined(CONFIG_EN_OOPS)
		ret = of_property_read_u32(np, "fuelgauge,ichgterm",
				&fuelgauge->battery_data->ichgterm);
		if (ret < 0)
			pr_err("error reading ichgterm (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,ichgterm_2nd",
				&fuelgauge->battery_data->ichgterm_2nd);
		if (ret < 0)
			pr_err("error reading ichgterm_2nd (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,misccfg",
				&fuelgauge->battery_data->misccfg);
		if (ret < 0)
			pr_err("error reading misccfg (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,misccfg_2nd",
				&fuelgauge->battery_data->misccfg_2nd);
		if (ret < 0)
			pr_err("error reading misccfg_2nd (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,fullsocthr",
				&fuelgauge->battery_data->fullsocthr);
		if (ret < 0)
			pr_err("error reading fullsocthr (%d)\n", ret);

		ret = of_property_read_u32(np, "fuelgauge,fullsocthr_2nd",
				&fuelgauge->battery_data->fullsocthr_2nd);
		if (ret < 0)
			pr_err("error reading fullsocthr_2nd (%d)\n", ret);
#endif
		/*decimal rate*/
		len = 0;
		p = of_get_property(np, "fuelgauge,soc_decimal_rate", &len);
		if (p) {
			fuelgauge->dec_rate_seq = kzalloc(len, GFP_KERNEL);
			fuelgauge->dec_rate_len = len / sizeof(*fuelgauge->dec_rate_seq);
			//pr_err("%s: len= %ld, length= %d, %d\n", __func__,
			//		sizeof(int) * len, len, fuelgauge->verify_selected_reg_length);
			ret = of_property_read_u32_array(np, "fuelgauge,soc_decimal_rate",
					fuelgauge->dec_rate_seq, fuelgauge->dec_rate_len);
			if (ret) {
				/* pr_err("%s: failed to read dec_rate data: %d\n", */
						/* __func__, ret); */
				kfree(fuelgauge->dec_rate_seq);
				fuelgauge->dec_rate_seq = NULL;
			}
		} else {
			/* pr_err("%s: there is no decimal data\n", __func__); */
		}

		pdata->bat_id_gpio = of_get_named_gpio(np, "fuelgauge,bat_id_gpio", 0);
		if (pdata->bat_id_gpio >= 0) {
			/* 0: BAT_ID LOW, 1: BAT_ID OPEN */
			battery_id = gpio_get_value(pdata->bat_id_gpio);
		} else {
			battery_id = fgauge_get_battery_id();
			/* pr_err("%s: reading battery_id = %d\n", */
				/* __func__, battery_id); */
		}

		fuelgauge->battery_data->battery_id = battery_id;
		/*for fake battery test scene*/
		if (battery_id == BATTERY_VENDOR_UNKNOWN)
			battery_id = 1;

		/* pr_info("%s: battery_id = %d\n", __func__, fuelgauge->battery_data->battery_id); */

		if (fuelgauge->pdata->capacity_calculation_type & SEC_FUELGAUGE_CAPACITY_TYPE_LOST_SOC)
			max77729_fuelgauge_parse_dt_lost_soc(fuelgauge, np);

		/* SoC 0%, shutdown delay feature */
		fuelgauge->shutdown_delay_enable = of_property_read_bool(np, "battery,shutdown-delay-enable");

		/* get battery_params node */
		{
			struct device_node *bp_np;

			bp_np = of_find_node_by_name(np, "battery_params");
			if (bp_np == NULL) {
				pr_err("Cannot find child node \"battery_params\"\n");
				of_node_put(np);
				return -EINVAL;
			} else {
				char prop_name[PROPERTY_NAME_SIZE];

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "v_empty");
				ret = of_property_read_u32(bp_np, prop_name,
					&fuelgauge->battery_data->V_empty);
				if (ret < 0)
					pr_err("error reading v_empty (%d)\n", ret);

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "v_empty_origin");
				ret = of_property_read_u32(bp_np, prop_name,
					&fuelgauge->battery_data->V_empty_origin);
				if (ret < 0)
					pr_err("error reading v_empty_origin (%d)\n", ret);

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "capacity");
				ret = of_property_read_u32(bp_np, prop_name,
					&fuelgauge->battery_data->Capacity);
				if (ret < 0)
					pr_err("error reading capacity (%d)\n", ret);

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "fg_reset_wa_data");
				len = of_property_count_u32_elems(bp_np, prop_name);
				if (len != FG_RESET_DATA_COUNT) {
					pr_err("fg_reset_wa_data is %d < %d, need more data\n",
							len, FG_RESET_DATA_COUNT);
					fuelgauge->fg_reset_data = NULL;
				} else {
					fuelgauge->fg_reset_data = kzalloc(sizeof(struct fg_reset_wa), GFP_KERNEL);
					ret = of_property_read_u32_array(bp_np, prop_name,
							(u32 *)fuelgauge->fg_reset_data, FG_RESET_DATA_COUNT);
					if (ret < 0) {
						pr_err("failed to read fuelgauge->fg_reset_wa_data (%d)\n", ret);

						kfree(fuelgauge->fg_reset_data);
						fuelgauge->fg_reset_data = NULL;
					}
				}
				/* pr_info("%s: V_empty(0x%04x), v_empty_origin(0x%04x), capacity(0x%04x)\n", */
					/* __func__, fuelgauge->battery_data->V_empty, */
					/* fuelgauge->battery_data->V_empty_origin, fuelgauge->battery_data->Capacity); */

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "data_ver");
				ret = of_property_read_u32(bp_np, prop_name,
					&fuelgauge->data_ver);
				if (ret < 0) {
					pr_err("error reading data_ver (%d)\n", ret);
					fuelgauge->data_ver = 0xff;
				}
				/* pr_info("%s: fg data_ver (%x)\n", __func__, fuelgauge->data_ver); */

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
					battery_id, "battery_model");
				len = of_property_count_u32_elems(bp_np, prop_name);
				if (len != FG_MODEL_DATA_COUNT) {
					pr_err("fg_model_data is %d < %d, need more data\n",
							len, FG_MODEL_DATA_COUNT);
					fuelgauge->fg_model_data = NULL;
				} else {
					fuelgauge->fg_model_data = kzalloc(FG_MODEL_DATA_COUNT*sizeof(u32), GFP_KERNEL);
					ret = of_property_read_u32_array(bp_np, prop_name,
							fuelgauge->fg_model_data, FG_MODEL_DATA_COUNT);
					if (ret < 0) {
						pr_err("failed to read fuelgauge->fg_model_data (%d)\n", ret);

						kfree(fuelgauge->fg_model_data);
						fuelgauge->fg_model_data = NULL;
					}
				}

				snprintf(prop_name, PROPERTY_NAME_SIZE, "battery%d,%s",
						battery_id, "selected_reg");
				p = of_get_property(bp_np, prop_name, &len);
				if (p) {
					fuelgauge->verify_selected_reg = kzalloc(len, GFP_KERNEL);
					fuelgauge->verify_selected_reg_length = len / (int)sizeof(struct verify_reg);
					/* pr_err("%s: len= %ld, length= %d, %d\n", __func__, */
							/* sizeof(int) * len, len, fuelgauge->verify_selected_reg_length); */
					ret = of_property_read_u32_array(bp_np, prop_name,
							(u32 *)fuelgauge->verify_selected_reg, len / sizeof(u32));
					if (ret) {
						pr_err("failed to read fuelgauge->verify_selected_reg (%d)\n", ret);
						kfree(fuelgauge->verify_selected_reg);
						fuelgauge->verify_selected_reg = NULL;
					}
				} else {
					pr_err("there is not selected_reg\n");
					fuelgauge->verify_selected_reg = NULL;
				}

				of_node_put(bp_np);
			}
		}

		{
			struct device_node *tmp;

			tmp = of_find_node_by_name(NULL, "battery");
			if (tmp) {
				ret = of_property_read_u32(tmp, "battery,thermal_source",
						&pdata->thermal_source);
				if (ret < 0)
					pr_err("error reading pdata->thermal_source (%d)\n", ret);
				of_node_put(tmp);
			}

			tmp = of_find_node_by_name(NULL, "cable-info");
			if (tmp) {
				ret = of_property_read_u32(tmp, "full_check_current_1st",
						&pdata->full_check_current_1st);
				ret = of_property_read_u32(tmp, "full_check_current_2nd",
						&pdata->full_check_current_2nd);

#if defined(CONFIG_BATTERY_AGE_FORECAST)
				ret = of_property_read_u32(tmp, "battery,full_condition_soc",
						&pdata->full_condition_soc);
				if (ret) {
					pdata->full_condition_soc = 93;
					pr_info("Full condition soc is Empty\n");
				}
#endif
				of_node_put(tmp);
			}
		}

		/* pr_info("%s: thermal: %d, jig_gpio: %d, capacity_max: %d\n" */
			/* "capacity_max_margin: %d, capacity_min: %d\n" */
			/* "calculation_type: 0x%x, fuel_alert_soc: %d,\n" */
			/* "repeated_fuelalert: %d, fg_resistor : %d\n", */
			/* __func__, pdata->thermal_source, pdata->jig_gpio, pdata->capacity_max, */
			/* pdata->capacity_max_margin, pdata->capacity_min, */
			/* pdata->capacity_calculation_type, pdata->fuel_alert_soc, */
			/* pdata->repeated_fuelalert, fuelgauge->fg_resistor); */

		of_node_put(np);
	}
	/* pr_info("%s: (%d)\n", __func__, fuelgauge->battery_data->Capacity); */

	return 0;
}
#endif

static const struct power_supply_desc max77729_fuelgauge_power_supply_desc = {
	.name = "bms",
	.type = POWER_SUPPLY_TYPE_BMS,
	.properties = max77729_fuelgauge_props,
	.num_properties = ARRAY_SIZE(max77729_fuelgauge_props),
	.get_property = max77729_fg_get_property,
	.set_property = max77729_fg_set_property,
	.property_is_writeable = max77729_fuelgauge_prop_is_writeable,
	.no_thermal = true,
};

static int max77729_fuelgauge_probe(struct platform_device *pdev)
{
	struct max77729_dev *max77729 = dev_get_drvdata(pdev->dev.parent);
	struct max77729_platform_data *pdata = dev_get_platdata(max77729->dev);
	max77729_fuelgauge_platform_data_t *fuelgauge_data;
	struct max77729_fuelgauge_data *fuelgauge;
	struct power_supply_config fuelgauge_cfg = {};
	union power_supply_propval raw_soc_val;
	struct power_supply *verify_psy = NULL;
	int ret = 0;

	pr_info("probe start\n");
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	if (max77729_read_word(max77729->fuelgauge, STATUS_REG) & BIT(1)) {
		//if fg chip por flag set, should be wait for battery id from ds28e16
		verify_psy = power_supply_get_by_name("batt_verify");
		if (!verify_psy) {
			pr_err("batt_verify psy not ready, defer probe\n");
			return -EPROBE_DEFER;
		}
	}
#endif

	fuelgauge = kzalloc(sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	fuelgauge_data = kzalloc(sizeof(max77729_fuelgauge_platform_data_t), GFP_KERNEL);
	if (!fuelgauge_data) {
		ret = -ENOMEM;
		goto err_free;
	}

	mutex_init(&fuelgauge->fg_lock);

	fuelgauge->dev = &pdev->dev;
	fuelgauge->pdata = fuelgauge_data;
	fuelgauge->i2c = max77729->fuelgauge;
	fuelgauge->pmic = max77729->i2c;
	fuelgauge->max77729_pdata = pdata;

	fuelgauge->fake_temp = -EINVAL;
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	fuelgauge->verify_psy = verify_psy;
#endif
#if defined(CONFIG_OF)
	fuelgauge->battery_data = kzalloc(sizeof(struct battery_data_t), GFP_KERNEL);
	if (!fuelgauge->battery_data) {
		pr_err("Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_pdata_free;
	}
	ret = max77729_fuelgauge_parse_dt(fuelgauge);
	if (ret < 0)
		pr_err("not found fg dt! ret(%d)\n", ret);
#endif

	platform_set_drvdata(pdev, fuelgauge);

	max77729_fg_model_load(fuelgauge);

	fuelgauge->capacity_max = fuelgauge->pdata->capacity_max;
	fuelgauge->g_capacity_max = 0;
	fuelgauge->capacity_max_conv = false;
	max77729_lost_soc_reset(fuelgauge);

	raw_soc_val.intval = max77729_get_fuelgauge_value(fuelgauge, FG_RAW_SOC) / 10;

	if (raw_soc_val.intval > fuelgauge->capacity_max)
		max77729_fg_calculate_dynamic_scale(fuelgauge, 100, false);

	if (!max77729_fg_init(fuelgauge)) {
		pr_err("Failed to Initialize Fuelgauge\n");
		goto err_data_free;
	}

	/* SW/HW init code. SW/HW V Empty mode must be opposite ! */
	fuelgauge->vempty_init_flag = false;	/* default value */
	/* pr_info("%s: SW/HW V empty init\n", __func__); */
	max77729_fg_set_vempty(fuelgauge, VEMPTY_MODE_SW);

	fuelgauge_cfg.drv_data = fuelgauge;

	fuelgauge->psy_fg = power_supply_register(&pdev->dev,
			&max77729_fuelgauge_power_supply_desc,
			&fuelgauge_cfg);
	if (IS_ERR(fuelgauge->psy_fg)) {
		ret = PTR_ERR(fuelgauge->psy_fg);
		pr_err("Failed to Register psy_fg(%d)\n", ret);
		goto err_data_free;
	}

	fuelgauge->fg_irq = pdata->irq_base + MAX77729_FG_IRQ_ALERT;
	/* pr_info("[%s]IRQ_BASE(%d) FG_IRQ(%d)\n", */
		/* __func__, pdata->irq_base, fuelgauge->fg_irq); */

	fuelgauge->is_fuel_alerted = false;
	if (fuelgauge->pdata->fuel_alert_soc >= 0) {
		if (max77729_fg_fuelalert_init(fuelgauge, fuelgauge->pdata->fuel_alert_soc, 2800)) {
			fuelgauge->fuel_alert_ws = wakeup_source_register(&pdev->dev, "fuel_alerted");
			if (fuelgauge->fg_irq) {
				INIT_DELAYED_WORK(&fuelgauge->isr_work, max77729_fg_isr_work);

				INIT_DELAYED_WORK(&fuelgauge->shutdown_delay_work, max77729_shutdown_delay_work);

				ret = request_threaded_irq(fuelgauge->fg_irq, NULL,
						max77729_fg_irq_thread,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"fuelgauge-irq", fuelgauge);
				if (ret) {
					pr_err("Failed to Request IRQ(%d)\n", fuelgauge->fg_irq);
					goto err_supply_unreg;
				}
			}
		} else {
			pr_err("Failed to Initialize Fuel-alert\n");
			goto err_supply_unreg;
		}
	}

	ret = max77729_fg_create_attrs(&fuelgauge->psy_fg->dev);
	if (ret) {
		pr_err("Failed to create_attrs\n");
		goto err_irq;
	}

	/* vote(fuelgauge->fcc_votable, PROFILE_CHG_VOTER, true, CHG_FCC_CURR_MAX); */
	fuelgauge->sleep_initial_update_of_soc = false;
	fuelgauge->initial_update_of_soc = true;

#if defined(CONFIG_BATTERY_CISD)
	fuelgauge->valert_count_flag = false;
#endif
	pr_info("probe success\n");
	return 0;

err_irq:
	free_irq(fuelgauge->fg_irq, fuelgauge);
err_supply_unreg:
	power_supply_unregister(fuelgauge->psy_fg);
	wakeup_source_unregister(fuelgauge->fuel_alert_ws);
err_data_free:
#if defined(CONFIG_OF)
	kfree(fuelgauge->battery_data);
#endif
err_pdata_free:
	kfree(fuelgauge_data);
	mutex_destroy(&fuelgauge->fg_lock);
err_free:
	kfree(fuelgauge);
	pr_err("probe failed\n");
	return ret;
}

static int max77729_fuelgauge_remove(struct platform_device *pdev)
{
	struct max77729_fuelgauge_data *fuelgauge = platform_get_drvdata(pdev);

	/* pr_info("%s: ++\n", __func__); */

	if (fuelgauge->psy_fg)
		power_supply_unregister(fuelgauge->psy_fg);

	if (fuelgauge->psy_batt)
		power_supply_unregister(fuelgauge->psy_batt);

	free_irq(fuelgauge->fg_irq, fuelgauge);
	wakeup_source_unregister(fuelgauge->fuel_alert_ws);
#if defined(CONFIG_OF)
	kfree(fuelgauge->battery_data);
#endif
	kfree(fuelgauge->pdata);
	mutex_destroy(&fuelgauge->fg_lock);
	kfree(fuelgauge);

	/* pr_info("%s: --\n", __func__); */

	return 0;
}

static int max77729_fuelgauge_suspend(struct device *dev)
{
	return 0;
}

static int max77729_fuelgauge_resume(struct device *dev)
{
	struct max77729_fuelgauge_data *fuelgauge = dev_get_drvdata(dev);

	fuelgauge->sleep_initial_update_of_soc = true;

	return 0;
}

static void max77729_fuelgauge_shutdown(struct platform_device *pdev)
{
	struct max77729_fuelgauge_data *fuelgauge = platform_get_drvdata(pdev);

	/* pr_info("%s: ++\n", __func__); */

	free_irq(fuelgauge->fg_irq, fuelgauge);
	if (fuelgauge->pdata->jig_gpio)
		free_irq(fuelgauge->pdata->jig_irq, fuelgauge);

	cancel_delayed_work(&fuelgauge->isr_work);
	cancel_delayed_work(&fuelgauge->shutdown_delay_work);

	/* pr_info("%s: --\n", __func__); */
}

static SIMPLE_DEV_PM_OPS(max77729_fuelgauge_pm_ops,
		max77729_fuelgauge_suspend,
		max77729_fuelgauge_resume);

static struct platform_driver max77729_fuelgauge_driver = {
	.driver = {
		.name = "max77729-fuelgauge",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &max77729_fuelgauge_pm_ops,
#endif
	},
	.probe = max77729_fuelgauge_probe,
	.remove = max77729_fuelgauge_remove,
	.shutdown = max77729_fuelgauge_shutdown,
};

static int __init max77729_fuelgauge_init(void)
{
	return platform_driver_register(&max77729_fuelgauge_driver);
}
fs_initcall(max77729_fuelgauge_init);

static void __exit max77729_fuelgauge_exit(void)
{
	platform_driver_unregister(&max77729_fuelgauge_driver);
}

/* module_init(max77729_fuelgauge_init); */
module_exit(max77729_fuelgauge_exit);

MODULE_DESCRIPTION("max77729 Fuel Gauge Driver");
MODULE_LICENSE("GPL");

