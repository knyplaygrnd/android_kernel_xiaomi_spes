// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "[ant_check]: %s: " fmt, __func__

#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/fb.h>

#define CONFIG_ANT_SYS
struct ant_check_info {
	u32 irq_gpio;
	u32 irq_gpio_flags;
	int irq;
	int ant_check_state;
	struct input_dev *ipdev;
#ifdef CONFIG_ANT_SYS
	struct class ant_sys_class;
	bool ant_sys_registered;
#endif
};

struct ant_check_info *global_ant_info;

static irqreturn_t ant_interrupt(int irq, void *data)
{
	struct ant_check_info *ant_info = data;
	int ant_gpio;

	ant_gpio = gpio_get_value_cansleep(ant_info->irq_gpio);
	pr_info("gpio = %d\n", ant_gpio);
	if (ant_gpio == ant_info->ant_check_state)
		return IRQ_HANDLED;

	ant_info->ant_check_state = ant_gpio;
	pr_err("report key %s\n", ant_gpio ? "connect" : "disconnect");

	if (ant_gpio) {
		input_report_key(ant_info->ipdev, KEY_ANT_CONNECT, 1);
		input_report_key(ant_info->ipdev, KEY_ANT_CONNECT, 0);
		input_sync(ant_info->ipdev);
	} else {
		input_report_key(ant_info->ipdev, KEY_ANT_UNCONNECT, 1);
		input_report_key(ant_info->ipdev, KEY_ANT_UNCONNECT, 0);
		input_sync(ant_info->ipdev);
	}

	return IRQ_HANDLED;
}

static int ant_parse_dt(struct device *dev, struct ant_check_info *pdata)
{
	struct device_node *np = dev->of_node;

	pdata->irq_gpio = of_get_named_gpio_flags(np, "ant_check_gpio", 0,
						  &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		return pdata->irq_gpio;

	pr_info("irq_gpio = %d\n", pdata->irq_gpio);
	return 0;
}

#ifdef CONFIG_ANT_SYS
static ssize_t ant_state_show(struct class *class,
			      struct class_attribute *attr,
			      char *buf)
{
	struct ant_check_info *ant_info =
		container_of(class, struct ant_check_info, ant_sys_class);
	int state;

	if (!ant_info)
		return -ENODEV;

	state = ant_info->ant_check_state ? 3 : 2;

	pr_info("state = %d, custome_state = %d\n",
		ant_info->ant_check_state, state);

	return sysfs_emit(buf, "%d\n", state);
}

#if defined(CONFIG_KERNEL_DRIVER_D2S_CN)
static ssize_t ant_state_store(struct class *class,
			       struct class_attribute *attr,
			       const char *buf, size_t size)
{
	struct ant_check_info *ant_info =
		container_of(class, struct ant_check_info, ant_sys_class);
	int rc, state;

	if (!ant_info)
		return -ENODEV;

	rc = kstrtoint(buf, 10, &state);
	if (rc) {
		pr_err("kstrtoint failed, rc=%d\n", rc);
		return rc;
	}

	if (state == 0)
		disable_irq(ant_info->irq);
	else
		enable_irq(ant_info->irq);

	return size;
}
#endif

#if defined(CONFIG_KERNEL_DRIVER_D2S_CN)
static struct class_attribute ant_state = __ATTR_RW(ant_state);
#else
static struct class_attribute ant_state = __ATTR_RO(ant_state);
#endif

static int ant_register_class_dev(struct ant_check_info *ant_info)
{
	int err;

	ant_info->ant_sys_class.name = "ant_class";
	ant_info->ant_sys_class.owner = THIS_MODULE;

	err = class_register(&ant_info->ant_sys_class);
	if (err)
		return err;

	err = class_create_file(&ant_info->ant_sys_class, &ant_state);
	if (err) {
		class_unregister(&ant_info->ant_sys_class);
		return err;
	}

	ant_info->ant_sys_registered = true;
	return 0;
}
#endif

static int ant_probe(struct platform_device *pdev)
{
	int rc = 0;
	int err;
	struct ant_check_info *ant_info;

	pr_info("entry\n");
	if (!pdev->dev.of_node)
		return -ENODEV;

	ant_info = kzalloc(sizeof(*ant_info), GFP_KERNEL);
	if (!ant_info)
		return -ENOMEM;

	err = ant_parse_dt(&pdev->dev, ant_info);
	if (err) {
		dev_err(&pdev->dev, "DT parsing failed\n");
		goto free_struct;
	}

	platform_set_drvdata(pdev, ant_info);

	/* input system config */
	ant_info->ipdev = input_allocate_device();
	if (!ant_info->ipdev) {
		pr_err("input_allocate_device fail\n");
		rc = -ENOMEM;
		goto put_drvdata_free;
	}

	ant_info->ipdev->name = "ant_check-input";
	input_set_capability(ant_info->ipdev, EV_KEY, KEY_ANT_CONNECT);
	input_set_capability(ant_info->ipdev, EV_KEY, KEY_ANT_UNCONNECT);

	rc = input_register_device(ant_info->ipdev);
	if (rc) {
		pr_err("input_register_device fail, rc=%d\n", rc);
		input_free_device(ant_info->ipdev);
		goto put_drvdata_free;
	}

	/* interrupt config */
	if (!gpio_is_valid(ant_info->irq_gpio)) {
		pr_err("irq gpio not provided\n");
		rc = -EINVAL;
		goto unregister_input;
	}

	rc = gpio_request(ant_info->irq_gpio, "ant_check");
	if (rc < 0) {
		pr_err("gpio_request fail, rc=%d\n", rc);
		goto unregister_input;
	}

	rc = gpio_direction_input(ant_info->irq_gpio);
	if (rc < 0) {
		pr_err("gpio_direction_input fail, rc=%d\n", rc);
		goto free_gpio;
	}

	ant_info->ant_check_state = gpio_get_value(ant_info->irq_gpio);
	pr_info("gpios = %d\n", ant_info->ant_check_state);

	ant_info->irq = gpio_to_irq(ant_info->irq_gpio);
	pr_info("irq = %d\n", ant_info->irq);

#if defined(CONFIG_KERNEL_DRIVER_D2S_CN)
	rc = request_irq(ant_info->irq, ant_interrupt,
			 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			 "ant-switch-irq", ant_info);
	if (rc < 0) {
		pr_err("request_irq fail, rc=%d\n", rc);
		goto free_gpio;
	}
	disable_irq(ant_info->irq);
#else
	rc = devm_request_threaded_irq(&pdev->dev, ant_info->irq, NULL, ant_interrupt,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"ant-switch-irq", ant_info);
	if (rc < 0) {
		pr_err("devm_request_threaded_irq fail, rc=%d\n", rc);
		goto free_gpio;
	}

	device_init_wakeup(&pdev->dev, true);
	irq_set_irq_wake(ant_info->irq, 1);
#endif

#ifdef CONFIG_ANT_SYS
	rc = ant_register_class_dev(ant_info);
	if (rc) {
		pr_err("ant_register_class_dev fail, rc=%d\n", rc);
		goto free_irq;
	}
#endif

	global_ant_info = ant_info;
	pr_info("done\n");

	return 0;

#ifdef CONFIG_ANT_SYS
free_irq:
#if defined(CONFIG_KERNEL_DRIVER_D2S_CN)
	free_irq(ant_info->irq, ant_info);
#else
	irq_set_irq_wake(ant_info->irq, 0);
	device_init_wakeup(&pdev->dev, false);
#endif
#endif
free_gpio:
	gpio_free(ant_info->irq_gpio);
unregister_input:
	input_unregister_device(ant_info->ipdev);
put_drvdata_free:
	platform_set_drvdata(pdev, NULL);
free_struct:
	kfree(ant_info);
	pr_err("failed\n");
	return rc;
}

static int ant_remove(struct platform_device *pdev)
{
	struct ant_check_info *ant = platform_get_drvdata(pdev);

	if (!ant)
		return 0;

#ifdef CONFIG_ANT_SYS
	if (ant->ant_sys_registered) {
		class_remove_file(&ant->ant_sys_class, &ant_state);
		class_unregister(&ant->ant_sys_class);
	}
#endif

#if defined(CONFIG_KERNEL_DRIVER_D2S_CN)
	disable_irq(ant->irq);
	free_irq(ant->irq, ant);
#else
	irq_set_irq_wake(ant->irq, 0);
	device_init_wakeup(&pdev->dev, false);
#endif

	if (gpio_is_valid(ant->irq_gpio))
		gpio_free(ant->irq_gpio);

	input_unregister_device(ant->ipdev);

	platform_set_drvdata(pdev, NULL);
	global_ant_info = NULL;
	kfree(ant);

	return 0;
}

static const struct of_device_id sn_match_table[] = {
	{ .compatible = "ant_check", },
	{ },
};
MODULE_DEVICE_TABLE(of, sn_match_table);

static struct platform_driver ant_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "ant_check",
		.of_match_table	= of_match_ptr(sn_match_table),
	},
	.probe		= ant_probe,
	.remove	= ant_remove,
};

static int __init ant_init(void)
{
	return platform_driver_register(&ant_driver);
}

static void __exit ant_exit(void)
{
	platform_driver_unregister(&ant_driver);
}

module_init(ant_init);
module_exit(ant_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("lisuyang");
