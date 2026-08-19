// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2012-2020, Focaltech Ltd. All rights reserved.
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
 * File Name: focaltech_gesture.c
 *
 * Author: Focaltech Driver Team
 *
 * Created: 2016-08-08
 *
 * Abstract:
 *
 * Reference:
 *
 *****************************************************************************/

/*****************************************************************************
 * 1.Included header files
 *****************************************************************************/
#include "focaltech_core.h"
#ifdef CONFIG_TP_COMMON
#include <linux/input/tp_common.h>
#endif

/******************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
#define KEY_GESTURE_DOUBLECLICK	KEY_WAKEUP
#define KEY_GESTURE_UP	KEY_UP
#define KEY_GESTURE_DOWN	KEY_DOWN
#define KEY_GESTURE_LEFT	KEY_LEFT
#define KEY_GESTURE_RIGHT	KEY_RIGHT
#define KEY_GESTURE_O	KEY_O
#define KEY_GESTURE_E	KEY_E
#define KEY_GESTURE_M	KEY_M
#define KEY_GESTURE_L	KEY_L
#define KEY_GESTURE_W	KEY_W
#define KEY_GESTURE_S	KEY_S
#define KEY_GESTURE_V	KEY_V
#define KEY_GESTURE_C	KEY_C
#define KEY_GESTURE_Z	KEY_Z
#define KEY_GESTURE_SINGLECLICK	KEY_GOTO

#define GESTURE_LEFT		0x20
#define GESTURE_RIGHT		0x21
#define GESTURE_UP		0x22
#define GESTURE_DOWN		0x23
#define GESTURE_DOUBLECLICK	0x24
#define GESTURE_O	0x30
#define GESTURE_W	0x31
#define GESTURE_M	0x32
#define GESTURE_E	0x33
#define GESTURE_L	0x44
#define GESTURE_S	0x46
#define GESTURE_V	0x54
#define GESTURE_Z	0x41
#define GESTURE_C	0x34
#define GESTURE_SINGLECLICK	0x25

#define nt_info(fmt, ...) printk(KERN_INFO "NetErnels: " fmt, ##__VA_ARGS__)

#define WAKEUP_OFF		4
#define WAKEUP_ON		5

#define GESTURE_RETRY_COUNT	5
#define GESTURE_RETRY_DELAY_US_MIN	1000
#define GESTURE_RETRY_DELAY_US_MAX	1500

/*****************************************************************************
 * Private enumerations, structures and unions using typedef
 *****************************************************************************/
/*
 * gesture_id   - which gesture was recognised
 * point_num    - number of trajectory points for this gesture
 * coordinate_x - all gesture point X coordinates
 * coordinate_y - all gesture point Y coordinates
 */
struct fts_gesture_st {
	u8 gesture_id;
	u8 point_num;
	u16 coordinate_x[FTS_GESTURE_POINTS_MAX];
	u16 coordinate_y[FTS_GESTURE_POINTS_MAX];
};

/*****************************************************************************
 * Static variables
 *****************************************************************************/
static struct fts_gesture_st fts_gesture_data;

/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/
extern bool is_dt2w_sensor;
extern bool is_st2w_sensor;

/*****************************************************************************
 * Static function prototypes
 *****************************************************************************/

/**
 * fts_gesture_write_mask - Program all gesture-enable mask registers.
 *
 * Enables the full gesture bitmask (0xD1-0xD2, 0xD5-0xD8).  Called both
 * during suspend entry and state recovery after a reset.
 *
 * Return: 0 on success, negative errno on I2C failure.
 */
static int fts_gesture_write_mask(void)
{
	int ret;

	ret = fts_write_reg(0xD1, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD1 fail: %d", ret);
		return ret;
	}

	ret = fts_write_reg(0xD2, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD2 fail: %d", ret);
		return ret;
	}

	ret = fts_write_reg(0xD5, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD5 fail: %d", ret);
		return ret;
	}

	ret = fts_write_reg(0xD6, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD6 fail: %d", ret);
		return ret;
	}

	ret = fts_write_reg(0xD7, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD7 fail: %d", ret);
		return ret;
	}

	ret = fts_write_reg(0xD8, 0xFF);
	if (ret < 0) {
		FTS_ERROR("write 0xD8 fail: %d", ret);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_TP_COMMON
static ssize_t double_tap_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct fts_ts_data *ts_data = fts_data;

	return sprintf(buf, "%d\n", ts_data->gesture_mode);
}

static ssize_t double_tap_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	struct fts_ts_data *ts_data = fts_data;
	int rc, val;

	rc = kstrtoint(buf, 10, &val);
	if (rc)
		return -EINVAL;

	ts_data->gesture_mode = !!val;
	return count;
}

static struct tp_common_ops double_tap_ops = {
	.show = double_tap_show,
	.store = double_tap_store
};
#endif

static ssize_t fts_gesture_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int ret;
	int count = 0;
	u8 val = 0;
	struct fts_ts_data *ts_data = fts_data;

	mutex_lock(&ts_data->input_dev->mutex);

	ret = fts_read_reg(FTS_REG_GESTURE_EN, &val);
	if (ret < 0)
		FTS_ERROR("read gesture en reg failed: %d", ret);

	count = snprintf(buf, PAGE_SIZE, "Gesture Mode:%s\n",
			 ts_data->gesture_mode ? "On" : "Off");
	count += snprintf(buf + count, PAGE_SIZE - count, "Reg(0xD0)=%d\n", val);

	mutex_unlock(&ts_data->input_dev->mutex);
	return count;
}

static ssize_t fts_gesture_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct fts_ts_data *ts_data = fts_data;

	mutex_lock(&ts_data->input_dev->mutex);
	if (FTS_SYSFS_ECHO_ON(buf)) {
		FTS_DEBUG("enable gesture");
		ts_data->gesture_mode = ENABLE;
	} else if (FTS_SYSFS_ECHO_OFF(buf)) {
		FTS_DEBUG("disable gesture");
		ts_data->gesture_mode = DISABLE;
	}
	mutex_unlock(&ts_data->input_dev->mutex);

	return count;
}

static ssize_t fts_gesture_buf_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int count = 0;
	int i = 0;
	struct input_dev *input_dev = fts_data->input_dev;
	struct fts_gesture_st *gesture = &fts_gesture_data;

	mutex_lock(&input_dev->mutex);
	count = snprintf(buf, PAGE_SIZE, "Gesture ID:%d\n",
			 gesture->gesture_id);
	count += snprintf(buf + count, PAGE_SIZE - count,
			  "Gesture PointNum:%d\n", gesture->point_num);
	count += snprintf(buf + count, PAGE_SIZE - count,
			  "Gesture Points Buffer:\n");

	for (i = 0; i < FTS_GESTURE_POINTS_MAX; i++) {
		count += snprintf(buf + count, PAGE_SIZE - count,
				  "%3d(%4d,%4d) ", i,
				  gesture->coordinate_x[i],
				  gesture->coordinate_y[i]);
		if ((i + 1) % 4 == 0)
			count += snprintf(buf + count, PAGE_SIZE - count, "\n");
	}
	count += snprintf(buf + count, PAGE_SIZE - count, "\n");
	mutex_unlock(&input_dev->mutex);

	return count;
}

static inline ssize_t double_tap_pressed_get(struct device *device,
                               struct device_attribute *attribute,
                               char *buffer)
{
       struct fts_ts_data *ts = dev_get_drvdata(device);
       return scnprintf(buffer, PAGE_SIZE, "%i\n", ts->double_tap_pressed);
}

static inline ssize_t single_tap_pressed_get(struct device *device,
                               struct device_attribute *attribute,
                               char *buffer)
{
       struct fts_ts_data *ts = dev_get_drvdata(device);
       return scnprintf(buffer, PAGE_SIZE, "%i\n", ts->single_tap_pressed);
}

/*
 * sysfs gesture nodes:
 *   fts_gesture_mode : read/write gesture enable state
 *   fts_gesture_buf  : read-only trajectory point dump
 */
static DEVICE_ATTR_RW(fts_gesture_mode);
static DEVICE_ATTR_RO(fts_gesture_buf);
static DEVICE_ATTR(double_tap_pressed, S_IRUGO,
                   double_tap_pressed_get, NULL);
static DEVICE_ATTR(single_tap_pressed, S_IRUGO,
                   single_tap_pressed_get, NULL);


static struct attribute *fts_gesture_mode_attrs[] = {
	&dev_attr_fts_gesture_mode.attr,
	&dev_attr_fts_gesture_buf.attr,
	&dev_attr_double_tap_pressed.attr,
	&dev_attr_single_tap_pressed.attr,
	NULL,
};

static struct attribute_group fts_gesture_group = {
	.attrs = fts_gesture_mode_attrs,
};

static int fts_create_gesture_sysfs(struct device *dev)
{
	int ret;

	ret = sysfs_create_group(&dev->kobj, &fts_gesture_group);
	if (ret)
		FTS_ERROR("gesture sysfs node create fail: %d", ret);

	return ret;
}

/**
 * fts_gesture_report - Translate a firmware gesture ID to an input key event.
 * @input_dev:  the driver's input device
 * @gesture_id: gesture code read from the IC
 */
static void fts_gesture_report(struct input_dev *input_dev, int gesture_id)
{
	int gesture;

	if (is_dt2w_sensor) {
		fts_data->double_tap_pressed = (gesture_id == GESTURE_DOUBLECLICK) ? 1 : 0;
		sysfs_notify(&fts_data->client->dev.kobj, NULL, "double_tap_pressed");
	}

	if (is_st2w_sensor) {
		fts_data->single_tap_pressed = (gesture_id == GESTURE_SINGLECLICK) ? 1 : 0;
		sysfs_notify(&fts_data->client->dev.kobj, NULL, "single_tap_pressed");
	}

	FTS_DEBUG("gesture_id:0x%x", gesture_id);

	switch (gesture_id) {
	case GESTURE_LEFT:
		gesture = KEY_GESTURE_LEFT;
		break;
	case GESTURE_RIGHT:
		gesture = KEY_GESTURE_RIGHT;
		break;
	case GESTURE_UP:
		gesture = KEY_GESTURE_UP;
		break;
	case GESTURE_DOUBLECLICK:
		if (!is_dt2w_sensor)
			gesture = KEY_GESTURE_DOUBLECLICK;
		break;
	case GESTURE_DOWN:
		gesture = KEY_GESTURE_DOWN;
		break;
	case GESTURE_O:
		gesture = KEY_GESTURE_O;
		break;
	case GESTURE_W:
		gesture = KEY_GESTURE_W;
		break;
	case GESTURE_M:
		gesture = KEY_GESTURE_M;
		break;
	case GESTURE_E:
		gesture = KEY_GESTURE_E;
		break;
	case GESTURE_L:
		gesture = KEY_GESTURE_L;
		break;
	case GESTURE_S:
		gesture = KEY_GESTURE_S;
		break;
	case GESTURE_V:
		gesture = KEY_GESTURE_V;
		break;
	case GESTURE_Z:
		gesture = KEY_GESTURE_Z;
		break;
	case GESTURE_C:
		gesture = KEY_GESTURE_C;
		break;
	case GESTURE_SINGLECLICK:
		gesture = KEY_GESTURE_SINGLECLICK;
		break;
	default:
		FTS_DEBUG("unknown gesture_id:0x%x, skip", gesture_id);
		gesture = 1;
		break;
	}

	/* report event key */
	if (gesture != 1) {
		FTS_DEBUG("Gesture Code=%d", gesture);
		input_report_key(input_dev, gesture, 1);
		input_sync(input_dev);
		input_report_key(input_dev, gesture, 0);
		input_sync(input_dev);
	}
}

/*****************************************************************************
 * Name: fts_gesture_readdata
 * Brief: Read gesture data from the IC and report to input subsystem.
 *   Called from the IRQ thread handler on every interrupt while in suspend
 *   with gesture or AOD mode active.
 *
 * Input:  ts_data - driver state
 *         data    - pre-read gesture buffer (non-NULL)
 * Return: 0  - gesture was valid and reported
 *         1  - not in gesture-eligible state, or gesture not enabled in FW
 *         -E - I/O or parameter error
 *****************************************************************************/
int fts_gesture_readdata(struct fts_ts_data *ts_data, u8 *data)
{
	int i;
	int index;
	u8 buf[FTS_GESTURE_DATA_LEN] = { 0 };
	struct input_dev *input_dev = ts_data->input_dev;
	struct fts_gesture_st *gesture = &fts_gesture_data;

	if (!READ_ONCE(ts_data->suspended))
		return 1;

	if (!data) {
		FTS_ERROR("gesture data buffer is null");
		return -EINVAL;
	}

	memcpy(buf, data, FTS_GESTURE_DATA_LEN);
	if (buf[0] != ENABLE) {
		FTS_DEBUG("gesture not enabled in fw, skip");
		return 1;
	}

	memset(gesture->coordinate_x, 0,
	       FTS_GESTURE_POINTS_MAX * sizeof(u16));
	memset(gesture->coordinate_y, 0,
	       FTS_GESTURE_POINTS_MAX * sizeof(u16));

	gesture->gesture_id = buf[2];
	gesture->point_num  = buf[3];
	FTS_DEBUG("gesture_id=%d, point_num=%d",
		  gesture->gesture_id, gesture->point_num);

	for (i = 0; i < FTS_GESTURE_POINTS_MAX; i++) {
		index = 4 * i + 4;
		gesture->coordinate_x[i] =
			(u16)(((buf[0 + index] & 0x0F) << 8) + buf[1 + index]);
		gesture->coordinate_y[i] =
			(u16)(((buf[2 + index] & 0x0F) << 8) + buf[3 + index]);
	}

	fts_gesture_report(input_dev, gesture->gesture_id);
	return 0;
}

/**
 * fts_gesture_recovery - Re-arm gesture mode after an unexpected IC reset.
 *
 * Called from fts_tp_state_recovery().  Only acts while the device is
 * suspended with gesture_mode active.
 */
void fts_gesture_recovery(struct fts_ts_data *ts_data)
{
	int ret;

	if (!READ_ONCE(ts_data->suspended) ||
	    (!ts_data->gesture_mode && !ts_data->aod_changed))
		return;

	FTS_DEBUG("gesture recovery...");

	ret = fts_gesture_write_mask();
	if (ret < 0) {
		FTS_ERROR("gesture recovery: write mask fail: %d", ret);
		return;
	}

	ret = fts_write_reg(FTS_REG_GESTURE_EN, ENABLE);
	if (ret < 0)
		FTS_ERROR("gesture recovery: write en fail: %d", ret);
}

/**
 * fts_gesture_suspend - Put the IC into gesture-wakeup mode for system sleep.
 *
 * Writes the gesture mask and enables gesture mode, then polls until the IC
 * confirms the register write.  Retries up to GESTURE_RETRY_COUNT times.
 *
 * Return: 0 on success, -EIO if the IC does not acknowledge within the retry
 *         budget, or a negative errno on I2C failure.
 */
int fts_gesture_suspend(struct fts_ts_data *ts_data)
{
	int i;
	int ret;
	u8 state = 0;

	FTS_FUNC_ENTER();

	if (enable_irq_wake(ts_data->irq))
		FTS_DEBUG("enable_irq_wake(irq:%d) fail", ts_data->irq);

	for (i = 0; i < GESTURE_RETRY_COUNT; i++) {
		ret = fts_gesture_write_mask();
		if (ret < 0) {
			FTS_ERROR("write gesture mask fail: %d", ret);
			goto out;
		}

		ret = fts_write_reg(FTS_REG_GESTURE_EN, ENABLE);
		if (ret < 0) {
			FTS_ERROR("write gesture en fail: %d", ret);
			goto out;
		}

		usleep_range(GESTURE_RETRY_DELAY_US_MIN,
			     GESTURE_RETRY_DELAY_US_MAX);

		ret = fts_read_reg(FTS_REG_GESTURE_EN, &state);
		if (ret < 0) {
			FTS_ERROR("read gesture en fail: %d", ret);
			goto out;
		}

		if (state == ENABLE)
			break;
	}

	if (i >= GESTURE_RETRY_COUNT) {
		FTS_ERROR("IC did not enter gesture mode after %d tries, state:0x%x",
			  GESTURE_RETRY_COUNT, state);
		ret = -EIO;
		goto out;
	}

	FTS_INFO("gesture suspend: successfully");
	ret = 0;

out:
	FTS_FUNC_EXIT();
	return ret;
}

/**
 * fts_gesture_resume - Take the IC out of gesture-wakeup mode on system resume.
 *
 * Disables gesture mode and polls until the IC confirms.  Retries up to
 * GESTURE_RETRY_COUNT times.
 *
 * Return: 0 on success, -EIO if the IC does not acknowledge within the retry
 *         budget, or a negative errno on I2C failure.
 */
int fts_gesture_resume(struct fts_ts_data *ts_data)
{
	int i;
	int ret;
	u8 state = 0xFF;

	FTS_FUNC_ENTER();

	for (i = 0; i < GESTURE_RETRY_COUNT; i++) {
		ret = fts_write_reg(FTS_REG_GESTURE_EN, DISABLE);
		if (ret < 0) {
			FTS_ERROR("write gesture en fail: %d", ret);
			goto out;
		}

		usleep_range(GESTURE_RETRY_DELAY_US_MIN,
			     GESTURE_RETRY_DELAY_US_MAX);

		ret = fts_read_reg(FTS_REG_GESTURE_EN, &state);
		if (ret < 0) {
			FTS_ERROR("read gesture en fail: %d", ret);
			goto out;
		}

		if (state == DISABLE)
			break;
	}

	if (i >= GESTURE_RETRY_COUNT) {
		FTS_ERROR("IC did not exit gesture mode after %d tries, state:0x%x",
			  GESTURE_RETRY_COUNT, state);
		ret = -EIO;
		goto out;
	}

	if (disable_irq_wake(ts_data->irq))
		FTS_DEBUG("disable_irq_wake(irq:%d) fail", ts_data->irq);

	FTS_INFO("gesture resume: successfully");
	ret = 0;

out:
	FTS_FUNC_EXIT();
	return ret;
}

/**
 * fts_gesture_switch - input_dev->event() callback for gesture enable/disable.
 *
 * Userspace (e.g. power-manager) sends EV_SYN/SYN_CONFIG with WAKEUP_ON or
 * WAKEUP_OFF to toggle gesture-wakeup at runtime.
 */
int fts_gesture_switch(struct input_dev *dev,
		       unsigned int type, unsigned int code, int value)
{
	struct fts_ts_data *ts_data = fts_data;

	FTS_DEBUG("type=%u, code=%u, value=%d", type, code, value);

	if (type == EV_SYN && code == SYN_CONFIG) {
		if (value == WAKEUP_OFF) {
			ts_data->gesture_mode = DISABLE;
			ts_data->aod_changed = DISABLE;
		} else if (value == WAKEUP_ON) {
			ts_data->gesture_mode = ENABLE;
			ts_data->aod_changed = ENABLE;
		}
	}

	return 0;
}

int fts_gesture_init(struct fts_ts_data *ts_data)
{
	struct input_dev *input_dev = ts_data->input_dev;

	FTS_FUNC_ENTER();

	input_dev->event = fts_gesture_switch;
	if (!is_st2w_sensor) {
		nt_info("Legacy ST2W detected! Setting capability for it...");
		input_set_capability(input_dev, EV_KEY, KEY_GOTO);
	}
	input_set_capability(input_dev, EV_KEY, KEY_SLEEP);
	input_set_capability(input_dev, EV_KEY, KEY_POWER);
	if (!is_dt2w_sensor) {
		nt_info("Legacy DT2W detected! Setting capability for it...");
		input_set_capability(input_dev, EV_KEY, KEY_GESTURE_DOUBLECLICK);
	}
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_UP);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_DOWN);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_LEFT);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_RIGHT);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_O);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_E);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_M);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_L);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_W);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_S);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_V);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_Z);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_C);

	if (!is_st2w_sensor) {
		nt_info("Legacy ST2W detected! Setting key bit for it...");
		__set_bit(KEY_GOTO, input_dev->keybit);
	}
	__set_bit(KEY_SLEEP, input_dev->keybit);
	__set_bit(KEY_GESTURE_RIGHT, input_dev->keybit);
	__set_bit(KEY_GESTURE_LEFT, input_dev->keybit);
	__set_bit(KEY_GESTURE_UP, input_dev->keybit);
	__set_bit(KEY_GESTURE_DOWN, input_dev->keybit);
	if (!is_dt2w_sensor) {
		nt_info("Legacy DT2W detected! Setting key bit for it...");
		__set_bit(KEY_GESTURE_DOUBLECLICK, input_dev->keybit);
	}
	__set_bit(KEY_GESTURE_O, input_dev->keybit);
	__set_bit(KEY_GESTURE_E, input_dev->keybit);
	__set_bit(KEY_GESTURE_M, input_dev->keybit);
	__set_bit(KEY_GESTURE_W, input_dev->keybit);
	__set_bit(KEY_GESTURE_L, input_dev->keybit);
	__set_bit(KEY_GESTURE_S, input_dev->keybit);
	__set_bit(KEY_GESTURE_V, input_dev->keybit);
	__set_bit(KEY_GESTURE_C, input_dev->keybit);
	__set_bit(KEY_GESTURE_Z, input_dev->keybit);

	fts_create_gesture_sysfs(ts_data->dev);

#ifdef CONFIG_TP_COMMON
	tp_common_set_ops(TP_FEATURE_DOUBLE_TAP, &double_tap_ops);
#endif

	memset(&fts_gesture_data, 0, sizeof(struct fts_gesture_st));
	ts_data->gesture_mode = FTS_GESTURE_EN;

	FTS_FUNC_EXIT();
	return 0;
}

int fts_gesture_exit(struct fts_ts_data *ts_data)
{
	FTS_FUNC_ENTER();
#ifdef CONFIG_TP_COMMON
	tp_common_remove_ops(TP_FEATURE_DOUBLE_TAP);
#endif
	sysfs_remove_group(&ts_data->dev->kobj, &fts_gesture_group);
	FTS_FUNC_EXIT();
	return 0;
}
