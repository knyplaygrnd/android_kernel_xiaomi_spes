// SPDX-License-Identifier: GPL-2.0-only
/*
 * onewire_gpio - Bit-bang 1-Wire bus driver via direct GPIO register access
 *
 * Copyright (c) 2016 Xiaomi Inc.
 *
 */
#define pr_fmt(fmt) "[Onewire]: %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "onewire_gpio.h"

#define DRV_STRENGTH_16MA	GENMASK(8, 6)
#define GPIO_ENABLE		BIT(12)
#define GPIO_OUTPUT		BIT(9)
#define GPIO_INPUT		0
#define GPIO_PULL_UP		0x3
#define OUTPUT_HIGH		0x2
#define OUTPUT_LOW		0x0

struct onewire_gpio_data {
	struct platform_device *pdev;
	struct device *dev;

	int ow_gpio;

	void __iomem *gpio_in_out_reg;
	void __iomem *gpio_cfg_reg;

	struct pinctrl *ow_gpio_pinctrl;
	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_sleep;

	int version;
	int gpio_num;

	u32 onewire_gpio_cfg_addr;
	u32 onewire_gpio_level_addr;
	u32 gpio_offset;
	u32 gpio_reg[2];

	char bus_label[32];
	struct onewire_bus *bus;
};

/* Shared bus instance list */
static LIST_HEAD(ow_bus_list);
static DEFINE_MUTEX(ow_bus_mutex);
static struct class *onewire_class;
static int onewire_major;

static void ow_bus_kref_release(struct kref *ref)
{
	struct onewire_bus *bus = container_of(ref, struct onewire_bus, ref);

	WARN_ON(READ_ONCE(bus->priv));
	kfree(bus);
}

int onewire_bus_register(struct onewire_bus *bus, const char *name)
{
	if (!bus || !name)
		return -EINVAL;

	strscpy(bus->name, name, sizeof(bus->name));
	kref_init(&bus->ref);

	mutex_lock(&ow_bus_mutex);
	list_add(&bus->node, &ow_bus_list);
	mutex_unlock(&ow_bus_mutex);

	pr_info("registered bus '%s'\n", bus->name);
	return 0;
}
EXPORT_SYMBOL(onewire_bus_register);

void onewire_bus_unregister(struct onewire_bus *bus)
{
	if (!bus)
		return;

	mutex_lock(&ow_bus_mutex);
	list_del(&bus->node);
	mutex_unlock(&ow_bus_mutex);

	pr_info("unregistered bus '%s'\n", bus->name);
	kref_put(&bus->ref, ow_bus_kref_release);
}
EXPORT_SYMBOL(onewire_bus_unregister);

struct onewire_bus *onewire_bus_get(const char *name)
{
	struct onewire_bus *bus, *found = NULL;

	if (!name)
		return NULL;

	mutex_lock(&ow_bus_mutex);
	list_for_each_entry(bus, &ow_bus_list, node) {
		if (strcmp(bus->name, name) == 0) {
			if (kref_get_unless_zero(&bus->ref))
				found = bus;
			break;
		}
	}
	mutex_unlock(&ow_bus_mutex);

	return found;
}
EXPORT_SYMBOL(onewire_bus_get);

void onewire_bus_put(struct onewire_bus *bus)
{
	if (bus)
		kref_put(&bus->ref, ow_bus_kref_release);
}
EXPORT_SYMBOL(onewire_bus_put);

static inline void ow_pin_output(struct onewire_gpio_data *od)
{
	writel(GPIO_ENABLE | DRV_STRENGTH_16MA | GPIO_OUTPUT | GPIO_PULL_UP,
	       od->gpio_cfg_reg);
}

static inline void ow_pin_input(struct onewire_gpio_data *od)
{
	writel(GPIO_ENABLE | DRV_STRENGTH_16MA | GPIO_INPUT | GPIO_PULL_UP,
	       od->gpio_cfg_reg);
}

static inline void ow_pin_high(struct onewire_gpio_data *od)
{
	writel(OUTPUT_HIGH, od->gpio_in_out_reg);
}

static inline void ow_pin_low(struct onewire_gpio_data *od)
{
	writel(OUTPUT_LOW, od->gpio_in_out_reg);
}

static inline unsigned int ow_pin_read(struct onewire_gpio_data *od)
{
	return readl(od->gpio_in_out_reg) & 0x01;
}

/**
 * ow_read_bit - read one 1-Wire bit slot.
 *
 * IRQs are disabled only for the timing-critical window: from the master's
 * falling edge through the 500 ns sample point. The post-sample recovery
 * delay (udelay(5) + pin idle + udelay(6)) runs with IRQs enabled.
 */
static u8 ow_read_bit(struct onewire_gpio_data *od)
{
	unsigned long flags;
	unsigned int val;

	local_irq_save(flags);
	ow_pin_output(od);
	ow_pin_low(od);
	udelay(1);
	ow_pin_input(od);
	ndelay(500);
	val = ow_pin_read(od);
	local_irq_restore(flags);

	udelay(5);
	ow_pin_high(od);
	ow_pin_output(od);
	udelay(6);

	return (u8)val;
}

/**
 * ow_write_bit - write one 1-Wire bit slot.
 *
 * IRQs are disabled for the drive-low -> drive-high window (~11 us).
 */
static void ow_write_bit(struct onewire_gpio_data *od, u8 bitval)
{
	unsigned long flags;

	ow_pin_output(od);
	udelay(5);
	local_irq_save(flags);
	ow_pin_low(od);
	udelay(1);
	if (bitval)
		ow_pin_high(od);
	udelay(10);
	local_irq_restore(flags);

	ow_pin_high(od);
	udelay(6);
}

/**
 * ops implementations.
 *
 * Each op reads bus->priv with READ_ONCE. If the provider has already called
 * onewire_bus_unregister (which sets priv = NULL), the consumer gets a safe
 * no-op rather than a UAF.
 */
static u8 ow_ops_reset(struct onewire_bus *bus)
{
	struct onewire_gpio_data *od = READ_ONCE(bus->priv);
	unsigned long flags;
	u8 presence;

	if (!od)
		return (u8)-ENODEV;

	ow_pin_output(od);
	ow_pin_low(od);
	udelay(50);
	local_irq_save(flags);
	ow_pin_high(od);
	ow_pin_input(od);
	udelay(7);
	presence = (u8)ow_pin_read(od);
	local_irq_restore(flags);
	udelay(50);

	return presence;
}

static u8 ow_ops_read_byte(struct onewire_bus *bus)
{
	struct onewire_gpio_data *od = READ_ONCE(bus->priv);
	unsigned long flags;
	u8 i, value = 0;

	if (!od)
		return (u8)-ENODEV;

	local_irq_save(flags);
	for (i = 0; i < 8; i++) {
		if (ow_read_bit(od))
			value |= BIT(i);
	}
	local_irq_restore(flags);

	return value;
}

static void ow_ops_write_byte(struct onewire_bus *bus, u8 val)
{
	struct onewire_gpio_data *od = READ_ONCE(bus->priv);
	unsigned long flags;
	u8 i;

	if (!od)
		return;

	ow_pin_output(od);
	local_irq_save(flags);
	for (i = 0; i < 8; i++)
		ow_write_bit(od, (val >> i) & 0x01);
	local_irq_restore(flags);
}

static void ow_ops_software_reset(struct onewire_bus *bus)
{
	struct onewire_gpio_data *od = READ_ONCE(bus->priv);
	unsigned long flags;

	if (!od)
		return;

	ow_pin_output(od);
	ow_pin_low(od);
	local_irq_save(flags);
	udelay(480);
	ow_pin_high(od);
	ow_pin_input(od);
	udelay(70);
	(void)ow_pin_read(od);
	local_irq_restore(flags);
	udelay(410);
}

static const struct onewire_bus_ops ow_gpio_bus_ops = {
	.reset		= ow_ops_reset,
	.read_byte	= ow_ops_read_byte,
	.write_byte	= ow_ops_write_byte,
	.software_reset	= ow_ops_software_reset,
};

static ssize_t ow_gpio_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct onewire_gpio_data *od = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 gpio_get_value(od->ow_gpio));
}

static ssize_t ow_gpio_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct onewire_gpio_data *od = dev_get_drvdata(dev);
	struct onewire_bus tmp_bus = {
		.ops = &ow_gpio_bus_ops,
		.priv = od,
	};
	u8 rom_id[8] = {};
	u8 result;
	u8 i;
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	switch (val) {
	case 0:
		ow_pin_low(od);
		pr_info("pin: LOW\n");
		break;
	case 1:
		ow_pin_high(od);
		pr_info("pin: HIGH\n");
		break;
	case 2:
		ow_pin_output(od);
		pr_info("pin: OUTPUT\n");
		break;
	case 3:
		ow_pin_input(od);
		pr_info("pin: INPUT\n");
		break;
	case 4:
		result = ow_ops_reset(&tmp_bus);
		pr_info("ow_reset: %s (0x%02x)\n",
			result ? "no device" : "present", result);
		break;
	case 5:
		result = ow_read_bit(od);
		pr_info("read_bit: 0x%02x\n", result);
		break;
	case 6:
		ow_write_bit(od, 0x01);
		pr_info("write_bit: 1\n");
		break;
	case 7:
		result = ow_ops_reset(&tmp_bus);
		pr_info("ow_reset: %s (0x%02x)\n",
			result ? "no device" : "present", result);
		ow_ops_write_byte(&tmp_bus, 0x33);
		for (i = 0; i < 8; i++)
			rom_id[i] = ow_ops_read_byte(&tmp_bus);
		pr_info("RomID = %8phC\n", rom_id);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR(ow_gpio, 0660, ow_gpio_show, ow_gpio_store);

static int onewire_gpio_parse_dt(struct device *dev,
				 struct onewire_gpio_data *od)
{
	struct device_node *np = dev->of_node;
	const char *label;
	int ret, val;

	ret = of_property_read_string(np, "label", &label);
	if (ret) {
		pr_err("missing 'label' property: %d\n", ret);
		return ret;
	}
	strscpy(od->bus_label, label, sizeof(od->bus_label));
	pr_debug("bus label: '%s'\n", od->bus_label);

	od->version = 0;
	if (!of_property_read_u32(np, "xiaomi,version", &val))
		od->version = val;

	od->ow_gpio = of_get_named_gpio_flags(np, "xiaomi,ow_gpio", 0, NULL);
	pr_debug("ow_gpio: %d\n", od->ow_gpio);

	od->gpio_num = 0;
	if (!of_property_read_u32(np, "xiaomi,gpio_number", &val))
		od->gpio_num = val;

	ret = of_property_read_u32_array(np, "mi,onewire-gpio-cfg-addr",
					 od->gpio_reg, 2);
	if (ret) {
		pr_err("missing mi,onewire-gpio-cfg-addr: %d\n", ret);
		return ret;
	}

	od->onewire_gpio_cfg_addr = od->gpio_reg[0];
	od->gpio_offset = od->gpio_reg[1];
	od->onewire_gpio_level_addr = od->onewire_gpio_cfg_addr + od->gpio_offset;
	return 0;
}

static int onewire_gpio_pinctrl_init(struct onewire_gpio_data *od)
{
	int ret;

	od->ow_gpio_pinctrl = devm_pinctrl_get(&od->pdev->dev);
	if (IS_ERR_OR_NULL(od->ow_gpio_pinctrl)) {
		ret = PTR_ERR(od->ow_gpio_pinctrl);
		pr_err("devm_pinctrl_get: %d\n", ret);
		od->ow_gpio_pinctrl = NULL;
		return ret;
	}

	od->pinctrl_state_active =
		pinctrl_lookup_state(od->ow_gpio_pinctrl, "onewire_active");
	if (IS_ERR_OR_NULL(od->pinctrl_state_active)) {
		ret = PTR_ERR(od->pinctrl_state_active);
		pr_err("lookup onewire_active: %d\n", ret);
		return ret;
	}

	od->pinctrl_state_sleep =
		pinctrl_lookup_state(od->ow_gpio_pinctrl, "onewire_sleep");
	if (IS_ERR_OR_NULL(od->pinctrl_state_sleep)) {
		ret = PTR_ERR(od->pinctrl_state_sleep);
		pr_err("lookup onewire_sleep: %d\n", ret);
		return ret;
	}

	return 0;
}

static int onewire_gpio_probe(struct platform_device *pdev)
{
	struct onewire_gpio_data *od;
	struct onewire_bus *bus;
	struct device *dev = &pdev->dev;
	int ret;

	pr_info("entry\n");

	if (!dev->of_node || !of_device_is_available(dev->of_node))
		return -ENODEV;

	od = devm_kzalloc(dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	ret = onewire_gpio_parse_dt(dev, od);
	if (ret)
		return ret;

	od->pdev = pdev;
	platform_set_drvdata(pdev, od);

	ret = onewire_gpio_pinctrl_init(od);
	if (ret)
		return ret;

	if (od->ow_gpio_pinctrl) {
		ret = pinctrl_select_state(od->ow_gpio_pinctrl,
					   od->pinctrl_state_active);
		if (ret) {
			pr_err("select active pinstate: %d\n", ret);
			return ret;
		}
	}

	if (!gpio_is_valid(od->ow_gpio)) {
		pr_err("invalid GPIO %d\n", od->ow_gpio);
		return -EINVAL;
	}

	ret = gpio_request(od->ow_gpio, "onewire gpio");
	if (ret) {
		pr_err("gpio_request: %d\n", ret);
		return ret;
	}

	gpio_direction_output(od->ow_gpio, 1);
	od->gpio_in_out_reg = devm_ioremap(dev,
					   (uintptr_t)od->onewire_gpio_level_addr, 0x4);
	if (!od->gpio_in_out_reg) {
		pr_err("ioremap gpio_in_out_reg failed\n");
		ret = -ENOMEM;
		goto err_free_gpio;
	}

	od->gpio_cfg_reg = devm_ioremap(dev,
					(uintptr_t)od->onewire_gpio_cfg_addr, 0x4);
	if (!od->gpio_cfg_reg) {
		pr_err("ioremap gpio_cfg_reg failed\n");
		ret = -ENOMEM;
		goto err_free_gpio;
	}

	pr_info("cfg_reg=%p in_out_reg=%p\n",
		od->gpio_cfg_reg, od->gpio_in_out_reg);

	if (!dev->parent || !dev->parent->parent) {
		pr_err("parent chain unavailable\n");
		ret = -ENODEV;
		goto err_free_gpio;
	}

	od->dev = device_create(onewire_class, dev->parent->parent,
				MKDEV(onewire_major, 0), od, "onewirectrl");
	if (IS_ERR(od->dev)) {
		ret = PTR_ERR(od->dev);
		pr_err("device_create: %d\n", ret);
		od->dev = NULL;
		goto err_free_gpio;
	}

	ret = sysfs_create_file(&od->dev->kobj, &dev_attr_ow_gpio.attr);
	if (ret) {
		pr_err("sysfs_create_file: %d\n", ret);
		goto err_destroy_dev;
	}

	ret = sysfs_create_link(&od->dev->kobj, &dev->kobj, "pltdev");
	if (ret) {
		pr_err("sysfs_create_link: %d\n", ret);
		goto err_remove_attr;
	}

	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus) {
		ret = -ENOMEM;
		goto err_remove_link;
	}

	bus->ops = &ow_gpio_bus_ops;
	WRITE_ONCE(bus->priv, od);

	ret = onewire_bus_register(bus, od->bus_label);
	if (ret) {
		pr_err("onewire_bus_register('%s'): %d\n", od->bus_label, ret);
		kfree(bus);
		goto err_remove_link;
	}

	od->bus = bus;
	pr_info("success\n");
	return 0;

err_remove_link:
	sysfs_remove_link(&od->dev->kobj, "pltdev");
err_remove_attr:
	sysfs_remove_file(&od->dev->kobj, &dev_attr_ow_gpio.attr);
err_destroy_dev:
	device_destroy(onewire_class, MKDEV(onewire_major, 0));
	od->dev = NULL;
err_free_gpio:
	gpio_free(od->ow_gpio);
	return ret;
}

static int onewire_gpio_remove(struct platform_device *pdev)
{
	struct onewire_gpio_data *od = platform_get_drvdata(pdev);
	struct onewire_bus *bus = NULL;

	if (!od)
		goto out;

	if (od->bus) {
		bus = od->bus;
		od->bus = NULL;
		WRITE_ONCE(bus->priv, NULL);
		onewire_bus_unregister(bus);
	}

	if (od->dev) {
		sysfs_remove_link(&od->dev->kobj, "pltdev");
		sysfs_remove_file(&od->dev->kobj, &dev_attr_ow_gpio.attr);
		device_destroy(onewire_class, MKDEV(onewire_major, 0));
		od->dev = NULL;
	}

	if (gpio_is_valid(od->ow_gpio))
		gpio_free(od->ow_gpio);

out:
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct file_operations onewire_dev_fops = {
	.owner	= THIS_MODULE,
};

static const struct of_device_id onewire_gpio_dt_match[] = {
	{ .compatible = "xiaomi,onewire_gpio", },
	{ },
};
MODULE_DEVICE_TABLE(of, onewire_gpio_dt_match);

static struct platform_driver onewire_gpio_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "onewire_gpio",
		.of_match_table	= of_match_ptr(onewire_gpio_dt_match),
	},
	.probe	= onewire_gpio_probe,
	.remove	= onewire_gpio_remove,
};

static int __init onewire_gpio_init(void)
{
	int ret;

	pr_info("entry\n");

	onewire_class = class_create(THIS_MODULE, "onewire");
	if (IS_ERR(onewire_class)) {
		pr_err("class_create failed\n");
		return PTR_ERR(onewire_class);
	}

	onewire_major = register_chrdev(0, "onewirectrl", &onewire_dev_fops);
	if (onewire_major < 0) {
		ret = onewire_major;
		pr_err("register_chrdev failed: %d\n", ret);
		goto err_class;
	}

	ret = platform_driver_register(&onewire_gpio_driver);
	if (ret) {
		pr_err("platform_driver_register failed: %d\n", ret);
		goto err_chrdev;
	}

	pr_info("driver registered\n");
	return 0;

err_chrdev:
	unregister_chrdev(onewire_major, "onewirectrl");
err_class:
	class_destroy(onewire_class);
	return ret;
}

static void __exit onewire_gpio_exit(void)
{
	pr_info("exit\n");
	platform_driver_unregister(&onewire_gpio_driver);
	unregister_chrdev(onewire_major, "onewirectrl");
	class_destroy(onewire_class);
}

subsys_initcall(onewire_gpio_init);
module_exit(onewire_gpio_exit);

MODULE_AUTHOR("Xiaomi Inc.");
MODULE_DESCRIPTION("1-Wire bit-bang GPIO bus driver");
MODULE_LICENSE("GPL");
