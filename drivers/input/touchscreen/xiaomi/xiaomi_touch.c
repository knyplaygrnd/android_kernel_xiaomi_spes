/*************************************************************************
 @Author: zengrui
 @Created Time : 2021年10月09日 星期六 16时22分35秒
 @File Name: xiaomi_touch.c
 @Description:
 ************************************************************************/

#include "xiaomi_touch.h"

static struct xiaomi_touch_pdata *touch_pdata;

static int xiaomi_touch_dev_open(struct inode *inode, struct file *file)
{
	struct xiaomi_touch *touch_dev;
	struct xiaomi_touch_pdata *pdata;
	int minor = MINOR(inode->i_rdev);

	pr_info("%s\n", __func__);

	touch_dev = xiaomi_touch_dev_get(minor);
	if (!touch_dev) {
		pr_err("%s: cant get dev\n", __func__);
		return -ENOMEM;
	}

	pdata = dev_get_drvdata(touch_dev->dev);
	file->private_data = pdata;

	return 0;
}

static ssize_t xiaomi_touch_dev_read(struct file *file, char __user *buf,
				     size_t count, loff_t *pos)
{
	return 0;
}

static ssize_t xiaomi_touch_dev_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *pos)
{
	return 0;
}

static unsigned int xiaomi_touch_dev_poll(struct file *file, poll_table *wait)
{
	return 0;
}

static long xiaomi_touch_dev_ioctl(struct file *file,
				   unsigned int cmd,
				   unsigned long arg)
{
	int err = 0;
	int buf[VALUE_TYPE_SIZE] = {0, };
	struct xiaomi_touch_pdata *pdata;
	struct xiaomi_touch_interface *touch_data;
	struct xiaomi_touch *touch_dev;
	void __user *argp = (void __user *)arg;
	int user_cmd = _IOC_NR(cmd);

	if (!file)
		return -ENODEV;

	pdata = file->private_data;
	if (!pdata)
		return -ENODEV;

	touch_data = pdata->touch_data;
	touch_dev = &pdata->touch_dev;
	if (!touch_data || !touch_dev)
		return -ENODEV;

	if (copy_from_user(buf, argp, sizeof(buf)))
		return -EFAULT;

	pr_info("%s: cmd: %d, mode: %d, value: %d\n",
			__func__, user_cmd, buf[1], buf[2]);

	mutex_lock(&touch_dev->mutex);

	switch (user_cmd) {
	case SET_CUR_VALUE:
		if (touch_data->setModeValue) {
			int r = touch_data->setModeValue(buf[1], buf[2]);
			if (r < 0)
				err = r;
			else
				buf[0] = r;
		} else {
			err = -ENOTTY;
		}
		break;
	case GET_CUR_VALUE:
	case GET_DEF_VALUE:
	case GET_MIN_VALUE:
	case GET_MAX_VALUE:
		if (touch_data->getModeValue) {
			int r = touch_data->getModeValue(buf[1], user_cmd);
			if (r < 0)
				err = r;
			else
				buf[0] = r;
		} else {
			err = -ENOTTY;
		}
		break;
	case RESET_MODE:
		if (touch_data->resetMode) {
			int r = touch_data->resetMode(buf[1]);
			if (r < 0)
				err = r;
			else
				buf[0] = r;
		} else {
			err = -ENOTTY;
		}
		break;
	case GET_MODE_VALUE:
		if (touch_data->getModeAll) {
			int r = touch_data->getModeAll(buf[1], buf);
			if (r < 0)
				err = r;
		} else {
			err = -ENOTTY;
		}
		break;
	default:
		pr_err("%s: unsupported ioctl cmd %d\n", __func__, user_cmd);
		err = -EINVAL;
		break;
	}

	mutex_unlock(&touch_dev->mutex);

	if (!err) {
		if (copy_to_user(argp, buf, sizeof(buf)))
			err = -EFAULT;
	} else {
		pr_err("%s: driver operation failed: %d\n", __func__, err);
	}

	return err;
}

static int xiaomi_touch_dev_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		file->private_data = NULL;

	return 0;
}

static const struct file_operations xiaomitouch_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= xiaomi_touch_dev_open,
	.read		= xiaomi_touch_dev_read,
	.write		= xiaomi_touch_dev_write,
	.poll		= xiaomi_touch_dev_poll,
	.unlocked_ioctl	= xiaomi_touch_dev_ioctl,
	.compat_ioctl	= xiaomi_touch_dev_ioctl,
	.release	= xiaomi_touch_dev_release,
	.llseek		= no_llseek,
};

struct xiaomi_touch *xiaomi_touch_dev_get(int minor)
{
	if (touch_pdata &&
	    touch_pdata->touch_dev.misc_dev.minor == minor)
		return &touch_pdata->touch_dev;

	return NULL;
}

struct class *get_xiaomi_touch_class(void)
{
	return touch_pdata ? touch_pdata->touch_dev.class : NULL;
}

struct device *get_xiaomi_touch_dev(void)
{
	return touch_pdata ? touch_pdata->touch_dev.dev : NULL;
}

int xiaomitouch_register_modedata(struct xiaomi_touch_interface *data)
{
	struct xiaomi_touch_interface *touch_data;

	if (!touch_pdata) {
		pr_err("%s: touch_pdata not ready\n", __func__);
		return -ENOMEM;
	}

	touch_data = touch_pdata->touch_data;
	if (!touch_data) {
		pr_err("%s: touch_data not allocated\n", __func__);
		return -ENOMEM;
	}

	pr_info("%s\n", __func__);

	mutex_lock(&touch_pdata->touch_dev.mutex);

	touch_data->setModeValue = data->setModeValue;
	touch_data->getModeValue = data->getModeValue;
	touch_data->resetMode = data->resetMode;
	touch_data->getModeAll = data->getModeAll;
	touch_data->palm_sensor_read = data->palm_sensor_read;
	touch_data->palm_sensor_write = data->palm_sensor_write;

	mutex_unlock(&touch_pdata->touch_dev.mutex);
	return 0;
}

int update_palm_sensor_value(int value)
{
	if (!touch_pdata)
		return -ENODEV;

	mutex_lock(&touch_pdata->touch_dev.palm_mutex);

	if (value != touch_pdata->palm_value) {
		pr_info("%s: value: %d\n", __func__, value);
		touch_pdata->palm_value   = value;
		touch_pdata->palm_changed = true;
		sysfs_notify(&touch_pdata->touch_dev.dev->kobj,
			     NULL, "palm_sensor");
	}

	mutex_unlock(&touch_pdata->touch_dev.palm_mutex);
	return 0;
}

static ssize_t palm_sensor_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);

	pdata->palm_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->palm_value);
}

static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	if (pdata->touch_data->palm_sensor_write) {
		pdata->touch_data->palm_sensor_write(!!input);
	} else {
		pr_err("%s: has not implement\n", __func__);
	}

	pr_info("%s: value: %d\n", __func__, !!input);
	return count;
}
static DEVICE_ATTR(palm_sensor, (0664), palm_sensor_show, palm_sensor_store);

static struct attribute *touch_attr_group[] = {
	&dev_attr_palm_sensor.attr,
	NULL,
};

static int xiaomi_touch_parse_dt(struct device *dev,
				 struct xiaomi_touch_pdata *data)
{
	int ret;
	struct device_node *np;

	np = dev->of_node;
	if (!np)
		return -ENODEV;

	ret = of_property_read_string(np, "touch,name", &data->name);
	if (ret)
		return ret;

	pr_info("%s: touch,name: %s\n", __func__, data->name);
	return 0;
}

static int xiaomi_touch_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct xiaomi_touch_pdata *pdata;
	struct xiaomi_touch *touch_dev;

	pr_info("%s: enter\n", __func__);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	touch_dev = &pdata->touch_dev;
	mutex_init(&touch_dev->mutex);
	mutex_init(&touch_dev->palm_mutex);
	mutex_init(&touch_dev->psensor_mutex);
	init_waitqueue_head(&touch_dev->wait_queue);

	ret = xiaomi_touch_parse_dt(dev, pdata);
	if (ret < 0) {
		pr_err("%s: parse dt error: %d\n", __func__, ret);
		goto parse_dt_err;
	}

	touch_dev->misc_dev.minor = MISC_DYNAMIC_MINOR;
	touch_dev->misc_dev.name = "xiaomi-touch";
	touch_dev->misc_dev.fops = &xiaomitouch_dev_fops;
	touch_dev->misc_dev.parent = NULL;

	ret = misc_register(&touch_dev->misc_dev);
	if (ret) {
		pr_err("%s: create misc device err: %d\n", __func__, ret);
		goto parse_dt_err;
	}

	touch_dev->class = class_create(THIS_MODULE, "touch");
	if (!touch_dev->class) {
		pr_err("%s: create device class err\n", __func__);
		goto class_create_err;
	}

	touch_dev->dev = device_create(touch_dev->class, NULL, 'T',
				       NULL, "touch_dev");
	if (!touch_dev->dev) {
		pr_err("%s: create device dev err\n", __func__);
		goto device_create_err;
	}

	pdata->touch_data = kzalloc(sizeof(struct xiaomi_touch_interface),
				    GFP_KERNEL);
	if (!pdata->touch_data) {
		ret = -ENOMEM;
		pr_err("%s: alloc mem for touch_data\n", __func__);
		goto data_mem_err;
	}

	dev_set_drvdata(touch_dev->dev, pdata);
	platform_set_drvdata(pdev, pdata);

	touch_pdata = pdata;

	touch_dev->attrs.attrs = touch_attr_group;
	ret = sysfs_create_group(&touch_dev->dev->kobj, &touch_dev->attrs);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!: %d\n",
		       __func__, ret);
		ret = -ENODEV;
		goto sys_group_err;
	}

	pr_info("%s: over\n", __func__);
	return 0;

sys_group_err:
	kfree(pdata->touch_data);
	pdata->touch_data = NULL;
data_mem_err:
	device_destroy(touch_dev->class, touch_dev->dev->devt);
device_create_err:
	class_destroy(touch_dev->class);
class_create_err:
	misc_deregister(&touch_dev->misc_dev);
parse_dt_err:
	mutex_destroy(&touch_dev->mutex);
	mutex_destroy(&touch_dev->palm_mutex);
	mutex_destroy(&touch_dev->psensor_mutex);
	touch_pdata = NULL;
	pr_err("%s: fail!\n", __func__);
	return ret;
}

static int xiaomi_touch_remove(struct platform_device *pdev)
{
	struct xiaomi_touch_pdata *pdata = platform_get_drvdata(pdev);
	struct xiaomi_touch *touch_dev;

	if (!pdata)
		return 0;

	touch_dev = &pdata->touch_dev;

	if (touch_dev->dev && touch_dev->attrs.attrs)
		sysfs_remove_group(&touch_dev->dev->kobj, &touch_dev->attrs);

	if (touch_dev->dev) {
		device_destroy(touch_dev->class, touch_dev->dev->devt);
		touch_dev->dev = NULL;
	}

	if (touch_dev->class) {
		class_destroy(touch_dev->class);
		touch_dev->class = NULL;
	}

	misc_deregister(&touch_dev->misc_dev);

	if (pdata->touch_data) {
		kfree(pdata->touch_data);
		pdata->touch_data = NULL;
	}

	mutex_destroy(&touch_dev->mutex);
	mutex_destroy(&touch_dev->palm_mutex);
	mutex_destroy(&touch_dev->psensor_mutex);
	touch_pdata = NULL;
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id xiaomi_touch_of_match[] = {
	{ .compatible = "xiaomi-touch", },
	{ },
};
MODULE_DEVICE_TABLE(of, xiaomi_touch_of_match);

static struct platform_driver xiaomi_touch_device_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "xiaomi-touch",
		.of_match_table	= of_match_ptr(xiaomi_touch_of_match),
	},
	.probe		= xiaomi_touch_probe,
	.remove	= xiaomi_touch_remove,
};

static int __init xiaomi_touch_init(void)
{
	return platform_driver_register(&xiaomi_touch_device_driver);
}

static void __exit xiaomi_touch_exit(void)
{
	platform_driver_unregister(&xiaomi_touch_device_driver);
}

subsys_initcall(xiaomi_touch_init);
module_exit(xiaomi_touch_exit);
