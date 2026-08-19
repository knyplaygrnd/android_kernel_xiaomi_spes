// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "[qpnp_vibrator_pwm]: %s: " fmt, __func__

#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>

#define QPNP_VIB_EFFECT1_MS	35
#define QPNP_VIB_EFFECT2_MS	50
#define QPNP_VIB_EFFECT3_MS	60

#define QPNP_VIB_AVG_PLAY_MS	1000
#define QPNP_VIB_MIN_PLAY_MS	50
#define QPNP_VIB_MAX_PLAY_MS	15000

/*
 * PWM period — fixed at 20 kHz for all effects.
 */
#define VIB_PWM_PERIOD_NS	50000ULL

/*
 * Overdrive: high duty cycle at startup to achieve fast spin-up (<30ms),
 * replacing the slow ~100ms ramp-up that occurs without overdrive.
 * 80% -> V_eff ~2.40V >> V_start ~2.3V, aggressive spin-up.
 */
#define VIB_OVERDRIVE_DUTY_PCT	80

/*
 * Brake: very low duty cycle just before full stop.
 * Accelerates spin-down from ~100ms to ~15ms, making
 * each effect feel sharp and clean rather than trailing off.
 */
#define VIB_BRAKE_DUTY_PCT	5
#define VIB_BRAKE_MS		15

/*
 * Per-effect parameter table.
 *
 * Timeline per effect:
 *   Effect 1: 20ms OD + 35ms sustain + 15ms brake = ~70ms
 *   Effect 2: 22ms OD + 50ms sustain + 15ms brake = ~87ms
 *   Effect 3: 25ms OD + 60ms sustain + 15ms brake = ~100ms
 *   Default : 18ms OD + vib_play_ms + 15ms brake
 *
 * sustain_duty is chosen so V_eff >= V_start (~2.3V @ 3V supply):
 *   Effect 1: 72% - intentionally lower.
 *   Effect 2: 78% - medium intensity, V_eff ~2.34V > V_start.
 *   Effect 3: 85% - strong, V_eff ~2.64V.
 *   Default : 77% - safe margin above V_start (2310mV > 2300mV).
 */
struct vib_effect_param {
	u32 overdrive_ms;
	u8 sustain_duty_pct;
};

static const struct vib_effect_param vib_effects[] = {
	[0] = { 18, 77 }, /* default : OD 18ms, sustain 77% */
	[1] = { 20, 72 }, /* effect 1: OD 20ms, sustain 72% */
	[2] = { 22, 78 }, /* effect 2: OD 22ms, sustain 78% */
	[3] = { 25, 85 }, /* effect 3: OD 25ms, sustain 85% */
};

struct vib_pwm_chip {
	struct device *dev;
	struct led_classdev cdev;
	struct mutex lock;
	struct hrtimer stop_timer;
	struct work_struct vib_work;

	u16 base;
	int state;
	int effect_idx;
	u64 vib_play_ms;
	bool vib_enabled;

	struct pwm_device *pwm_dev;
	struct pwm_device **pwm_devs;
	int pwm_count;
	int pwm_last_valid;

	u32 en_gpio;
	u32 en_gpio_flags;

	int pwm_nums;
	const char *label;
	u8 id;
};

static int vib_apply_duty(struct vib_pwm_chip *chip,
			  struct pwm_state *pstate, u8 duty_pct)
{
	u64 duty_ns = pstate->period * duty_pct / 100;

	if (duty_ns >= pstate->period)
		duty_ns = pstate->period - 1;

	pstate->duty_cycle = duty_ns;

	return pwm_apply_state(chip->pwm_dev, pstate);
}

static int qpnp_vibrator_play_on(struct vib_pwm_chip *chip)
{
	struct pwm_state pstate;
	const struct vib_effect_param *eff;
	int idx;
	int err = 0;

	if (!chip || !chip->pwm_dev)
		return -ENODEV;

	idx = chip->effect_idx;
	if (idx < 0 || idx >= ARRAY_SIZE(vib_effects))
		idx = 0;

	eff = &vib_effects[idx];

	pwm_get_state(chip->pwm_dev, &pstate);
	pstate.period = VIB_PWM_PERIOD_NS;
	pstate.polarity = PWM_POLARITY_NORMAL;
	pstate.enabled = true;

	if (gpio_is_valid(chip->en_gpio)) {
		err = gpio_direction_output(chip->en_gpio, 1);
		if (err)
			pr_err("set (en_gpio,1) failed: %d\n", err);
	}

	/* Phase 1: Overdrive.
	 * Duty cycle 80% for fast spin-up.
	 * V_eff ~2.40V >= V_start ~2.3V.
	 */
	if (eff->overdrive_ms > 0) {
		err = vib_apply_duty(chip, &pstate, VIB_OVERDRIVE_DUTY_PCT);
		if (err) {
			pr_err("pwm_apply_state(overdrive) failed: %d\n", err);
			chip->vib_enabled = false;
			return err;
		}
		pr_info("overdrive: effect=%d duty=%u%% duration=%ums\n",
			idx, VIB_OVERDRIVE_DUTY_PCT, eff->overdrive_ms);
		usleep_range(eff->overdrive_ms * 1000, eff->overdrive_ms * 1000 + 500);
	}

	/* Phase 2: Sustain.
	 * Drop to duty cycle target.
	 * hrtimer runs from this point on for EFFECT_MS.
	 * then play_off() called for brake -> off sequence.
	 */
	err = vib_apply_duty(chip, &pstate, eff->sustain_duty_pct);
	if (err) {
		pr_err("pwm_apply_state(sustain) failed: %d\n", err);
		chip->vib_enabled = false;
		return err;
	}

	pr_info("sustain: effect=%d duty=%u%% period=%llu\n",
		idx, eff->sustain_duty_pct, pstate.period);

	chip->vib_enabled = true;
	return 0;
}

static int qpnp_vibrator_play_off(struct vib_pwm_chip *chip)
{
	struct pwm_state pstate;
	int err;

	if (!chip || !chip->pwm_dev)
		return -ENODEV;

	pwm_get_state(chip->pwm_dev, &pstate);

	if (pstate.period == 0 || pstate.period > 1000000000ULL)
		pstate.period = VIB_PWM_PERIOD_NS;

	/* Phase 3: Brake.
	 * 5% duty for 15ms.
	 */
	pstate.enabled = true;
	err = vib_apply_duty(chip, &pstate, VIB_BRAKE_DUTY_PCT);
	if (err)
		pr_err("pwm_apply_state(brake) failed: %d\n", err);
	else
		pr_info("brake: duty=%u%% duration=%ums\n",
			VIB_BRAKE_DUTY_PCT, VIB_BRAKE_MS);

	usleep_range(VIB_BRAKE_MS * 1000, VIB_BRAKE_MS * 1000 + 500);

	/* Phase 4: Full stop.*/
	pstate.enabled = false;

	pr_info("apply PWM off: period=%llu\n", pstate.period);

	err = pwm_apply_state(chip->pwm_dev, &pstate);
	if (err) {
		pr_err("pwm_apply_state(off) failed: %d\n", err);
		return err;
	}

	if (gpio_is_valid(chip->en_gpio)) {
		err = gpio_direction_output(chip->en_gpio, 0);
		if (err)
			pr_err("set (en_gpio,0) failed: %d\n", err);
	}

	chip->vib_enabled = false;
	return 0;
}

static void qpnp_vib_work(struct work_struct *work)
{
	struct vib_pwm_chip *chip = container_of(work, struct vib_pwm_chip, vib_work);
	int ret = 0;
	int en_time = 0;

	mutex_lock(&chip->lock);
	if (chip->state) {
		if (!chip->vib_enabled)
			ret = qpnp_vibrator_play_on(chip);

		if (ret == 0) {
			if (chip->effect_idx == 1)
				en_time = QPNP_VIB_EFFECT1_MS;
			else if (chip->effect_idx == 2)
				en_time = QPNP_VIB_EFFECT2_MS;
			else if (chip->effect_idx == 3)
				en_time = QPNP_VIB_EFFECT3_MS;
			else
				en_time = chip->vib_play_ms;

			pr_info("vib start: effect=%d, duration=%dms\n",
				chip->effect_idx, en_time);
			hrtimer_start(&chip->stop_timer,
				      ms_to_ktime(en_time),
				      HRTIMER_MODE_REL);
		}
	} else {
		if (chip->vib_enabled)
			qpnp_vibrator_play_off(chip);
	}
	mutex_unlock(&chip->lock);
}

static enum hrtimer_restart vib_stop_timer(struct hrtimer *timer)
{
	struct vib_pwm_chip *chip = container_of(timer, struct vib_pwm_chip, stop_timer);

	chip->state = 0;
	schedule_work(&chip->vib_work);
	return HRTIMER_NORESTART;
}

static ssize_t qpnp_vib_show_state(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", chip->vib_enabled);
}

static ssize_t qpnp_vib_store_state(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return count;
}

static ssize_t qpnp_vib_show_effect(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", chip->effect_idx);
}

static ssize_t qpnp_vib_store_effect(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0)
		return ret;

	mutex_lock(&chip->lock);
	chip->effect_idx = val;
	pr_info("store_effect -> effect_idx=%d\n", chip->effect_idx);
	mutex_unlock(&chip->lock);

	return count;
}

static ssize_t qpnp_vib_show_duration(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);
	ktime_t time_rem;
	s64 time_ms = 0;

	if (hrtimer_active(&chip->stop_timer)) {
		time_rem = hrtimer_get_remaining(&chip->stop_timer);
		time_ms = ktime_to_ms(time_rem);
	}
	return snprintf(buf, PAGE_SIZE, "%lld\n", time_ms);
}

static ssize_t qpnp_vib_store_duration(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0)
		return ret;

	mutex_lock(&chip->lock);

	if (val <= QPNP_VIB_EFFECT1_MS)
		chip->effect_idx = 1;
	else if (val <= QPNP_VIB_EFFECT2_MS)
		chip->effect_idx = 2;
	else if (val <= QPNP_VIB_EFFECT3_MS)
		chip->effect_idx = 3;
	else
		chip->effect_idx = 0;

	if (chip->effect_idx == 0) {
		if (val < QPNP_VIB_MIN_PLAY_MS)
			val = QPNP_VIB_MIN_PLAY_MS;
		if (val > QPNP_VIB_MAX_PLAY_MS)
			val = QPNP_VIB_MAX_PLAY_MS;
	}

	chip->vib_play_ms = val;
	pr_info("store_duration->effect=%d val=%u\n", chip->effect_idx, val);
	mutex_unlock(&chip->lock);

	return count;
}

static ssize_t qpnp_vib_show_activate(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t qpnp_vib_store_activate(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct vib_pwm_chip *chip = container_of(cdev, struct vib_pwm_chip, cdev);
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val != 0 && val != 1)
		return count;

	mutex_lock(&chip->lock);
	hrtimer_cancel(&chip->stop_timer);
	chip->state = val;
	pr_info("store_activate -> state=%d duration=%llums\n",
		chip->state, chip->vib_play_ms);
	mutex_unlock(&chip->lock);

	schedule_work(&chip->vib_work);
	return count;
}

static struct device_attribute qpnp_vib_attrs[] = {
	__ATTR(state, 0644, qpnp_vib_show_state, qpnp_vib_store_state),
	__ATTR(effect, 0644, qpnp_vib_show_effect, qpnp_vib_store_effect),
	__ATTR(duration, 0644, qpnp_vib_show_duration, qpnp_vib_store_duration),
	__ATTR(activate, 0644, qpnp_vib_show_activate, qpnp_vib_store_activate),
};

static struct attribute *qpnp_vib_attr_ptrs[] = {
	&qpnp_vib_attrs[0].attr,
	&qpnp_vib_attrs[1].attr,
	&qpnp_vib_attrs[2].attr,
	&qpnp_vib_attrs[3].attr,
	NULL
};

static const struct attribute_group qpnp_vib_attr_group = {
	.attrs = qpnp_vib_attr_ptrs,
};

static enum led_brightness qpnp_vib_brightness_get(struct led_classdev *cdev)
{
	return 0;
}

static void qpnp_vib_brightness_set(struct led_classdev *cdev,
				    enum led_brightness level)
{
}

static int qpnp_vibrator_pwm_suspend(struct device *dev)
{
	struct vib_pwm_chip *chip = dev_get_drvdata(dev);

	hrtimer_cancel(&chip->stop_timer);

	mutex_lock(&chip->lock);
	chip->state = 0;
	mutex_unlock(&chip->lock);

	schedule_work(&chip->vib_work);
	flush_work(&chip->vib_work);

	return 0;
}

static SIMPLE_DEV_PM_OPS(qpnp_vibrator_pwm_pm_ops, qpnp_vibrator_pwm_suspend, NULL);

static int qpnp_vib_parse_dt(struct vib_pwm_chip *chip)
{
	struct device_node *node = chip->dev->of_node, *child_node;
	int rc = 0, id = 0, idx = 0;

	chip->pwm_nums = of_get_available_child_count(node);
	if (chip->pwm_nums == 0) {
		pr_err("no vib child node defined\n");
		return -ENODEV;
	}

	rc = of_get_named_gpio_flags(node, "vib,en-gpio", 0, &chip->en_gpio_flags);
	if (rc < 0) {
		pr_info("vib,en-gpio not found or error (%d), disabling en_gpio\n", rc);
		chip->en_gpio = -1;
	} else {
		chip->en_gpio = rc;
		pr_info("vib,en-gpio=%d\n", chip->en_gpio);
	}

	chip->pwm_devs = devm_kcalloc(chip->dev, chip->pwm_nums,
				      sizeof(*chip->pwm_devs), GFP_KERNEL);
	if (!chip->pwm_devs)
		return -ENOMEM;

	chip->pwm_count = 0;
	chip->pwm_last_valid = -1;

	for_each_available_child_of_node(node, child_node) {
		struct pwm_device *pwm;

		if (idx >= chip->pwm_nums)
			break;

		rc = of_property_read_u32(child_node, "pwm-sources", &id);
		if (rc) {
			pr_err("get pwm-sources failed, rc=%d\n", rc);
			return rc;
		}

		chip->id = id;
		chip->label = of_get_property(child_node, "label", NULL) ? : child_node->name;
		pr_info("label=%s\n", chip->label);

		pwm = devm_of_pwm_get(chip->dev, child_node, NULL);
		if (IS_ERR(pwm)) {
			rc = PTR_ERR(pwm);
			if (rc == -EPROBE_DEFER)
				return rc;
			pr_err("get pwm device for %s failed, rc=%d\n", chip->label, rc);
			continue;
		}

		chip->pwm_devs[idx] = pwm;
		chip->pwm_last_valid = idx;
		idx++;
	}

	chip->pwm_count = idx;
	if (chip->pwm_count == 0) {
		pr_err("no valid pwm devices found\n");
		return -ENODEV;
	}

	chip->pwm_dev = chip->pwm_devs[chip->pwm_last_valid];

	pr_info("found %d valid pwm(s), using index %d\n",
		chip->pwm_count, chip->pwm_last_valid);

	return 0;
}

static int qpnp_vibrator_pwm_probe(struct platform_device *pdev)
{
	struct vib_pwm_chip *chip;
	int ret;
	u32 base = 0;

	pr_info("probe start\n");

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;

	ret = qpnp_vib_parse_dt(chip);
	if (ret < 0) {
		pr_err("couldn't parse device tree, ret=%d\n", ret);
		return ret;
	}

	if (of_property_read_u32(pdev->dev.of_node, "reg", &base) == 0)
		chip->base = (u16)base;
	chip->vib_play_ms = QPNP_VIB_AVG_PLAY_MS;
	mutex_init(&chip->lock);
	INIT_WORK(&chip->vib_work, qpnp_vib_work);

	hrtimer_init(&chip->stop_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	chip->stop_timer.function = vib_stop_timer;
	dev_set_drvdata(&pdev->dev, chip);

	chip->cdev.name = "vibrator";
	chip->cdev.brightness_get = qpnp_vib_brightness_get;
	chip->cdev.brightness_set = qpnp_vib_brightness_set;
	chip->cdev.max_brightness = 100;

	ret = devm_led_classdev_register(&pdev->dev, &chip->cdev);
	if (ret < 0) {
		pr_err("error in registering led class device, ret=%d\n", ret);
		goto fail;
	}

	ret = sysfs_create_group(&chip->cdev.dev->kobj, &qpnp_vib_attr_group);
	if (ret < 0) {
		pr_err("error creating sysfs group, ret=%d\n", ret);
		goto fail;
	}

	pr_info("successfully registered\n");
	return 0;

fail:
	mutex_destroy(&chip->lock);
	dev_set_drvdata(&pdev->dev, NULL);
	return ret;
}

static int qpnp_vibrator_pwm_remove(struct platform_device *pdev)
{
	struct vib_pwm_chip *chip = dev_get_drvdata(&pdev->dev);

	if (!chip)
		return 0;

	hrtimer_cancel(&chip->stop_timer);
	cancel_work_sync(&chip->vib_work);

	sysfs_remove_group(&chip->cdev.dev->kobj, &qpnp_vib_attr_group);

	mutex_destroy(&chip->lock);
	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}

static const struct of_device_id vibrator_pwm_match_table[] = {
	{ .compatible = "qcom,lct-pwm-vibrator", },
	{ },
};
MODULE_DEVICE_TABLE(of, vibrator_pwm_match_table);

static struct platform_driver qpnp_vibrator_pwm_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "qcom,lct-pwm-vibrator",
		.of_match_table	= of_match_ptr(vibrator_pwm_match_table),
		.pm		= &qpnp_vibrator_pwm_pm_ops,
	},
	.probe		= qpnp_vibrator_pwm_probe,
	.remove	= qpnp_vibrator_pwm_remove,
};
module_platform_driver(qpnp_vibrator_pwm_driver);

MODULE_DESCRIPTION("QCOM QPNP Vibrator-PWM driver");
MODULE_LICENSE("GPL v2");
