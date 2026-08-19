// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, controlling GPIOs such as sensor reset
 * line, sensor IRQ line.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node.
 *
 * This driver will NOT send any commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */
#define pr_fmt(fmt) "[fpc1020] %s: " fmt, __func__

#include "fpc1020_platform_tee.h"

static irqreturn_t fpc1020_irq_handler(int irq, void *handle);
static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
				      const char *label, int *gpio);
static int hw_reset(struct fpc1020_data *fpc1020);

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
		      bool enable)
{
	size_t i;
	int rc;
	struct regulator *vreg;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(fpc1020->vreg); i++) {
		const char *n = vreg_conf[i].name;

		if (!strncmp(n, name, strlen(n)))
			goto found;
	}

	pr_err("Regulator %s not found\n", name);

	return -EINVAL;

found:
	vreg = fpc1020->vreg[i];
	if (enable) {
		if (!vreg) {
			vreg = devm_regulator_get(dev, name);
			if (IS_ERR_OR_NULL(vreg)) {
				pr_err("Unable to get %s\n", name);
				return PTR_ERR(vreg);
			}
		}

		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, vreg_conf[i].vmin,
						   vreg_conf[i].vmax);
			if (rc)
				pr_err("Unable to set voltage on %s, %d\n",
				       name, rc);
		}

		rc = regulator_set_load(vreg, vreg_conf[i].ua_load);
		if (rc < 0)
			pr_err("Unable to set current on %s, %d\n",
			       name, rc);

		rc = regulator_enable(vreg);
		if (rc) {
			pr_err("error enabling %s: %d\n", name, rc);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	} else {
		if (vreg) {
			if (regulator_is_enabled(vreg)) {
				regulator_disable(vreg);
				pr_debug("disabled %s\n", name);
			}
			fpc1020->vreg[i] = NULL;
		}
		rc = 0;
	}

	return rc;
}

/**
 * sysfs node for controlling clocks.
 *
 * This is disabled in platform variant of this driver but kept for
 * backwards compatibility. Only prints a debug print that it is
 * disabled.
 */
static ssize_t clk_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	pr_debug("clk_enable sysfs node not enabled in platform driver\n");

	return count;
}
static DEVICE_ATTR_WO(clk_enable);

/**
 * Will try to select the set of pins (GPIOS) defined in a pin control node of
 * the device tree named @p name.
 *
 * The node can contain several eg. GPIOs that is controlled when selecting it.
 * The node may activate or deactivate the pins it contains, the action is
 * defined in the device tree node itself and not here. The states used
 * internally is fetched at probe time.
 *
 * @see pctl_names
 * @see fpc1020_probe
 */
static int select_pin_ctl(struct fpc1020_data *fpc1020, const char *name)
{
	size_t i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];

		if (!strncmp(n, name, strlen(n))) {
			rc = pinctrl_select_state(fpc1020->fingerprint_pinctrl,
						  fpc1020->pinctrl_state[i]);
			if (rc)
				pr_err("cannot select '%s'\n", name);
			else
				pr_debug("Selected '%s'\n", name);
			goto exit;
		}
	}

	rc = -EINVAL;
	pr_err("'%s' not found\n", name);

exit:
	return rc;
}

static ssize_t pinctl_set_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;

	mutex_lock(&fpc1020->lock);
	rc = select_pin_ctl(fpc1020, buf);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(pinctl_set);

static ssize_t regulator_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	char op;
	char name[16];
	int rc;
	bool enable;

	if (sscanf(buf, "%15[^,],%c", name, &op) != NUM_PARAMS_REG_ENABLE_SET)
		return -EINVAL;
	if (op == 'e')
		enable = true;
	else if (op == 'd')
		enable = false;
	else
		return -EINVAL;

	mutex_lock(&fpc1020->lock);
	rc = vreg_setup(fpc1020, name, enable);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(regulator_enable);

static int hw_reset(struct fpc1020_data *fpc1020)
{
	int irq_gpio;
	int rc;

	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	pr_info("IRQ before reset %d\n", irq_gpio);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc) {
		pr_err("[active]: select_pin before pre sleep fail, rc=%d\n", rc);
		goto exit;
	}
	usleep_range(RESET_HIGH_SLEEP1_MIN_US, RESET_HIGH_SLEEP1_MAX_US);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc) {
		pr_err("[reset]: select_pin before sleep fail, rc=%d\n", rc);
		goto exit;
	}
	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc) {
		pr_err("[active]: select_pin before post sleep fail, rc=%d\n", rc);
		goto exit;
	}
	usleep_range(RESET_HIGH_SLEEP2_MIN_US, RESET_HIGH_SLEEP2_MAX_US);

	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	pr_info("IRQ after reset %d\n", irq_gpio);

exit:
	return rc;
}

static ssize_t hw_reset_store(struct device *dev,
			      struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "reset", strlen("reset"))) {
		mutex_lock(&fpc1020->lock);
		rc = hw_reset(fpc1020);
		mutex_unlock(&fpc1020->lock);
	} else {
		return -EINVAL;
	}

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(hw_reset);

/**
 * Will setup GPIOs, and regulators to correctly initialize the touch sensor to
 * be ready for work.
 *
 * In the correct order according to the sensor spec this function will
 * enable/disable regulators, and reset line, all to set the sensor in a
 * correct power on or off state "electrical" wise.
 *
 * @see  device_prepare_store
 * @note This function will not send any commands to the sensor it will only
 *       control it "electrically".
 */
static int device_prepare(struct fpc1020_data *fpc1020, bool enable)
{
	int rc;

	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		fpc1020->prepared = true;
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");

		rc = vreg_setup(fpc1020, "vcc_spi", true);
		if (rc)
			goto exit;

		rc = vreg_setup(fpc1020, "vdd_io", true);
		if (rc)
			goto exit_1;

		rc = vreg_setup(fpc1020, "vdd_ana", true);
		if (rc)
			goto exit_2;

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		/* As we can't control chip select here the other part of the
		 * sensor driver eg. the TEE driver needs to do a _SOFT_ reset
		 * on the sensor after power up to be sure that the sensor is
		 * in a good state after power up. Okeyed by ASIC.
		 */

		(void)select_pin_ctl(fpc1020, "fpc1020_reset_active");
	} else if (!enable && fpc1020->prepared) {
		rc = 0;
		(void)select_pin_ctl(fpc1020, "fpc1020_reset_reset");

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		(void)vreg_setup(fpc1020, "vdd_ana", false);
exit_2:
		(void)vreg_setup(fpc1020, "vdd_io", false);
exit_1:
		(void)vreg_setup(fpc1020, "vcc_spi", false);
exit:
		fpc1020->prepared = false;
	} else {
		rc = 0;
	}
	mutex_unlock(&fpc1020->lock);

	return rc;
}

/**
 * sysfs node to enable/disable (power up/power down) the touch sensor
 *
 * @see device_prepare
 */
static ssize_t device_prepare_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable")))
		rc = device_prepare(fpc1020, true);
	else if (!strncmp(buf, "disable", strlen("disable")))
		rc = device_prepare(fpc1020, false);
	else
		return -EINVAL;

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(device_prepare);

/**
 * sysfs node for controlling whether the driver is allowed
 * to wake up the platform on interrupt.
 */
static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc1020->lock);
	if (!strncmp(buf, "enable", strlen("enable")))
		atomic_set(&fpc1020->wakeup_enabled, 1);
	else if (!strncmp(buf, "disable", strlen("disable")))
		atomic_set(&fpc1020->wakeup_enabled, 0);
	else
		ret = -EINVAL;
	mutex_unlock(&fpc1020->lock);

	return ret;
}
static DEVICE_ATTR_WO(wakeup_enable);

/**
 * sysfs node for controlling the wakelock.
 */
static ssize_t handle_wakelock_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc1020->lock);
	if (!strncmp(buf, RELEASE_WAKELOCK_W_V,
		     min(count, strlen(RELEASE_WAKELOCK_W_V)))) {
		if (fpc1020->nbr_irqs_received_counter_start == fpc1020->nbr_irqs_received) {
			__pm_relax(fpc1020->ttw_wl);
		} else {
			pr_debug("Ignore releasing of wakelock %d != %d\n",
				 fpc1020->nbr_irqs_received_counter_start,
					fpc1020->nbr_irqs_received);
		}
	} else if (!strncmp(buf, RELEASE_WAKELOCK,
		    min(count, strlen(RELEASE_WAKELOCK)))) {
		__pm_relax(fpc1020->ttw_wl);
	} else if (!strncmp(buf, START_IRQS_RECEIVED_CNT,
		    min(count, strlen(START_IRQS_RECEIVED_CNT)))) {
		fpc1020->nbr_irqs_received_counter_start = fpc1020->nbr_irqs_received;
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&fpc1020->lock);

	return ret;
}
static DEVICE_ATTR_WO(handle_wakelock);

/**
 * sysf node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static ssize_t irq_get(struct device *dev,
		       struct device_attribute *attr,
	char *buf)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int irq = gpio_get_value(fpc1020->irq_gpio);

	return scnprintf(buf, PAGE_SIZE, "%i\n", irq);
}

/**
 * writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 */
static ssize_t irq_ack(struct device *dev,
		       struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	pr_debug("%p\n", fpc1020);
	return count;
}
static DEVICE_ATTR(irq, 0600, irq_get, irq_ack);

#ifdef CONFIG_FPC_COMPAT
static ssize_t compatible_all_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int rc;
	int i;
	int irqf;
	struct fpc1020_data *fpc1020;

	fpc1020 = dev_get_drvdata(dev);
	if (!fpc1020) {
		pr_err("fpc1020 is NULL\n");
		return -EINVAL;
	}

	pr_info("compatible all enter %d\n", fpc1020->compatible_enabled);

	if (!strncmp(buf, "enable", strlen("enable")) &&
	    fpc1020->compatible_enabled != 1) {
		rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq",
						&fpc1020->irq_gpio);
		if (rc) {
			pr_err("fpc request irq result = %d\n", rc);
			goto exit;
		}

		rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst",
						&fpc1020->rst_gpio);
		if (rc) {
			pr_err("fpc request reset result = %d\n", rc);
			goto exit;
		}

		fpc1020->fingerprint_pinctrl = devm_pinctrl_get(dev);
		if (IS_ERR(fpc1020->fingerprint_pinctrl)) {
			if (PTR_ERR(fpc1020->fingerprint_pinctrl) == -EPROBE_DEFER) {
				pr_info("pinctrl not ready\n");
				rc = -EPROBE_DEFER;
				goto exit;
			}
			pr_err("Target does not use pinctrl\n");
			fpc1020->fingerprint_pinctrl = NULL;
			rc = -EINVAL;
			goto exit;
		}

		for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
			const char *n;
			struct pinctrl_state *state;

			n = pctl_names[i];
			state = pinctrl_lookup_state(fpc1020->fingerprint_pinctrl, n);
			if (IS_ERR(state)) {
				pr_err("cannot find '%s'\n", n);
				rc = -EINVAL;
				goto exit;
			}
			pr_info("found pin control %s\n", n);
			fpc1020->pinctrl_state[i] = state;
		}

		rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
		if (rc) {
			pr_err("[reset]: select_pin fail, rc=%d\n", rc);
			goto exit;
		}

		rc = select_pin_ctl(fpc1020, "fpc1020_irq_active");
		if (rc) {
			pr_err("[active]: select_pin fail, rc=%d\n", rc);
			goto exit;
		}

		irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
		if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
			irqf |= IRQF_NO_SUSPEND;
			device_init_wakeup(dev, 1);
			fpc1020->dev_wake = true;
		}

		rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
					       NULL, fpc1020_irq_handler, irqf,
					       dev_name(dev), fpc1020);
		if (rc) {
			pr_err("could not request irq %d\n",
			       gpio_to_irq(fpc1020->irq_gpio));
			goto exit;
		}
		pr_debug("requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

		enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));
		fpc1020->irq_wake = true;
		fpc1020->compatible_enabled = 1;

		if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
			pr_info("Enabling hardware\n");
			(void)device_prepare(fpc1020, true);
		}
		hw_reset(fpc1020);

	} else if (!strncmp(buf, "disable", strlen("disable")) &&
		   fpc1020->compatible_enabled != 0) {
		if (fpc1020->irq_wake) {
			disable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));
			devm_free_irq(dev, gpio_to_irq(fpc1020->irq_gpio), fpc1020);
			fpc1020->irq_wake = false;
		}

		if (fpc1020->dev_wake) {
			device_init_wakeup(dev, 0);
			fpc1020->dev_wake = false;
		}

		if (gpio_is_valid(fpc1020->irq_gpio)) {
			devm_gpio_free(dev, fpc1020->irq_gpio);
			pr_info("remove irq_gpio success\n");
		}

		if (gpio_is_valid(fpc1020->rst_gpio)) {
			devm_gpio_free(dev, fpc1020->rst_gpio);
			pr_info("remove rst_gpio success\n");
		}
		fpc1020->compatible_enabled = 0;
	}

	return count;

exit:
	return -EINVAL;
}
static DEVICE_ATTR_WO(compatible_all);
#endif

static struct attribute *attributes[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_device_prepare.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_handle_wakelock.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
#ifdef CONFIG_FPC_COMPAT
	&dev_attr_compatible_all.attr,
#endif
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	mutex_lock(&fpc1020->lock);
	if (atomic_read(&fpc1020->wakeup_enabled)) {
		fpc1020->nbr_irqs_received++;
		__pm_wakeup_event(fpc1020->ttw_wl, FPC_TTW_HOLD_TIME);
	}
	mutex_unlock(&fpc1020->lock);

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
				      const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);

	if (rc < 0) {
		pr_err("failed to get '%s'\n", label);
		return rc;
	}
	*gpio = rc;

	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		pr_err("failed to request gpio %d\n", *gpio);
		return rc;
	}
	pr_info("%s %d\n", label, *gpio);

	return 0;
}

static int proc_show_ver(struct seq_file *file, void *v)
{
	seq_puts(file, "Fingerprint: FPC\n");
	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	single_open(file, proc_show_ver, NULL);
	return 0;
}

static const struct file_operations proc_file_fpc_ops = {
	.owner = THIS_MODULE,
	.open = proc_open,
	.read = seq_read,
	.release = single_release,
};

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
#ifndef CONFIG_FPC_COMPAT
	size_t i;
	int irqf;
#endif
	struct device_node *np = dev->of_node;
	struct fpc1020_data *fpc1020;

	pr_info("fpc1020 probe entry\n");

	fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020), GFP_KERNEL);
	if (!fpc1020)
		return -ENOMEM;

	fpc1020->dev = dev;
	platform_set_drvdata(pdev, fpc1020);

	if (!np) {
		pr_err("no of node found\n");
		return -EINVAL;
	}

	atomic_set(&fpc1020->wakeup_enabled, 0);
	mutex_init(&fpc1020->lock);

#ifndef CONFIG_FPC_COMPAT
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq", &fpc1020->irq_gpio);
	if (rc)
		goto out;

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst", &fpc1020->rst_gpio);
	if (rc)
		goto out;

	fpc1020->fingerprint_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc1020->fingerprint_pinctrl)) {
		if (PTR_ERR(fpc1020->fingerprint_pinctrl) == -EPROBE_DEFER) {
			pr_info("pinctrl not ready\n");
			rc = -EPROBE_DEFER;
			goto out;
		}
		pr_err("Target does not use pinctrl\n");
		fpc1020->fingerprint_pinctrl = NULL;
		rc = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state = pinctrl_lookup_state(fpc1020->fingerprint_pinctrl, n);

		if (IS_ERR(state)) {
			pr_err("cannot find '%s'\n", n);
			rc = -EINVAL;
			goto out;
		}
		pr_info("found pin control %s\n", n);
		fpc1020->pinctrl_state[i] = state;
	}

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc)
		goto out;

	rc = select_pin_ctl(fpc1020, "fpc1020_irq_active");
	if (rc)
		goto out;
#endif /* CONFIG_FPC_COMPAT */

	fpc1020->ttw_wl = wakeup_source_register(dev, "fpc_ttw_wl");
	if (IS_ERR(fpc1020->ttw_wl)) {
		rc = PTR_ERR(fpc1020->ttw_wl);
		fpc1020->ttw_wl = NULL;
		goto out;
	}

#ifndef CONFIG_FPC_COMPAT
	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		device_init_wakeup(dev, 1);
		fpc1020->dev_wake = true;
	}

	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
				       NULL, fpc1020_irq_handler, irqf,
				dev_name(dev), fpc1020);
	if (rc) {
		pr_err("could not request irq %d\n", gpio_to_irq(fpc1020->irq_gpio));
		goto err_wl;
	}

	pr_debug("requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));
	enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));
	fpc1020->irq_wake = true;
#endif /* CONFIG_FPC_COMPAT */

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc) {
		pr_err("could not create sysfs");
#ifndef CONFIG_FPC_COMPAT
		goto err_irq;
#else
		goto err_wl;
#endif
	}

#ifndef CONFIG_FPC_COMPAT
	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
		pr_info("Enabling hardware\n");
		(void)device_prepare(fpc1020, true);
	}

	rc = hw_reset(fpc1020);
	if (rc) {
		pr_err("hardware reset failed\n");
		goto err_sysfs;
	}
#endif /* CONFIG_FPC_COMPAT */

	proc_entry = proc_create(PROC_NAME, 0644, NULL, &proc_file_fpc_ops);
	if (!proc_entry) {
		pr_err("fpc1020 Couldn't create proc entry!");
		rc = -ENOMEM;
		goto err_sysfs;
	}

	pr_info("fpc1020 probe success\n");
	return 0;

err_sysfs:
	sysfs_remove_group(&dev->kobj, &attribute_group);
#ifndef CONFIG_FPC_COMPAT
err_irq:
	if (fpc1020->irq_wake) {
		disable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));
		fpc1020->irq_wake = false;
	}
#endif
err_wl:
#ifndef CONFIG_FPC_COMPAT
	if (fpc1020->dev_wake) {
		device_init_wakeup(dev, 0);
		fpc1020->dev_wake = false;
	}
#endif
	if (fpc1020->ttw_wl) {
		wakeup_source_unregister(fpc1020->ttw_wl);
		fpc1020->ttw_wl = NULL;
	}
out:
	mutex_destroy(&fpc1020->lock);
	return rc;
}

static int fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020_data *fpc1020 = platform_get_drvdata(pdev);

	if (!fpc1020)
		return 0;

	if (fpc1020->irq_wake)
		disable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));

	if (fpc1020->dev_wake)
		device_init_wakeup(&pdev->dev, 0);

	if (proc_entry)
		remove_proc_entry(PROC_NAME, NULL);
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);

	if (fpc1020->ttw_wl)
		wakeup_source_unregister(fpc1020->ttw_wl);

	(void)vreg_setup(fpc1020, "vdd_ana", false);
	(void)vreg_setup(fpc1020, "vdd_io", false);
	(void)vreg_setup(fpc1020, "vcc_spi", false);

	mutex_destroy(&fpc1020->lock);

	return 0;
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{ },
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "fpc1020",
		.of_match_table	= of_match_ptr(fpc1020_of_match),
	},
	.probe		= fpc1020_probe,
	.remove	= fpc1020_remove,
};

static int __init fpc1020_init(void)
{
	if (fpsensor != 1) {
		pr_err("%s failed as fpsensor = %d(1=fp)\n", __func__, fpsensor);
		return -EINVAL;
	}

	platform_driver_register(&fpc1020_driver);
	pr_info("%s - OK\n", __func__);

	return 0;
}

static void __exit fpc1020_exit(void)
{
	platform_driver_unregister(&fpc1020_driver);
}

module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
