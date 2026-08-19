// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2012-2020, FocalTech Systems, Ltd., all rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*****************************************************************************
 *
 * File Name: focaltech_core.c
 *
 * Author: Focaltech Driver Team
 *
 * Created: 2016-08-08
 *
 * Abstract: entrance for focaltech ts driver
 *
 * Version: V1.0
 *
 *****************************************************************************/

/*****************************************************************************
 * Included header files
 *****************************************************************************/
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <drm/drm_panel.h>
#include "focaltech_core.h"

/*****************************************************************************
 * Private constant and macro definitions using #define
 *****************************************************************************/
#define FTS_DRIVER_NAME		"fts_ts"
#define FTS_IRQ_NAME		"fts_ts_irq"
#define INTERVAL_READ_REG	200 /* unit:ms */
#define TIMEOUT_READ_REG	1000 /* unit:ms */
#if FTS_POWER_SOURCE_CUST_EN
#define FTS_VTG_MIN_UV		2800000
#define FTS_VTG_MAX_UV		3300000
#define FTS_I2C_VTG_MIN_UV	1800000
#define FTS_I2C_VTG_MAX_UV	1800000
#endif

//enable 'check touch vendor' feature
#define CHECK_TOUCH_VENDOR

#ifdef CHECK_TOUCH_VENDOR
extern char mtkfb_lcm_name[256];
#endif

/*****************************************************************************
 * Global variable or extern global variabls/functions
 *****************************************************************************/
struct fts_ts_data *fts_data;
static DEFINE_MUTEX(fts_panel_lock);
static struct drm_panel *active_panel;

/*****************************************************************************
 * Static function prototypes
 *****************************************************************************/
static int fts_ts_suspend(struct device *dev);
static int fts_ts_resume(struct device *dev);

/*****************************************************************************
 *  Name: fts_wait_tp_to_valid
 *  Brief: Read chip id until TP FW become valid(Timeout: TIMEOUT_READ_REG),
 *      need call when reset/power on/resume...
 *  Input:
 *  Output:
 *  Return: return 0 if tp valid, otherwise return error code
 *****************************************************************************/
int fts_wait_tp_to_valid(bool can_sleep)
{
	int cnt, ret;
	const int max_tries = TIMEOUT_READ_REG / INTERVAL_READ_REG;
	u8 idh = 0, idl = 0;
	u8 chip_idh, chip_idl;
	int last_err = 0;

	if (!fts_data) {
		FTS_ERROR("fts_data not initialized");
		return -ENODEV;
	}

	chip_idh = fts_data->ic_info.ids.chip_idh;
	chip_idl = fts_data->ic_info.ids.chip_idl;

	for (cnt = 0; cnt < max_tries; cnt++) {
		ret = fts_read_reg(FTS_REG_CHIP_ID, &idh);
		if (ret < 0) {
			last_err = ret;
			FTS_DEBUG("read CHIP_ID failed (%d), try %d/%d", ret, cnt + 1, max_tries);
			goto out;
		}

		ret = fts_read_reg(FTS_REG_CHIP_ID2, &idl);
		if (ret < 0) {
			last_err = ret;
			FTS_DEBUG("read CHIP_ID2 failed (%d), try %d/%d", ret, cnt + 1, max_tries);
			goto out;
		}

		if (idh == chip_idh && idl == chip_idl) {
			FTS_DEBUG("TP Ready, Device ID:0x%02x%02x", idh, idl);
			return 0;
		}

		FTS_DEBUG("TP Not Ready, ReadData:0x%02x%02x (try %d/%d)",
			  idh, idl, cnt + 1, max_tries);

out:
		/* Context-aware sleep */
		if (can_sleep) {
			if (msleep_interruptible(INTERVAL_READ_REG)) {
				FTS_ERROR("wait interrupted");
				return -EINTR;
			}
		} else {
			/* Atomic context: busy wait */
			mdelay(INTERVAL_READ_REG);
		}
	}

	FTS_ERROR("TP not responding after %d ms; last i2c err=%d",
		  max_tries * INTERVAL_READ_REG, last_err);
	return -EIO;
}

#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
static struct xiaomi_touch_interface xiaomi_touch_interfaces;
static int fts_get_mode_value(int mode, int value_type);
static int fts_get_mode_all(int mode, int *value);
static int fts_reset_mode(int mode);
static int fts_set_cur_value(int fts_mode, int fts_value);
static void fts_init_touchmode_data(void);
#endif

/*****************************************************************************
 *  Name: fts_tp_state_recovery
 *  Brief: Need execute this function when reset
 *  Input:
 *  Output:
 *  Return:
 *****************************************************************************/
void fts_tp_state_recovery(struct fts_ts_data *ts_data)
{
	if (!ts_data) {
		FTS_ERROR("ts_data not initialized");
		return;
	}

	/* wait tp stable */
	fts_wait_tp_to_valid(true);
	/* recover TP charger state 0x8B */
	/* recover TP glove state 0xC0 */
	/* recover TP cover state 0xC1 */
	fts_ex_mode_recovery(ts_data);
	/* recover TP gesture state 0xD0 */
	fts_gesture_recovery(ts_data);
}

int fts_reset_proc(int hdelayms)
{
	if (!fts_data) {
		FTS_ERROR("fts_data not initialized");
		return -ENODEV;
	}

	gpio_direction_output(fts_data->pdata->reset_gpio, 0);
	usleep_range(1000, 1500);
	gpio_direction_output(fts_data->pdata->reset_gpio, 1);
	if (hdelayms)
		msleep(hdelayms);

	return 0;
}

void fts_irq_disable(void)
{
	unsigned long irqflags;

	if (!fts_data) {
		FTS_ERROR("fts_data not initialized");
		return;
	}

	spin_lock_irqsave(&fts_data->irq_lock, irqflags);

	if (!fts_data->irq_disabled) {
		disable_irq_nosync(fts_data->irq);
		fts_data->irq_disabled = true;
	}

	spin_unlock_irqrestore(&fts_data->irq_lock, irqflags);
}

void fts_irq_enable(void)
{
	unsigned long irqflags;

	if (!fts_data) {
		FTS_ERROR("fts_data not initialized");
		return;
	}

	spin_lock_irqsave(&fts_data->irq_lock, irqflags);

	if (fts_data->irq_disabled) {
		enable_irq(fts_data->irq);
		fts_data->irq_disabled = false;
	}

	spin_unlock_irqrestore(&fts_data->irq_lock, irqflags);
}

void fts_hid2std(void)
{
	int ret = 0;
	u8 buf[3] = {0xEB, 0xAA, 0x09};

	if (!fts_data || fts_data->bus_type != BUS_TYPE_I2C)
		return;

	ret = fts_write(buf, 3);
	if (ret < 0) {
		FTS_ERROR("hid2std cmd write fail");
		return;
	}

	usleep_range(10000, 12000);
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	ret = fts_read(NULL, 0, buf, 3);
	if (ret < 0)
		FTS_ERROR("hid2std cmd read fail");
	else if (buf[0] == 0xEB && buf[1] == 0xAA && buf[2] == 0x08)
		FTS_DEBUG("hidi2c change to stdi2c successful");
	else
		FTS_DEBUG("hidi2c change to stdi2c not support or fail");
}

static int fts_get_chip_types(struct fts_ts_data *ts_data,
			      u8 id_h, u8 id_l, bool fw_valid)
{
	int i = 0;
	struct ft_chip_t ctype[] = FTS_CHIP_TYPE_MAPPING;
	u32 ctype_entries = sizeof(ctype) / sizeof(struct ft_chip_t);

	if (!ts_data) {
		FTS_ERROR("ts_data not initialized");
		return -ENODEV;
	}

	if (id_h == 0x0 || id_l == 0x0) {
		FTS_ERROR("id_h/id_l is 0");
		return -EINVAL;
	}

	FTS_DEBUG("verify id: 0x%02x%02x", id_h, id_l);
	for (i = 0; i < ctype_entries; i++) {
		if (fw_valid == VALID) {
			if (id_h == ctype[i].chip_idh && id_l == ctype[i].chip_idl)
				break;
		} else {
			if ((id_h == ctype[i].rom_idh && id_l == ctype[i].rom_idl) ||
			    (id_h == ctype[i].pb_idh && id_l == ctype[i].pb_idl) ||
			    (id_h == ctype[i].bl_idh && id_l == ctype[i].bl_idl))
				break;
		}
	}

	if (i >= ctype_entries)
		return -ENODATA;

	ts_data->ic_info.ids = ctype[i];
	return 0;
}

static int fts_read_bootid(struct fts_ts_data *ts_data, u8 *id)
{
	int ret = 0;
	u8 chip_id[2] = {0};
	u8 id_cmd[4] = {0};
	u32 id_cmd_len = 0;

	if (!ts_data) {
		FTS_ERROR("ts_data not initialized");
		return -ENODEV;
	}

	id_cmd[0] = FTS_CMD_START1;
	id_cmd[1] = FTS_CMD_START2;
	ret = fts_write(id_cmd, 2);
	if (ret < 0) {
		FTS_ERROR("start cmd write fail");
		return ret;
	}

	msleep(FTS_CMD_START_DELAY);
	id_cmd[0] = FTS_CMD_READ_ID;
	id_cmd[1] = 0x00;
	id_cmd[2] = 0x00;
	id_cmd[3] = 0x00;
	if (ts_data->ic_info.is_incell)
		id_cmd_len = FTS_CMD_READ_ID_LEN_INCELL;
	else
		id_cmd_len = FTS_CMD_READ_ID_LEN;

	ret = fts_read(id_cmd, id_cmd_len, chip_id, 2);
	if (ret < 0 || chip_id[0] == 0x0 || chip_id[1] == 0x0) {
		FTS_ERROR("read boot id fail, read: 0x%02x%02x, ret=%d",
			  chip_id[0], chip_id[1], ret);
		return -EIO;
	}

	id[0] = chip_id[0];
	id[1] = chip_id[1];
	return 0;
}

/*****************************************************************************
 * Name: fts_get_ic_information
 * Brief: read chip id to get ic information, after run the function,
 *      driver will know which IC is it.
 *      If cant get the ic information, maybe not focaltech's touch IC,
 *      need unregister the driver.
 * Input:
 * Output:
 * Return: return 0 if get correct ic information, otherwise return error code
 *****************************************************************************/
static int fts_get_ic_information(struct fts_ts_data *ts_data)
{
	int i;
	const int max_tries = TIMEOUT_READ_REG / INTERVAL_READ_REG;
	u8 chip_idh = 0, chip_idl = 0;
	u8 boot_id[2] = { 0, 0 };
	int last_i2c_err = 0;
	int ret;

	if (!ts_data) {
		FTS_ERROR("ts_data not initialized");
		return -ENODEV;
	}

	ts_data->ic_info.is_incell = FTS_CHIP_IDC;
	ts_data->ic_info.hid_supported = FTS_HID_SUPPORTTED;

	FTS_DEBUG("start normal polling for chip id (max %d tries)", max_tries);
	for (i = 0; i < max_tries; i++) {
		ret = fts_read_reg(FTS_REG_CHIP_ID, &chip_idh);
		if (ret < 0) {
			last_i2c_err = ret;
			FTS_ERROR("read CHIP_ID failed (%d) try %d/%d",
				  ret, i + 1, max_tries);
			goto out;
		}

		ret = fts_read_reg(FTS_REG_CHIP_ID2, &chip_idl);
		if (ret < 0) {
			last_i2c_err = ret;
			FTS_ERROR("read CHIP_ID2 failed (%d) try %d/%d",
				  ret, i + 1, max_tries);
			goto out;
		}

		if (chip_idh == 0x00 || chip_idl == 0x00) {
			FTS_DEBUG("chip id read invalid 0x%02x%02x try %d/%d",
				  chip_idh, chip_idl, i + 1, max_tries);
			goto out;
		}

		ret = fts_get_chip_types(ts_data, chip_idh, chip_idl, VALID);
		if (ret == 0) {
			ts_data->ic_info.ids.chip_idh = chip_idh;
			ts_data->ic_info.ids.chip_idl = chip_idl;
			FTS_INFO("get ic information, chip_id: 0x%02x%02x",
				 chip_idh, chip_idl);
			return 0;
		}

		FTS_DEBUG("fts_get_chip_types returned %d for 0x%02x%02x try %d/%d",
			  ret, chip_idh, chip_idl, i + 1, max_tries);
out:
		msleep(INTERVAL_READ_REG);
	}

	FTS_INFO("normal polling timed out after %d ms, last_i2c_err=%d",
		 max_tries * INTERVAL_READ_REG, last_i2c_err);
	if (ts_data->ic_info.hid_supported) {
		FTS_INFO("attempt HID->STD conversion as part of fallback");
		fts_hid2std();
	}

	ret = fts_read_bootid(ts_data, boot_id);
	if (ret < 0) {
		FTS_ERROR("read boot id fail (%d)", ret);
		return ret;
	}

	ret = fts_get_chip_types(ts_data, boot_id[0], boot_id[1], INVALID);
	if (ret < 0) {
		FTS_ERROR("can't get ic information from boot id (%d)", ret);
		return ret;
	}

	ts_data->ic_info.ids.chip_idh = boot_id[0];
	ts_data->ic_info.ids.chip_idl = boot_id[1];
	FTS_INFO("get ic information from boot id, chip_id: 0x%02x%02x",
		 boot_id[0], boot_id[1]);

	return 0;
}

/*****************************************************************************
 *  Reprot related
 *****************************************************************************/
static void fts_show_touch_buffer(u8 *data, int datalen)
{
	char tmpbuf[FTS_TOUCH_DATA_LEN * 3 + 1];
	int count = 0;
	int i;

	if (fts_data->log_level < 3)
		return;

	for (i = 0; i < datalen; i++) {
		if (count >= (int)sizeof(tmpbuf) - 3)
			break;
		count += snprintf(tmpbuf + count, sizeof(tmpbuf) - count,
				  "%02X,", data[i]);
	}

	FTS_DEBUG("point buffer: %s", tmpbuf);
}

void fts_release_all_finger(void)
{
	struct input_dev *input_dev;
	u32 finger_count = 0, max_touches = 0;

	if (!fts_data || !fts_data->input_dev || !fts_data->pdata) {
		FTS_ERROR("fts_data not initialized");
		return;
	}

	input_dev = fts_data->input_dev;
	max_touches = fts_data->pdata->max_touch_number;

	mutex_lock(&fts_data->report_mutex);
	for (finger_count = 0; finger_count < max_touches; finger_count++) {
		input_mt_slot(input_dev, finger_count);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, false);
	}
	input_report_key(input_dev, BTN_TOUCH, 0);
	input_sync(input_dev);

	fts_data->touchs = 0;
	fts_data->key_state = 0;
	mutex_unlock(&fts_data->report_mutex);
}

/*****************************************************************************
 * Name: fts_input_report_key
 * Brief: process key events,need report key-event if key enable.
 *      if point's coordinate is in (x_dim-50,y_dim-50) ~ (x_dim+50,y_dim+50),
 *      need report it to key event.
 *      x_dim: parse from dts, means key x_coordinate, dimension:+-50
 *      y_dim: parse from dts, means key y_coordinate, dimension:+-50
 * Input:
 * Output:
 * Return: return 0 if it's key event, otherwise return error code
 *****************************************************************************/
static int fts_input_report_key(struct fts_ts_data *data, int index)
{
	int i;
	int x, y;
	int *x_dim;
	int *y_dim;

	if (!data || !data->pdata || !data->input_dev)
		return -EINVAL;

	if (!data->pdata->have_key)
		return -EINVAL;

	if (index < 0)
		return -EINVAL;

	x = data->events[index].x;
	y = data->events[index].y;
	x_dim = &data->pdata->key_x_coords[0];
	y_dim = &data->pdata->key_y_coords[0];

	for (i = 0; i < data->pdata->key_number; i++) {
		if ((x >= x_dim[i] - FTS_KEY_DIM && x <= x_dim[i] + FTS_KEY_DIM) &&
		    (y >= y_dim[i] - FTS_KEY_DIM && y <= y_dim[i] + FTS_KEY_DIM)) {
			bool down = EVENT_DOWN(data->events[index].flag);
			bool up = EVENT_UP(data->events[index].flag);
			unsigned int mask = 1u << i;

			if (down && !(data->key_state & mask)) {
				input_report_key(data->input_dev, data->pdata->keys[i], 1);
				data->key_state |= mask;
				FTS_DEBUG("Key%d(%d,%d) DOWN!", i, x, y);
			} else if (up && (data->key_state & mask)) {
				input_report_key(data->input_dev, data->pdata->keys[i], 0);
				data->key_state &= ~mask;
				FTS_DEBUG("Key%d(%d,%d) UP!", i, x, y);
			}
			return 0;
		}
	}

	return -EINVAL;
}

static int fts_input_report_b(struct fts_ts_data *data)
{
	int i;
	int uppoint = 0;
	unsigned int touchs = 0;
	bool va_reported = false;
	u32 max_touch_num;
	struct ts_event *events;

	if (!data || !data->pdata || !data->input_dev || !data->events)
		return -EINVAL;

	max_touch_num = data->pdata->max_touch_number;
	events = data->events;

	for (i = 0; i < data->touch_point; i++) {
		if (fts_input_report_key(data, i) == 0)
			continue;

		va_reported = true;
		input_mt_slot(data->input_dev, events[i].id);

		if (EVENT_DOWN(events[i].flag)) {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, true);

#if FTS_REPORT_PRESSURE_EN
			if (events[i].p <= 0)
				events[i].p = 0x3f;
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, events[i].p);
#endif
			if (events[i].area <= 0)
				events[i].area = 0x09;
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, events[i].area);
			input_report_abs(data->input_dev, ABS_MT_POSITION_X, events[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, events[i].y);

			touchs |= BIT(events[i].id);
			data->touchs |= BIT(events[i].id);

			if (data->log_level >= 2 ||
			    (data->log_level == 1 && events[i].flag == FTS_TOUCH_DOWN)) {
				FTS_DEBUG("[B]P%d(%d, %d)[p:%d,tm:%d] DOWN!",
					  events[i].id, events[i].x, events[i].y,
					  events[i].p, events[i].area);
			}
		} else {
			uppoint++;
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
			data->touchs &= ~BIT(events[i].id);
			if (data->log_level >= 1)
				FTS_DEBUG("[B]P%d UP!", events[i].id);
		}
	}

	if (unlikely(data->touchs ^ touchs)) {
		unsigned int diff = data->touchs ^ touchs;

		for (i = 0; i < (int)max_touch_num; i++) {
			if (diff & BIT(i)) {
				if (data->log_level >= 1)
					FTS_DEBUG("[B]P%d UP!", i);
				va_reported = true;
				input_mt_slot(data->input_dev, i);
				input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
			}
		}
	}

	data->touchs = touchs;

	if (va_reported) {
		if (EVENT_NO_DOWN(data) || !touchs) {
			if (data->log_level >= 1)
				FTS_DEBUG("[B]Points All Up!");
			input_report_key(data->input_dev, BTN_TOUCH, 0);
		} else {
			input_report_key(data->input_dev, BTN_TOUCH, 1);
		}
	}

	input_sync(data->input_dev);
	return 0;
}

static int fts_read_touchdata(struct fts_ts_data *data)
{
	int ret;
	u8 *buf;
	u8 *gbuf;

	if (!data || !data->pdata || !data->events || !data->point_buf)
		return -EINVAL;

	buf = data->point_buf;
	memset(buf, 0xFF, data->pnt_buf_size);
	buf[0] = 0x01;

	ret = fts_read(buf, 1, buf + 1, FTS_TOUCH_DATA_LEN - 1);
	if (ret < 0) {
		FTS_ERROR("read touchdata failed, ret:%d", ret);
		return ret;
	}

	if (data->log_level >= 3)
		fts_show_touch_buffer(buf, FTS_TOUCH_DATA_LEN);

	if (data->gesture_mode || data->aod_changed) {
		gbuf = buf + FTS_TOUCH_DATA_LEN;
		memset(gbuf, 0, FTS_GESTURE_DATA_LEN);

		ret = fts_read_reg(FTS_REG_GESTURE_EN, &gbuf[0]);
		if (ret < 0) {
			FTS_ERROR("read gesture en reg failed: %d", ret);
			goto out_touch;
		}

		if (gbuf[0] != ENABLE) {
			FTS_DEBUG("gesture not enabled in fw");
			goto out_touch;
		}

		gbuf[2] = FTS_REG_GESTURE_OUTPUT_ADDRESS;
		ret = fts_read(&gbuf[2], 1, &gbuf[2], FTS_GESTURE_DATA_LEN - 2);
		if (ret < 0) {
			FTS_ERROR("read gesture output data failed: %d", ret);
			goto out_touch;
		}

		if (fts_gesture_readdata(data, gbuf) == 0) {
			FTS_INFO("gesture handled in irq handler");
			return 1;
		}
	}

out_touch:
	return 0;
}

static int fts_read_parse_touchdata(struct fts_ts_data *data)
{
	int ret = 0;
	int i;
	u8 pointid;
	int base;
	struct ts_event *events;
	int max_touch_num;
	u8 *buf;
	size_t need_size;

	if (!data || !data->pdata || !data->events || !data->point_buf)
		return -EINVAL;

	events = data->events;
	max_touch_num = data->pdata->max_touch_number;
	buf = data->point_buf;

	if (data->pnt_buf_size <= FTS_TOUCH_POINT_NUM)
		return -EINVAL;

	ret = fts_read_touchdata(data);
	if (ret)
		return ret;

	data->point_num = buf[FTS_TOUCH_POINT_NUM] & 0x0F;
	data->touch_point = 0;
	if (data->ic_info.is_incell) {
		if (data->pnt_buf_size > 6 && data->point_num == 0x0F &&
		    buf[2] == 0xFF && buf[3] == 0xFF &&
		    buf[4] == 0xFF && buf[5] == 0xFF &&
		    buf[6] == 0xFF) {
			FTS_DEBUG("touch buff is 0xff, need recovery state");
			fts_release_all_finger();
			fts_tp_state_recovery(data);
			return -EIO;
		}
	}

	if (data->point_num > max_touch_num) {
		FTS_DEBUG("invalid point_num(%d)", data->point_num);
		return -EIO;
	}

	need_size = FTS_ONE_TCH_LEN * max_touch_num;
	if (data->pnt_buf_size < (size_t)FTS_TOUCH_POINT_NUM + 1 + need_size)
		need_size = (data->pnt_buf_size > (size_t)FTS_TOUCH_POINT_NUM + 1) ?
			    data->pnt_buf_size - (FTS_TOUCH_POINT_NUM + 1) : 0;

	for (i = 0; i < max_touch_num; i++) {
		base = FTS_ONE_TCH_LEN * i;
		if ((size_t)(FTS_TOUCH_ID_POS + base) >= data->pnt_buf_size)
			break;

		pointid = (buf[FTS_TOUCH_ID_POS + base]) >> 4;
		if (pointid >= FTS_MAX_ID)
			break;

		if (pointid >= (u8)max_touch_num) {
			FTS_DEBUG("ID(%d) beyond max_touch_number", pointid);
			return -EINVAL;
		}

		if ((size_t)(FTS_TOUCH_X_H_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_X_L_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_Y_H_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_Y_L_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_EVENT_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_AREA_POS + base) >= data->pnt_buf_size ||
		    (size_t)(FTS_TOUCH_PRE_POS + base) >= data->pnt_buf_size) {
			FTS_DEBUG("touch data truncated for index %d", i);
			return -EIO;
		}

		data->touch_point++;
		events[i].x = ((buf[FTS_TOUCH_X_H_POS + base] & 0x0F) << 8) |
			       (buf[FTS_TOUCH_X_L_POS + base] & 0xFF);
		events[i].y = ((buf[FTS_TOUCH_Y_H_POS + base] & 0x0F) << 8) |
			       (buf[FTS_TOUCH_Y_L_POS + base] & 0xFF);
		events[i].flag = buf[FTS_TOUCH_EVENT_POS + base] >> 6;
		events[i].id = buf[FTS_TOUCH_ID_POS + base] >> 4;
		events[i].area = buf[FTS_TOUCH_AREA_POS + base] >> 4;
		events[i].p = buf[FTS_TOUCH_PRE_POS + base];

		if (EVENT_DOWN(events[i].flag) && data->point_num == 0) {
			FTS_DEBUG("abnormal touch data from fw");
			return -EIO;
		}
	}

	if (data->touch_point == 0) {
		FTS_DEBUG("no touch point information");
		return -EIO;
	}

	return 0;
}

static void fts_irq_read_report(void)
{
	int ret;
	struct fts_ts_data *ts_data = fts_data;

	if (!ts_data)
		return;

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(1);
#endif

#if FTS_POINT_REPORT_CHECK_EN
	fts_prc_queue_work(ts_data);
#endif

	ret = fts_read_parse_touchdata(ts_data);
	if (ret == 0) {
		mutex_lock(&ts_data->report_mutex);
		fts_input_report_b(ts_data);
		mutex_unlock(&ts_data->report_mutex);
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(0);
#endif
}

/**
 * fts_irq_handler - Hard IRQ handler (atomic context)
 */
static irqreturn_t fts_irq_handler(int irq, void *data)
{
	struct fts_ts_data *ts_data = data;

	if (!ts_data)
		return IRQ_NONE;

	if (READ_ONCE(ts_data->irq_disabled))
		return IRQ_HANDLED;

	return IRQ_WAKE_THREAD;
}

/**
 * fts_irq_thread_handler - Threaded IRQ handler (can sleep)
 */
static irqreturn_t fts_irq_thread_handler(int irq, void *data)
{
	struct fts_ts_data *ts_data = data;
	int ret;

	if (!ts_data)
		return IRQ_HANDLED;

	if (READ_ONCE(ts_data->suspended)) {
		if (!ts_data->gesture_mode && !ts_data->aod_changed)
			return IRQ_HANDLED;
	}

#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
	if ((ts_data->gesture_mode || ts_data->aod_changed) &&
	    READ_ONCE(ts_data->pm_suspend)) {
		ret = wait_for_completion_timeout(&ts_data->pm_completion,
						  msecs_to_jiffies(FTS_TIMEOUT_COMERR_PM));
		if (!ret) {
			FTS_ERROR("Bus don't resume from pm, timeout");
			return IRQ_HANDLED;
		}
	}
#endif

	fts_irq_read_report();

	return IRQ_HANDLED;
}

static int fts_irq_registration(struct fts_ts_data *ts_data)
{
	int ret;
	struct fts_ts_platform_data *pdata;

	if (!ts_data || !ts_data->dev || !ts_data->pdata) {
		FTS_ERROR("invalid ts_data or pdata");
		return -EINVAL;
	}

	pdata = ts_data->pdata;
	ts_data->irq = gpio_to_irq(pdata->irq_gpio);
	if (ts_data->irq < 0) {
		FTS_ERROR("gpio_to_irq failed for gpio %d: %d", pdata->irq_gpio, ts_data->irq);
		return ts_data->irq;
	}

	pdata->irq_gpio_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	FTS_INFO("irq:%d, flag:0x%x", ts_data->irq, pdata->irq_gpio_flags);

	ret = devm_request_threaded_irq(ts_data->dev, ts_data->irq,
					fts_irq_handler, fts_irq_thread_handler,
					pdata->irq_gpio_flags,
					FTS_IRQ_NAME, ts_data);
	if (ret) {
		FTS_ERROR("devm_request_threaded_irq failed: %d", ret);
		return ret;
	}

	return 0;
}

static int fts_input_init(struct fts_ts_data *ts_data)
{
	int ret;
	int key_num;
	struct fts_ts_platform_data *pdata;
	struct input_dev *input_dev;

	if (!ts_data || !ts_data->pdata || !ts_data->dev) {
		FTS_ERROR("invalid ts_data/pdata/dev");
		return -EINVAL;
	}

	pdata = ts_data->pdata;
	input_dev = input_allocate_device();
	if (!input_dev) {
		FTS_ERROR("Failed to allocate memory for input device");
		return -ENOMEM;
	}

	input_dev->name = FTS_DRIVER_NAME;
	if (ts_data->bus_type == BUS_TYPE_I2C)
		input_dev->id.bustype = BUS_I2C;
	else
		input_dev->id.bustype = BUS_SPI;
	input_dev->dev.parent = ts_data->dev;
	input_set_drvdata(input_dev, ts_data);

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	if (pdata->have_key && pdata->key_number > 0) {
		FTS_DEBUG("set key capabilities");
		for (key_num = 0; key_num < pdata->key_number; key_num++)
			input_set_capability(input_dev, EV_KEY, pdata->keys[key_num]);
	}

	input_mt_init_slots(input_dev, pdata->max_touch_number, INPUT_MT_DIRECT);

	if (pdata->x_min <= pdata->x_max)
		input_set_abs_params(input_dev, ABS_MT_POSITION_X,
				     pdata->x_min, pdata->x_max, 0, 0);
	else
		FTS_ERROR("invalid x range: min(%d) > max(%d)", pdata->x_min, pdata->x_max);

	if (pdata->y_min <= pdata->y_max)
		input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
				     pdata->y_min, pdata->y_max, 0, 0);
	else
		FTS_ERROR("invalid y range: min(%d) > max(%d)", pdata->y_min, pdata->y_max);

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xFF, 0, 0);
#if FTS_REPORT_PRESSURE_EN
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 0xFF, 0, 0);
#endif

	ret = input_register_device(input_dev);
	if (ret) {
		FTS_ERROR("Input device registration failed: %d", ret);
		input_set_drvdata(input_dev, NULL);
		input_free_device(input_dev);
		return ret;
	}

	ts_data->input_dev = input_dev;
	return 0;
}

static void fts_input_exit(struct fts_ts_data *ts_data)
{
	struct input_dev *input_dev;

	if (!ts_data)
		return;

	input_dev = ts_data->input_dev;
	if (!input_dev)
		return;

	input_unregister_device(input_dev);
	ts_data->input_dev = NULL;
}

static int fts_report_buffer_init(struct fts_ts_data *ts_data)
{
	int point_num;
	size_t events_num;

	if (!ts_data || !ts_data->pdata) {
		FTS_ERROR("invalid ts_data/pdata");
		return -EINVAL;
	}

	point_num = ts_data->pdata->max_touch_number;
	if (point_num <= 0 || point_num > 32) {
		FTS_ERROR("invalid max_touch_number: %d", point_num);
		return -EINVAL;
	}

	ts_data->pnt_buf_size = (size_t)FTS_TOUCH_DATA_LEN + FTS_GESTURE_DATA_LEN;
	ts_data->point_buf = kzalloc(ts_data->pnt_buf_size + 1, GFP_KERNEL);
	if (!ts_data->point_buf) {
		FTS_ERROR("failed to alloc memory for point buf");
		return -ENOMEM;
	}

	events_num = (size_t)point_num * sizeof(struct ts_event);
	ts_data->events = kzalloc(events_num, GFP_KERNEL);
	if (!ts_data->events) {
		FTS_ERROR("failed to alloc memory for point events");
		kfree_safe(ts_data->point_buf);
		return -ENOMEM;
	}

	return 0;
}

static void fts_report_buffer_exit(struct fts_ts_data *ts_data)
{
	if (!ts_data)
		return;

	kfree_safe(ts_data->events);
	kfree_safe(ts_data->point_buf);
	ts_data->pnt_buf_size = 0;
}

#if FTS_POWER_SOURCE_CUST_EN
/*****************************************************************************
 * Power Control
 *****************************************************************************/
#if FTS_PINCTRL_EN
static int fts_pinctrl_init(struct fts_ts_data *ts)
{
	int ret = 0;

	ts->pinctrl = devm_pinctrl_get(ts->dev);
	if (IS_ERR_OR_NULL(ts->pinctrl)) {
		FTS_ERROR("Failed to get pinctrl, please check dts");
		ret = PTR_ERR(ts->pinctrl);
		ts->pinctrl = NULL;
		goto out;
	}

	ts->pins_active = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_active");
	if (IS_ERR_OR_NULL(ts->pins_active)) {
		FTS_ERROR("Pin state[active] not found");
		ret = PTR_ERR(ts->pins_active);
		goto err_put;
	}

	ts->pins_suspend = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_suspend");
	if (IS_ERR_OR_NULL(ts->pins_suspend)) {
		FTS_ERROR("Pin state[suspend] not found");
		ret = PTR_ERR(ts->pins_suspend);
		goto err_put;
	}

	ts->pins_release = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_release");
	if (IS_ERR_OR_NULL(ts->pins_release)) {
		FTS_ERROR("Pin state[release] not found");
		ret = PTR_ERR(ts->pins_release);
		goto err_put;
	}

	return ret;

err_put:
	if (ts->pinctrl) {
		devm_pinctrl_put(ts->pinctrl);
		ts->pinctrl = NULL;
	}
	ts->pins_active = NULL;
	ts->pins_suspend = NULL;
	ts->pins_release = NULL;
out:
	return ret;
}

static int fts_pinctrl_select_normal(struct fts_ts_data *ts)
{
	int ret = 0;

	if (ts->pinctrl && ts->pins_active) {
		ret = pinctrl_select_state(ts->pinctrl, ts->pins_active);
		if (ret < 0)
			FTS_ERROR("Set normal pin state error: %d", ret);
	}

	return ret;
}

static int fts_pinctrl_select_suspend(struct fts_ts_data *ts)
{
	int ret = 0;

	if (ts->pinctrl && ts->pins_suspend) {
		ret = pinctrl_select_state(ts->pinctrl, ts->pins_suspend);
		if (ret < 0)
			FTS_ERROR("Set suspend pin state error: %d", ret);
	}

	return ret;
}

static int fts_pinctrl_select_release(struct fts_ts_data *ts)
{
	int ret = 0;

	if (!ts->pinctrl)
		return 0;

	if (IS_ERR_OR_NULL(ts->pins_release)) {
		FTS_ERROR("pins_release invalid, releasing pinctrl to avoid race");
		devm_pinctrl_put(ts->pinctrl);
		ts->pinctrl = NULL;
		ts->pins_active = NULL;
		ts->pins_suspend = NULL;
		ts->pins_release = NULL;
		return -ENODEV;
	}

	ret = pinctrl_select_state(ts->pinctrl, ts->pins_release);
	if (ret < 0) {
		FTS_ERROR("Set gesture pin state error: %d", ret);
		devm_pinctrl_put(ts->pinctrl);
		ts->pinctrl = NULL;
		ts->pins_active = NULL;
		ts->pins_suspend = NULL;
		ts->pins_release = NULL;
	}

	return ret;
}
#endif /* FTS_PINCTRL_EN */

#define FTS_POWER_ON_DELAY_MS	5 //rail
#define FTS_RESET_PULSE_MS	1 //reset
#define FTS_RESET_RELEASE_DELAY_MS	10 //pulse deassert
static int fts_power_source_ctrl(struct fts_ts_data *ts_data, int enable)
{
	int ret = 0;

	if (!ts_data || !ts_data->pdata) {
		FTS_ERROR("invalid ts_data/pdata");
		return -EINVAL;
	}

	mutex_lock(&ts_data->bus_lock);

	if (enable) {
		if (ts_data->power_disabled) {
			FTS_DEBUG("power enable sequence start");

#if FTS_PINCTRL_EN
			fts_pinctrl_select_normal(ts_data);
#endif
			if (gpio_is_valid(ts_data->pdata->avdd_gpio)) {
				ret = gpio_direction_output(ts_data->pdata->avdd_gpio, 1);
				if (ret) {
					FTS_ERROR("[GPIO] set avdd_gpio=1 failed: %d", ret);
					goto err_power_on;
				}
			}

			if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
				ret = regulator_enable(ts_data->vcc_i2c);
				if (ret) {
					FTS_ERROR("enable vcc_i2c failed: %d", ret);
					if (gpio_is_valid(ts_data->pdata->avdd_gpio))
						gpio_direction_output(ts_data->pdata->avdd_gpio, 0);
					goto err_power_on;
				}
			}
			msleep(FTS_POWER_ON_DELAY_MS);

			if (gpio_is_valid(ts_data->pdata->reset_gpio)) {
				ret = gpio_direction_output(ts_data->pdata->reset_gpio, 1);
				if (ret) {
					FTS_ERROR("[GPIO] set reset_gpio=1 failed: %d", ret);
					if (!IS_ERR_OR_NULL(ts_data->vcc_i2c))
						regulator_disable(ts_data->vcc_i2c);
					if (gpio_is_valid(ts_data->pdata->avdd_gpio))
						gpio_direction_output(ts_data->pdata->avdd_gpio, 0);
					goto err_power_on;
				}
			}
			msleep(FTS_RESET_RELEASE_DELAY_MS);

			ts_data->power_disabled = false;
			FTS_DEBUG("power enable sequence done");
		}

		mutex_unlock(&ts_data->bus_lock);
		return 0;
	}

	if (!ts_data->power_disabled) {
		FTS_DEBUG("power disable sequence start");

#if FTS_PINCTRL_EN
		fts_pinctrl_select_suspend(ts_data);
#endif
		if (gpio_is_valid(ts_data->pdata->reset_gpio)) {
			ret = gpio_direction_output(ts_data->pdata->reset_gpio, 0);
			if (ret)
				FTS_ERROR("[GPIO] set reset_gpio=0 failed: %d", ret);
		}
		msleep(FTS_RESET_PULSE_MS);

		if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
			ret = regulator_disable(ts_data->vcc_i2c);
			if (ret)
				FTS_ERROR("disable vcc_i2c failed: %d", ret);
		}

		if (gpio_is_valid(ts_data->pdata->avdd_gpio)) {
			ret = gpio_direction_output(ts_data->pdata->avdd_gpio, 0);
			if (ret)
				FTS_ERROR("[GPIO] set avdd_gpio=0 failed: %d", ret);
		}

		ts_data->power_disabled = true;
		FTS_DEBUG("power disable sequence done");
	}

	mutex_unlock(&ts_data->bus_lock);
	return 0;

err_power_on:
	mutex_unlock(&ts_data->bus_lock);
	FTS_ERROR("power on failed: %d", ret);
	return ret;
}

static int fts_power_source_init(struct fts_ts_data *ts_data)
{
	int ret = 0;

	if (!ts_data || !ts_data->pdata)
		return -EINVAL;

	if (gpio_is_valid(ts_data->pdata->avdd_gpio)) {
		ret = gpio_request(ts_data->pdata->avdd_gpio, "fts_avdd_gpio");
		if (ret) {
			FTS_ERROR("[GPIO] request avdd gpio failed: %d", ret);
			return ret;
		}
		gpio_direction_output(ts_data->pdata->avdd_gpio, 0);
	}

	ts_data->vcc_i2c = regulator_get_optional(ts_data->dev, "vcc_i2c");
	if (IS_ERR(ts_data->vcc_i2c)) {
		FTS_INFO("no vcc_i2c regulator");
		ts_data->vcc_i2c = NULL;
	} else {
		if (regulator_count_voltages(ts_data->vcc_i2c) > 0) {
			ret = regulator_set_voltage(ts_data->vcc_i2c,
						    FTS_I2C_VTG_MIN_UV, FTS_I2C_VTG_MAX_UV);
			if (ret) {
				FTS_ERROR("vcc_i2c set_vtg failed: %d", ret);
				regulator_put(ts_data->vcc_i2c);
				ts_data->vcc_i2c = NULL;
			}
		}
	}

#if FTS_PINCTRL_EN
	fts_pinctrl_init(ts_data);
	fts_pinctrl_select_normal(ts_data);
#endif

	ts_data->power_disabled = true;
	ret = fts_power_source_ctrl(ts_data, 1);
	if (ret)
		FTS_ERROR("fail to enable power: %d", ret);

	return ret;
}

static int fts_power_source_exit(struct fts_ts_data *ts_data)
{
	if (!ts_data)
		return -EINVAL;

	fts_power_source_ctrl(ts_data, 0);

#if FTS_PINCTRL_EN
	fts_pinctrl_select_release(ts_data);
#endif

	if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
		regulator_put(ts_data->vcc_i2c);
		ts_data->vcc_i2c = NULL;
	}

	if (gpio_is_valid(ts_data->pdata->avdd_gpio))
		gpio_free(ts_data->pdata->avdd_gpio);

	return 0;
}

static int fts_power_source_suspend(struct fts_ts_data *ts_data)
{
	int ret;

	ret = fts_power_source_ctrl(ts_data, 0);
	if (ret < 0)
		FTS_ERROR("power off fail, ret=%d", ret);

	return ret;
}

static int fts_power_source_resume(struct fts_ts_data *ts_data)
{
	int ret;

	ret = fts_power_source_ctrl(ts_data, 1);
	if (ret < 0)
		FTS_ERROR("power on fail, ret=%d", ret);

	return ret;
}
#endif /* FTS_POWER_SOURCE_CUST_EN */

static int fts_gpio_configure(struct fts_ts_data *data)
{
	int ret = 0;

	if (!data || !data->pdata) {
		FTS_ERROR("invalid data/pdata");
		return -EINVAL;
	}

	/* request irq gpio */
	if (gpio_is_valid(data->pdata->irq_gpio)) {
		ret = gpio_request(data->pdata->irq_gpio, "fts_irq_gpio");
		if (ret) {
			FTS_ERROR("[GPIO] request irq gpio failed: %d", ret);
			goto err_irq_gpio_req;
		}

		ret = gpio_direction_input(data->pdata->irq_gpio);
		if (ret) {
			FTS_ERROR("[GPIO] set irq_gpio in failed: %d", ret);
			goto err_irq_gpio_dir;
		}
	}

	/* request reset gpio */
	if (gpio_is_valid(data->pdata->reset_gpio)) {
		ret = gpio_request(data->pdata->reset_gpio, "fts_reset_gpio");
		if (ret) {
			FTS_ERROR("[GPIO] request reset gpio failed: %d", ret);
			goto err_irq_gpio_dir;
		}

		ret = gpio_direction_output(data->pdata->reset_gpio, 1);
		if (ret) {
			FTS_ERROR("[GPIO] set reset_gpio=1 failed: %d", ret);
			goto err_reset_gpio_dir;
		}
	}

	return 0;

err_reset_gpio_dir:
	if (gpio_is_valid(data->pdata->reset_gpio))
		gpio_free(data->pdata->reset_gpio);
err_irq_gpio_dir:
	if (gpio_is_valid(data->pdata->irq_gpio))
		gpio_free(data->pdata->irq_gpio);
err_irq_gpio_req:
	return ret;
}

static int fts_get_dt_coords(struct device *dev, char *name,
			     struct fts_ts_platform_data *pdata)
{
	int ret = 0;
	u32 coords[FTS_COORDS_ARR_SIZE] = {0};
	struct property *prop;
	struct device_node *np = dev->of_node;
	int coords_size;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	coords_size = prop->length / sizeof(u32);
	if (coords_size != FTS_COORDS_ARR_SIZE) {
		FTS_ERROR("invalid:%s, size:%d", name, coords_size);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, name, coords, coords_size);
	if (ret < 0) {
		FTS_ERROR("Unable to read %s, please check dts", name);
		pdata->x_min = FTS_X_MIN_DISPLAY_DEFAULT;
		pdata->y_min = FTS_Y_MIN_DISPLAY_DEFAULT;
		pdata->x_max = FTS_X_MAX_DISPLAY_DEFAULT;
		pdata->y_max = FTS_Y_MAX_DISPLAY_DEFAULT;
		return -ENODATA;
	}
	pdata->x_min = coords[0];
	pdata->y_min = coords[1];
	pdata->x_max = coords[2];
	pdata->y_max = coords[3];

	FTS_INFO("display x(%d %d) y(%d %d)", pdata->x_min, pdata->x_max,
		 pdata->y_min, pdata->y_max);
	return 0;
}

static int fts_parse_dt(struct device *dev, struct fts_ts_platform_data *pdata)
{
	int ret = 0;
	struct device_node *np = dev->of_node;
	u32 temp_val = 0;

	ret = fts_get_dt_coords(dev, "focaltech,display-coords", pdata);
	if (ret < 0)
		FTS_ERROR("Unable to get display-coords");

	/* key */
	pdata->have_key = of_property_read_bool(np, "focaltech,have-key");
	if (pdata->have_key) {
		ret = of_property_read_u32(np, "focaltech,key-number",
					   &pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key number undefined!");

		if (pdata->key_number > FTS_MAX_KEYS)
			pdata->key_number = FTS_MAX_KEYS;

		ret = of_property_read_u32_array(np, "focaltech,keys",
						 pdata->keys, pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Keys undefined!");

		ret = of_property_read_u32_array(np, "focaltech,key-x-coords",
						 pdata->key_x_coords, pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key X Coords undefined!");

		ret = of_property_read_u32_array(np, "focaltech,key-y-coords",
						 pdata->key_y_coords, pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key Y Coords undefined!");

		FTS_INFO("VK Number:%d, key:(%d,%d,%d,%d)",
			 pdata->key_number,
			 pdata->keys[0], pdata->keys[1],
			 pdata->keys[2], pdata->keys[3]);
		FTS_INFO("coords:(%d,%d),(%d,%d),(%d,%d),(%d,%d)",
			 pdata->key_x_coords[0], pdata->key_y_coords[0],
			 pdata->key_x_coords[1], pdata->key_y_coords[1],
			 pdata->key_x_coords[2], pdata->key_y_coords[2],
			 pdata->key_x_coords[3], pdata->key_y_coords[3]);
	}

	/* reset, irq gpio info */
	pdata->reset_gpio = of_get_named_gpio_flags(np, "focaltech,reset-gpio",
						    0, &pdata->reset_gpio_flags);
	if (pdata->reset_gpio < 0)
		FTS_ERROR("Unable to get reset_gpio");

	pdata->irq_gpio = of_get_named_gpio_flags(np, "focaltech,irq-gpio",
						  0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		FTS_ERROR("Unable to get irq_gpio");

	pdata->avdd_gpio = of_get_named_gpio_flags(np, "focaltech,avdd-gpio",
						   0, &pdata->avdd_gpio_flags);
	if (pdata->avdd_gpio < 0)
		FTS_ERROR("Unable to get avdd_gpio");

	ret = of_property_read_u32(np, "focaltech,max-touch-number", &temp_val);
	if (ret < 0) {
		FTS_ERROR("Unable to get max-touch-number, please check dts");
		pdata->max_touch_number = FTS_MAX_POINTS_SUPPORT;
	} else {
		if (temp_val < 2)
			pdata->max_touch_number = 2; /* max_touch_number must >= 2 */
		else if (temp_val > FTS_MAX_POINTS_SUPPORT)
			pdata->max_touch_number = FTS_MAX_POINTS_SUPPORT;
		else
			pdata->max_touch_number = temp_val;
	}

	FTS_INFO("max touch number: %d, irq gpio: %d, reset gpio: %d",
		 pdata->max_touch_number, pdata->irq_gpio, pdata->reset_gpio);

	return 0;
}

static void fts_resume_work(struct work_struct *work)
{
	struct fts_ts_data *ts_data = container_of(work, struct fts_ts_data, resume_work);

	fts_ts_resume(ts_data->dev);
}

static void fts_suspend_work(struct work_struct *work)
{
	struct fts_ts_data *ts_data = container_of(work, struct fts_ts_data, suspend_work);

	fts_ts_suspend(ts_data->dev);
}

static int fts_check_dt(struct device_node *np)
{
	int i, count;
	struct device_node *node = NULL;
	struct drm_panel *panel = NULL;

	if (!np)
		return -EINVAL;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0) {
		FTS_ERROR("find drm_panel count(%d) fail", count);
		return -ENODEV;
	}

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		if (!node)
			continue;

		panel = of_drm_find_panel(node);
		of_node_put(node);
		node = NULL;

		if (IS_ERR(panel))
			continue;

		FTS_INFO("find drm_panel successfully");

		mutex_lock(&fts_panel_lock);
		active_panel = panel;
		mutex_unlock(&fts_panel_lock);

		return 0;
	}

	FTS_ERROR("no find drm_panel");
	return -ENODEV;
}

static int fts_check_default_tp(struct device_node *dt, const char *prop)
{
	const char **active_tp = NULL;
	int count, tmp, score = 0;
	const char *active;
	int ret, i;

	count = of_property_count_strings(dt->parent, prop);
	if (count <= 0 || count > 3)
		return -ENODEV;

	active_tp = kcalloc(count, sizeof(char *), GFP_KERNEL);
	if (!active_tp) {
		FTS_ERROR("FTS alloc failed");
		return -ENOMEM;
	}

	ret = of_property_read_string_array(dt->parent, prop,
					    active_tp, count);
	if (ret < 0) {
		FTS_ERROR("fail to read %s %d", prop, ret);
		ret = -ENODEV;
		goto out;
	}

	for (i = 0; i < count; i++) {
		active = active_tp[i];
		if (active) {
			tmp = of_device_is_compatible(dt, active);
			if (tmp > 0)
				score++;
		}
	}

	if (score <= 0) {
		FTS_INFO("not match this driver");
		ret = -ENODEV;
		goto out;
	}
	ret = 0;
out:
	kfree_safe(active_tp);
	return ret;
}

static int drm_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct drm_panel_notifier *evdata = data;
	int *blank = NULL;
	bool is_suspend = false;

	if (!fts_data) {
		FTS_ERROR("fts_data is null");
		return NOTIFY_DONE;
	}

	if (!evdata || !evdata->data) {
		FTS_ERROR("evdata or evdata->data is null");
		return NOTIFY_DONE;
	}

	if (event != DRM_PANEL_EVENT_BLANK) {
		FTS_DEBUG("event(%lu) skipped, not a complete blank event", event);
		return NOTIFY_DONE;
	}

	blank = evdata->data;
	FTS_DEBUG("DRM EVENT_BLANK received, blank: %d", *blank);

	switch (*blank) {
	case DRM_PANEL_BLANK_UNBLANK:
		is_suspend = false;
		break;
	case DRM_PANEL_BLANK_POWERDOWN:
	case DRM_PANEL_BLANK_LP1:
	case DRM_PANEL_BLANK_LP2:
		is_suspend = true;
		break;
	default:
		FTS_DEBUG("DRM BLANK(%d) not relevant, ignored", *blank);
		return NOTIFY_DONE;
	}

	if (is_suspend) {
		cancel_work_sync(&fts_data->resume_work);
		queue_work(fts_data->pm_workqueue, &fts_data->suspend_work);
	} else {
		cancel_work_sync(&fts_data->suspend_work);
		queue_work(fts_data->pm_workqueue, &fts_data->resume_work);
	}

	return NOTIFY_OK;
}

static int fts_ts_probe_entry(struct fts_ts_data *ts_data)
{
	int ret = 0;
	int pdata_size = sizeof(struct fts_ts_platform_data);

	FTS_INFO("%s", FTS_DRIVER_VERSION);
	ts_data->pdata = kzalloc(pdata_size, GFP_KERNEL);
	if (!ts_data->pdata) {
		FTS_ERROR("allocate memory for platform_data fail");
		return -ENOMEM;
	}

	if (ts_data->dev->of_node) {
		ret = fts_parse_dt(ts_data->dev, ts_data->pdata);
		if (ret) {
			FTS_ERROR("device-tree parse fail");
			ret = -ENODEV;
			goto err_pdata;
		}

		if (fts_check_dt(ts_data->dev->of_node)) {
			if (!fts_check_default_tp(ts_data->dev->of_node, "qcom,i2c-touch-active"))
				ret = -EPROBE_DEFER;
			else
				ret = -ENODEV;
			goto err_pdata;
		}
	} else {
		if (ts_data->dev->platform_data) {
			memcpy(ts_data->pdata, ts_data->dev->platform_data, pdata_size);
		} else {
			FTS_ERROR("platform_data is null");
			ret = -ENODEV;
			goto err_pdata;
		}
	}

	spin_lock_init(&ts_data->irq_lock);
	mutex_init(&ts_data->report_mutex);
	mutex_init(&ts_data->bus_lock);

	/* Init communication interface */
	ret = fts_bus_init(ts_data);
	if (ret) {
		FTS_ERROR("bus initialize fail");
		goto err_mutex_init;
	}

	ret = fts_input_init(ts_data);
	if (ret) {
		FTS_ERROR("input initialize fail");
		goto err_bus_init;
	}

	ret = fts_report_buffer_init(ts_data);
	if (ret) {
		FTS_ERROR("report buffer init fail");
		goto err_input_init;
	}

	ret = fts_gpio_configure(ts_data);
	if (ret) {
		FTS_ERROR("configure the gpios fail");
		goto err_report_buffer;
	}
	/* end Init communication interface */

	/* WQ Interface handler */
	ts_data->ts_workqueue = alloc_workqueue("fts_wq", WQ_MEM_RECLAIM, 1);
	if (!ts_data->ts_workqueue) {
		FTS_ERROR("create fts workqueue fail");
		ret = -ENOMEM;
		goto err_gpio_config;
	}

	/* WQ PM interface handler */
	INIT_WORK(&ts_data->resume_work, fts_resume_work);
	INIT_WORK(&ts_data->suspend_work, fts_suspend_work);
	ts_data->pm_workqueue = alloc_workqueue("fts_pm_wq", WQ_MEM_RECLAIM, 1);
	if (!ts_data->pm_workqueue) {
		FTS_ERROR("create fts pm workqueue fail");
		ret = -ENOMEM;
		goto err_wq_init;
	}

#if FTS_POWER_SOURCE_CUST_EN
	ret = fts_power_source_init(ts_data);
	if (ret) {
		FTS_ERROR("fail to get power(regulator)");
		goto err_pm_wq_init;
	}
#endif

	/* must called before IC validation */
#if (!FTS_CHIP_IDC)
	fts_reset_proc(200);
#endif

	ret = fts_get_ic_information(ts_data);
	if (ret) {
		FTS_ERROR("not focal IC, unregister driver");
		goto err_power_init;
	}

	ret = fts_create_apk_debug_channel(ts_data);
	if (ret) {
		FTS_ERROR("create apk debug node fail");
		goto err_power_init;
	}

	ret = fts_create_sysfs(ts_data);
	if (ret) {
		FTS_ERROR("create sysfs node fail");
		goto err_apk_debug;
	}

#if FTS_POINT_REPORT_CHECK_EN
	ret = fts_point_report_check_init(ts_data);
	if (ret) {
		FTS_ERROR("init point report check fail");
		goto err_create_sysfs;
	}
#endif

	ret = fts_gesture_init(ts_data);
	if (ret) {
		FTS_ERROR("init gesture fail");
		goto err_point_report;
	}

	ret = fts_ex_mode_init(ts_data);
	if (ret) {
		FTS_ERROR("init glove/cover/charger fail");
		goto err_gesture_init;
	}

#if FTS_ESDCHECK_EN
	ret = fts_esdcheck_init(ts_data);
	if (ret) {
		FTS_ERROR("init esd check fail");
		goto err_ex_mode_init;
	}
#endif

	ret = fts_irq_registration(ts_data);
	if (ret) {
		FTS_ERROR("request irq failed");
		goto err_esdcheck_init;
	}

	ret = fts_fwupg_init(ts_data);
	if (ret) {
		FTS_ERROR("init fw upgrade fail");
		goto err_irq_reg;
	}

	/* notifier */
	ts_data->drm_notif.notifier_call = drm_notifier_callback;
	mutex_lock(&fts_panel_lock);
	if (active_panel) {
		ret = drm_panel_notifier_register(active_panel, &ts_data->drm_notif);
		if (ret < 0) {
			mutex_unlock(&fts_panel_lock);
			FTS_ERROR("register notifier failed");
			goto err_fwupg_init;
		}
		FTS_INFO("register notifier success");
	} else {
		FTS_INFO("no active_panel registered at probe time");
	}
	mutex_unlock(&fts_panel_lock);

	/* PM related */
#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
	init_completion(&ts_data->pm_completion);
	WRITE_ONCE(ts_data->pm_suspend, false);
#endif

/* 2021.10.9 longcheer wugang add (xiaomi game mode) start */
	if (!ts_data->fts_tp_class) {
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
		ts_data->fts_tp_class = get_xiaomi_touch_class();
#endif
		if (ts_data->fts_tp_class) {
			ts_data->fts_touch_dev = device_create(ts_data->fts_tp_class,
							       NULL, 0x38, ts_data, "tp_dev");
			if (IS_ERR(ts_data->fts_touch_dev)) {
				FTS_ERROR("Failed to create device!");
				ret = PTR_ERR(ts_data->fts_touch_dev);
				goto err_device_create;
			}
			dev_set_drvdata(ts_data->fts_touch_dev, ts_data);
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
			memset(&xiaomi_touch_interfaces, 0x00,
			       sizeof(struct xiaomi_touch_interface));
			xiaomi_touch_interfaces.getModeValue = fts_get_mode_value;
			xiaomi_touch_interfaces.setModeValue = fts_set_cur_value;
			xiaomi_touch_interfaces.resetMode = fts_reset_mode;
			xiaomi_touch_interfaces.getModeAll = fts_get_mode_all;
			fts_init_touchmode_data();
			xiaomitouch_register_modedata(&xiaomi_touch_interfaces);
#endif
		}
	}
/* 2021.10.9 longcheer wugang add (xiaomi game mode) end */

	return 0;

err_device_create:
	mutex_lock(&fts_panel_lock);
	if (active_panel)
		drm_panel_notifier_unregister(active_panel, &ts_data->drm_notif);
	mutex_unlock(&fts_panel_lock);
err_fwupg_init:
	fts_fwupg_exit(ts_data);
err_irq_reg:
err_esdcheck_init:
#if FTS_ESDCHECK_EN
	fts_esdcheck_exit(ts_data);
err_ex_mode_init:
#endif
	fts_ex_mode_exit(ts_data);
err_gesture_init:
	fts_gesture_exit(ts_data);
err_point_report:
#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_exit(ts_data);
err_create_sysfs:
#endif
	fts_remove_sysfs(ts_data);
err_apk_debug:
	fts_release_apk_debug_channel(ts_data);
err_power_init:
#if FTS_POWER_SOURCE_CUST_EN
	fts_power_source_exit(ts_data);
err_pm_wq_init:
#endif
	if (ts_data->pm_workqueue) {
		cancel_work_sync(&ts_data->resume_work);
		cancel_work_sync(&ts_data->suspend_work);
		flush_workqueue(ts_data->pm_workqueue);
		destroy_workqueue(ts_data->pm_workqueue);
	}
err_wq_init:
	if (ts_data->ts_workqueue) {
		flush_workqueue(ts_data->ts_workqueue);
		destroy_workqueue(ts_data->ts_workqueue);
	}
err_gpio_config:
	if (gpio_is_valid(ts_data->pdata->reset_gpio))
		gpio_free(ts_data->pdata->reset_gpio);
	if (gpio_is_valid(ts_data->pdata->irq_gpio))
		gpio_free(ts_data->pdata->irq_gpio);
err_report_buffer:
	fts_report_buffer_exit(ts_data);
err_input_init:
	fts_input_exit(ts_data);
err_bus_init:
	fts_bus_exit(ts_data);
err_mutex_init:
	mutex_destroy(&ts_data->bus_lock);
	mutex_destroy(&ts_data->report_mutex);
err_pdata:
	kfree_safe(ts_data->pdata);

	return ret;
}

static int fts_ts_remove_entry(struct fts_ts_data *ts_data)
{
	if (!ts_data)
		return 0;

	if (ts_data->fts_touch_dev && !IS_ERR(ts_data->fts_touch_dev))
		device_destroy(ts_data->fts_tp_class, ts_data->fts_touch_dev->devt);

	mutex_lock(&fts_panel_lock);
	if (active_panel)
		drm_panel_notifier_unregister(active_panel, &ts_data->drm_notif);
	mutex_unlock(&fts_panel_lock);

	fts_fwupg_exit(ts_data);

	if (ts_data->pm_workqueue) {
		cancel_work_sync(&ts_data->resume_work);
		cancel_work_sync(&ts_data->suspend_work);
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_exit(ts_data);
#endif
	fts_ex_mode_exit(ts_data);
	fts_gesture_exit(ts_data);
#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_exit(ts_data);
#endif
	fts_remove_sysfs(ts_data);
	fts_release_apk_debug_channel(ts_data);

	if (ts_data->pm_workqueue) {
		flush_workqueue(ts_data->pm_workqueue);
		destroy_workqueue(ts_data->pm_workqueue);
	}

	if (ts_data->ts_workqueue) {
		flush_workqueue(ts_data->ts_workqueue);
		destroy_workqueue(ts_data->ts_workqueue);
	}

#if FTS_POWER_SOURCE_CUST_EN
	fts_power_source_exit(ts_data);
#endif
	if (gpio_is_valid(ts_data->pdata->reset_gpio))
		gpio_free(ts_data->pdata->reset_gpio);
	if (gpio_is_valid(ts_data->pdata->irq_gpio))
		gpio_free(ts_data->pdata->irq_gpio);

	fts_report_buffer_exit(ts_data);
	fts_input_exit(ts_data);
	fts_bus_exit(ts_data);

	mutex_destroy(&ts_data->bus_lock);
	mutex_destroy(&ts_data->report_mutex);
	kfree_safe(ts_data->pdata);
	kfree_safe(ts_data);
	fts_data = NULL;

	return 0;
}

static int fts_ts_suspend(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);
	int ret = 0;

	if (!ts_data) {
		FTS_ERROR("ts_data is null");
		return -ENODEV;
	}

	mutex_lock(&ts_data->bus_lock);
	if (READ_ONCE(ts_data->suspended)) {
		mutex_unlock(&ts_data->bus_lock);
		FTS_INFO("Already in suspend state");
		return 0;
	}

	if (ts_data->fw_loading) {
		mutex_unlock(&ts_data->bus_lock);
		FTS_INFO("fw upgrade in process, can't suspend");
		return 0;
	}
	mutex_unlock(&ts_data->bus_lock);

	FTS_DEBUG("Enter tp suspend");

	fts_irq_disable();
	fts_release_all_finger();

#if FTS_ESDCHECK_EN
	fts_esdcheck_suspend();
#endif

	if (ts_data->gesture_mode || ts_data->aod_changed) {
		if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
			ret = regulator_enable(ts_data->vcc_i2c);
			if (ret)
				FTS_ERROR("enable vcc_i2c regulator failed, ret=%d", ret);
		}

		ret = fts_gesture_suspend(ts_data);
		if (ret < 0) {
			FTS_ERROR("gesture suspend failed: %d; aborting ts suspend", ret);
			fts_irq_enable();
			return 0;
		}

		fts_irq_enable();
	} else {
		FTS_DEBUG("make TP enter into sleep mode");
		ret = fts_write_reg(FTS_REG_POWER_MODE, FTS_REG_POWER_MODE_SLEEP);
		if (ret < 0)
			FTS_ERROR("set TP to sleep mode fail, ret=%d", ret);

		if (!ts_data->ic_info.is_incell) {
#if FTS_POWER_SOURCE_CUST_EN
			ret = fts_power_source_suspend(ts_data);
			if (ret < 0)
				FTS_ERROR("power enter suspend fail: %d", ret);
#endif
		}
	}

	mutex_lock(&ts_data->bus_lock);
	WRITE_ONCE(ts_data->suspended, true);
	mutex_unlock(&ts_data->bus_lock);

	return 0;
}

static int fts_ts_resume(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);
	int ret;

	if (!ts_data) {
		FTS_ERROR("ts_data is null");
		return -ENODEV;
	}

	mutex_lock(&ts_data->bus_lock);
	if (!READ_ONCE(ts_data->suspended)) {
		mutex_unlock(&ts_data->bus_lock);
		FTS_INFO("Already in awake state");
		return 0;
	}
	mutex_unlock(&ts_data->bus_lock);

	FTS_DEBUG("Enter tp resume");
	fts_release_all_finger();

	if (!ts_data->ic_info.is_incell) {
#if FTS_POWER_SOURCE_CUST_EN
		ret = fts_power_source_resume(ts_data);
		if (ret < 0)
			FTS_ERROR("power resume failed: %d", ret);
#endif
		fts_reset_proc(200);
	}

	fts_tp_state_recovery(ts_data);

#if FTS_ESDCHECK_EN
	fts_esdcheck_resume();
#endif

	if (ts_data->gesture_mode || ts_data->aod_changed) {
		ret = fts_gesture_resume(ts_data);
		if (ret < 0)
			FTS_ERROR("gesture resume failed: %d", ret);
	}

	mutex_lock(&ts_data->bus_lock);
	WRITE_ONCE(ts_data->suspended, false);
	mutex_unlock(&ts_data->bus_lock);

	fts_irq_enable();

	return 0;
}

/* 2021.10.9 longcheer wugang add (xiaomi game mode) start */
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
static void fts_init_touchmode_data(void)
{
	FTS_DEBUG("ENTER");
	/* Touch Game Mode Switch */
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_MAX_VALUE] = 1;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_CUR_VALUE] = 0;

	/* Active Mode */
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_MAX_VALUE] = 1;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_CUR_VALUE] = 0;

	/* sensivity */
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_MAX_VALUE] = 50;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_MIN_VALUE] = 35;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_CUR_VALUE] = 0;

	/* Tolerance */
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_MAX_VALUE] = 255;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_MIN_VALUE] = 64;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_CUR_VALUE] = 0;

	/* edge filter orientation */
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_MAX_VALUE] = 3;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_CUR_VALUE] = 0;

	/* edge filter area */
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_MAX_VALUE] = 3;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_DEF_VALUE] = 2;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_CUR_VALUE] = 0;
}

static int fts_set_cur_value(int fts_mode, int fts_value)
{
	u8 fts_game_value[2] = {0};
	u8 temp_value = 0, reg_value = 0;
	int ret = 0;

	if (fts_mode >= Touch_Mode_NUM || fts_mode < 0) {
		FTS_ERROR("fts_mode is error: %d", fts_mode);
		return -EINVAL;
	}

	if (xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE] >
	    xiaomi_touch_interfaces.touch_mode[fts_mode][GET_MAX_VALUE]) {
		xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[fts_mode][GET_MAX_VALUE];
	} else if (xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE] <
	    xiaomi_touch_interfaces.touch_mode[fts_mode][GET_MIN_VALUE]) {
		xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[fts_mode][GET_MIN_VALUE];
	}

	xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE] = fts_value;

	FTS_DEBUG("fts_mode: %d, fts_value: %d", fts_mode, fts_value);

	switch (fts_mode) {
	case Touch_Game_Mode:
		break;
	case Touch_Active_MODE:
		break;
	case Touch_UP_THRESHOLD:
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][SET_CUR_VALUE];
		if (temp_value > 35 && temp_value <= 40)
			reg_value = 0x28;
		else if (temp_value > 40 && temp_value <= 45)
			reg_value = 0x25;
		else if (temp_value > 45 && temp_value <= 50)
			reg_value = 0x23;
		else
			reg_value = 0x25;

		fts_game_value[0] = 0x81;
		fts_game_value[1] = reg_value;
		break;
	case Touch_Tolerance:
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][SET_CUR_VALUE];
		if (temp_value > 64 && temp_value <= 128)
			reg_value = 0x70;
		else if (temp_value > 128 && temp_value <= 192)
			reg_value = 0x60;
		else if (temp_value > 192 && temp_value <= 255)
			reg_value = 0x40;
		else
			reg_value = 0x80;

		fts_game_value[0] = 0x85;
		fts_game_value[1] = reg_value;
		break;
	case Touch_Edge_Filter:
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][SET_CUR_VALUE];
		if (temp_value == 0)
			reg_value = 0x01;
		else if (temp_value == 1)
			reg_value = 0x02;
		else if (temp_value == 2)
			reg_value = 0x03;
		else if (temp_value == 3)
			reg_value = 0x04;

		fts_game_value[0] = 0x8D;
		fts_game_value[1] = reg_value;
		break;
	case Touch_Panel_Orientation:
		temp_value =
			xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][SET_CUR_VALUE];
		if (temp_value == 0 || temp_value == 2)
			reg_value = 0;
		else if (temp_value == 1)
			reg_value = 0x01;
		else if (temp_value == 3)
			reg_value = 0x02;

		fts_game_value[0] = 0x8C;
		fts_game_value[1] = reg_value;
		break;
	case Touch_Aod_Enable:
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Aod_Enable][SET_CUR_VALUE];
		if (temp_value == 1) {
			fts_data->aod_changed = ENABLE;
			FTS_DEBUG("temp_value: %d, aod_changed: %d",
				  temp_value, fts_data->aod_changed);
		} else {
			fts_data->aod_changed = DISABLE;
			FTS_DEBUG("temp_value: %d, aod_changed: %d",
				  temp_value, fts_data->aod_changed);
		}
		break;
	default:
		break;
	}

	FTS_INFO("mode: %d, value: %d, temp_value: %d, game_value: 0x%x,0x%x",
		 fts_mode, fts_value, temp_value,
		 fts_game_value[0], fts_game_value[1]);

	xiaomi_touch_interfaces.touch_mode[fts_mode][GET_CUR_VALUE] =
		xiaomi_touch_interfaces.touch_mode[fts_mode][SET_CUR_VALUE];

	if (xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][SET_CUR_VALUE]) {
		FTS_INFO("fts_game_value[0]: %d, fts_game_value[1]: %d",
			 fts_game_value[0], fts_game_value[1]);

		ret = fts_write_reg(fts_game_value[0], fts_game_value[1]);
		if (ret < 0)
			FTS_ERROR("change game mode fail, ret=%d", ret);
	}

	return 0;
}

static int fts_get_mode_value(int mode, int value_type)
{
	int value = -1;

	if (mode < Touch_Mode_NUM && mode >= 0)
		value = xiaomi_touch_interfaces.touch_mode[mode][value_type];
	else
		FTS_ERROR("don't support");

	return value;
}

static int fts_get_mode_all(int mode, int *value)
{
	if (mode < Touch_Mode_NUM && mode >= 0) {
		value[0] = xiaomi_touch_interfaces.touch_mode[mode][GET_CUR_VALUE];
		value[1] = xiaomi_touch_interfaces.touch_mode[mode][GET_DEF_VALUE];
		value[2] = xiaomi_touch_interfaces.touch_mode[mode][GET_MIN_VALUE];
		value[3] = xiaomi_touch_interfaces.touch_mode[mode][GET_MAX_VALUE];
		FTS_INFO("mode: %d, value: (%d,%d,%d,%d)",
			 mode, value[0], value[1], value[2], value[3]);
	} else {
		FTS_ERROR("don't support");
	}

	return 0;
}

static int fts_reset_mode(int mode)
{
	int i = 0;
	int ret = 0;

	FTS_DEBUG("fts_reset_game_mode enter");
	FTS_INFO("mode: %d", mode);

	if (mode < Touch_Mode_NUM && mode > 0) {
		xiaomi_touch_interfaces.touch_mode[mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[mode][GET_DEF_VALUE];
		fts_set_cur_value(mode, xiaomi_touch_interfaces.touch_mode[mode][SET_CUR_VALUE]);
	} else if (mode == 0) {
		for (i = Touch_Mode_NUM - 1; i >= 0; i--) {
			xiaomi_touch_interfaces.touch_mode[i][SET_CUR_VALUE] =
				xiaomi_touch_interfaces.touch_mode[i][GET_DEF_VALUE];
			fts_set_cur_value(i, xiaomi_touch_interfaces.touch_mode[i][SET_CUR_VALUE]);
		}
		ret = fts_write_reg(0x8D, 0x00);
		if (ret < 0)
			FTS_ERROR("set 8D to reset mode fail, ret=%d", ret);
	} else {
		FTS_ERROR("don't support");
	}

	return 0;
}
#endif
/* 2021.10.9 longcheer wugang add (xiaomi game mode) end */

#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
static int fts_pm_suspend(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);

	FTS_INFO("system enters into pm_suspend");
	WRITE_ONCE(ts_data->pm_suspend, true);
	smp_wmb(); /* pairs with READ_ONCE(pm_suspend) in fts_irq_thread_handler */
	reinit_completion(&ts_data->pm_completion);
	return 0;
}

static int fts_pm_resume(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);

	FTS_INFO("system resumes from pm_suspend");
	WRITE_ONCE(ts_data->pm_suspend, false);
	smp_wmb(); /* pairs with READ_ONCE(pm_suspend) in fts_irq_thread_handler */
	complete(&ts_data->pm_completion);
	return 0;
}

static const struct dev_pm_ops fts_dev_pm_ops = {
	.suspend = fts_pm_suspend,
	.resume = fts_pm_resume,
};
#endif

/*****************************************************************************
 * TP Driver
 *****************************************************************************/

static int fts_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;
	struct fts_ts_data *ts_data = NULL;

	FTS_INFO("Touch Screen (I2C BUS)driver probe");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		FTS_ERROR("I2C not supported");
		return -ENODEV;
	}

	ts_data = kzalloc(sizeof(*ts_data), GFP_KERNEL);
	if (!ts_data) {
		FTS_ERROR("allocate memory for fts_data fail");
		return -ENOMEM;
	}

	fts_data = ts_data;
	ts_data->client = client;
	ts_data->dev = &client->dev;
	ts_data->log_level = 0;
	ts_data->fw_is_running = 0;
	ts_data->bus_type = BUS_TYPE_I2C;
	i2c_set_clientdata(client, ts_data);

	ret = fts_ts_probe_entry(ts_data);
	if (ret) {
		FTS_ERROR("Touch Screen (I2C BUS)driver probe fail");
		kfree_safe(ts_data);
		fts_data = NULL;
		return ret;
	}

	FTS_INFO("Touch Screen (I2C BUS)driver probe successfully");
	return 0;
}

static int fts_ts_remove(struct i2c_client *client)
{
	return fts_ts_remove_entry(i2c_get_clientdata(client));
}

static const struct of_device_id fts_dt_match[] = {
	{ .compatible = "focaltech,fts", },
	{ },
};
MODULE_DEVICE_TABLE(of, fts_dt_match);

static const struct i2c_device_id fts_ts_id[] = {
	{ FTS_DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fts_ts_id);

static struct i2c_driver fts_ts_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= FTS_DRIVER_NAME,
#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
		.pm		= &fts_dev_pm_ops,
#endif
		.of_match_table	= of_match_ptr(fts_dt_match),
		.probe_type		= PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table	= fts_ts_id,
	.probe		= fts_ts_probe,
	.remove	= fts_ts_remove,
};

static int __init fts_ts_init(void)
{
	int ret = 0;

#ifdef CHECK_TOUCH_VENDOR
	/* check TP vendor */
	FTS_INFO("TP info: [Vendor]samsung [IC]ft3418");
#endif

	ret = i2c_add_driver(&fts_ts_driver);
	if (ret)
		FTS_ERROR("Focaltech touch screen driver init failed!");

	return ret;
}

static void __exit fts_ts_exit(void)
{
	i2c_del_driver(&fts_ts_driver);
}

late_initcall(fts_ts_init);
module_exit(fts_ts_exit);

MODULE_AUTHOR("FocalTech Driver Team");
MODULE_DESCRIPTION("FocalTech Touchscreen Driver");
MODULE_LICENSE("GPL v2");
