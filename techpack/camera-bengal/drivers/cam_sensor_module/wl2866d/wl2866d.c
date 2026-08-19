// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "[wl2866d]: %s: " fmt, __func__

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/srcu.h>
#include "wl2866d.h"

#define WL2866D_MISC_MINOR	250
#define WL2866D_IO_REG_LIMIT	20
#define WL2866D_IO_BUFFER_LIMIT	128
#define WL2866D_MAX_CONFIG_NUM	16

#define WL2866D_REG_DISCHARGE	0x02
#define WL2866D_REG_OUT_EN	0x0E

static const struct {
	u8 reg;
	u8 default_val;
} wl2866d_rail_cfg[] = {
	[OUT_DVDD1] = { 0x03, 0x64 },  /* 1.20 V */
	[OUT_DVDD2] = { 0x04, 0x4B },  /* 1.05 V */
	[OUT_AVDD1] = { 0x05, 0x80 },  /* 2.80 V */
	[OUT_AVDD2] = { 0x06, 0x80 },  /* 2.80 V */
};

struct reg_value {
	u8 u8Add;
	u8 u8Val;
};

struct wl2866d_device {
	struct miscdevice	misc_dev;
	struct regmap		*regmap;
	struct device		*dev;
	struct gpio_desc	*en_gpiod;
	u8			chip_id;
	u8			id_reg;
	u8			id_val;
	u8			id_val1;
	u8			init_num;
	struct reg_value	inits[WL2866D_MAX_CONFIG_NUM];
	u32			offset;
	bool			on;
	struct mutex		lock;
	char			*io_buf;
	size_t			io_buf_size;
	char			misc_name[32];
};

struct wl2866d_domain {
	struct srcu_struct		 srcu;
	struct wl2866d_device __rcu	*active;
};

struct wl2866d_lock_ctx {
	struct wl2866d_domain	*dom;
	struct wl2866d_device	*wdev;
	int			 srcu_idx;
};

static struct wl2866d_domain wl2866d_dom;

static bool wl2866d_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config wl2866d_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0xFF,
	.cache_type	= REGCACHE_FLAT,
	.volatile_reg	= wl2866d_volatile_reg,
};

static int wl2866d_lock(struct wl2866d_domain *dom,
			struct wl2866d_lock_ctx *ctx)
{
	ctx->dom = dom;
	ctx->srcu_idx = srcu_read_lock(&dom->srcu);

	ctx->wdev = srcu_dereference(dom->active, &dom->srcu);
	if (!ctx->wdev || !READ_ONCE(ctx->wdev->on)) {
		srcu_read_unlock(&dom->srcu, ctx->srcu_idx);
		return -ENODEV;
	}

	return 0;
}

static void wl2866d_unlock(struct wl2866d_lock_ctx *ctx)
{
	srcu_read_unlock(&ctx->dom->srcu, ctx->srcu_idx);
	ctx->wdev = NULL;
}

static int wl2866d_write_reg(struct wl2866d_device *wdev, u8 reg, u8 val)
{
	int ret = regmap_write(wdev->regmap, reg, val);

	if (ret)
		pr_err("write error: reg=0x%02x val=0x%02x ret=%d\n",
		       reg, val, ret);
	return ret;
}

static int wl2866d_read_reg(struct wl2866d_device *wdev, u8 reg, u8 *val)
{
	unsigned int tmp;
	int ret = regmap_read(wdev->regmap, reg, &tmp);

	if (ret) {
		pr_err("read error: reg=0x%02x ret=%d\n", reg, ret);
		return ret;
	}

	*val = (u8)tmp;
	return 0;
}

static int wl2866d_read_reg_bypass(struct wl2866d_device *wdev,
				   u8 reg, u8 *val)
{
	unsigned int tmp;
	int ret;

	regcache_cache_bypass(wdev->regmap, true);
	ret = regmap_read(wdev->regmap, reg, &tmp);
	regcache_cache_bypass(wdev->regmap, false);

	if (ret) {
		pr_err("bypass read error: reg=0x%02x ret=%d\n", reg, ret);
		return ret;
	}

	*val = (u8)tmp;
	return 0;
}

static int __wl2866d_camera_power_control(struct wl2866d_device *wdev,
					  unsigned int out_iotype,
					  int is_power_on)
{
	u8 en_val, dis_val, vset;
	int ret;

	if (!wdev || !READ_ONCE(wdev->on))
		return -ENODEV;

	if (out_iotype > OUT_AVDD2) {
		pr_err("invalid out_iotype %u\n", out_iotype);
		return -EINVAL;
	}

	ret = wl2866d_read_reg(wdev, WL2866D_REG_OUT_EN, &en_val);
	if (ret)
		return ret;

	ret = wl2866d_read_reg(wdev, WL2866D_REG_DISCHARGE, &dis_val);
	if (ret)
		return ret;

	if (is_power_on) {
		if (!en_val)
			gpiod_set_value_cansleep(wdev->en_gpiod, 1);

		vset = wl2866d_rail_cfg[out_iotype].default_val;
		if (out_iotype == OUT_DVDD2) {
			if (is_power_on == 1050000)
				vset = 0x4B;
			else if (is_power_on == 1200000)
				vset = 0x64;
		}

		ret = wl2866d_write_reg(wdev,
					wl2866d_rail_cfg[out_iotype].reg, vset);
		if (ret) {
			pr_err("set voltage failed for rail %u\n", out_iotype);
			if (!en_val)
				gpiod_set_value_cansleep(wdev->en_gpiod, 0);
			return ret;
		}

		dis_val &= ~(u8)BIT(out_iotype);
		ret = wl2866d_write_reg(wdev, WL2866D_REG_DISCHARGE, dis_val);
		if (ret)
			return ret;

		en_val |= (u8)BIT(out_iotype);
		ret = wl2866d_write_reg(wdev, WL2866D_REG_OUT_EN, en_val);
	} else {
		en_val &= ~(u8)BIT(out_iotype);
		ret = wl2866d_write_reg(wdev, WL2866D_REG_OUT_EN, en_val);
		if (ret)
			return ret;

		dis_val |= (u8)BIT(out_iotype);
		ret = wl2866d_write_reg(wdev, WL2866D_REG_DISCHARGE, dis_val);
		if (ret)
			return ret;

		if (!en_val)
			gpiod_set_value_cansleep(wdev->en_gpiod, 0);
	}

	return ret;
}

static int wl2866d_camera_power_control(struct wl2866d_lock_ctx *ctx,
					unsigned int out_iotype,
					int is_power_on)
{
	struct wl2866d_device *wdev = ctx->wdev;
	int ret;

	if (WARN_ON(!wdev))
		return -ENODEV;

	mutex_lock(&wdev->lock);
	ret = __wl2866d_camera_power_control(wdev, out_iotype, is_power_on);
	mutex_unlock(&wdev->lock);

	return ret;
}

int wl2866d_power_control(unsigned int out_iotype, int is_power_on)
{
	struct wl2866d_lock_ctx ctx;
	int ret;

	ret = wl2866d_lock(&wl2866d_dom, &ctx);
	if (ret)
		return ret;

	ret = wl2866d_camera_power_control(&ctx, out_iotype, is_power_on);
	wl2866d_unlock(&ctx);
	return ret;
}
EXPORT_SYMBOL_GPL(wl2866d_power_control);

static char hex_to_char(u8 val)
{
	val &= 0x0F;
	return (val >= 10) ? (val - 10 + 'A') : (val + '0');
}

static u8 char_to_hex(char ch)
{
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	return 0;
}

static int wl2866d_open(struct inode *inode, struct file *file)
{
	struct wl2866d_device *wdev =
		container_of(file->private_data, struct wl2866d_device, misc_dev);

	mutex_lock(&wdev->lock);
	if (!READ_ONCE(wdev->on)) {
		mutex_unlock(&wdev->lock);
		return -ENODEV;
	}
	wdev->offset = 0;
	mutex_unlock(&wdev->lock);

	file->private_data = wdev;
	return 0;
}

static int wl2866d_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static ssize_t wl2866d_read(struct file *file, char __user *buf,
			    size_t count, loff_t *offset)
{
	struct wl2866d_device *wdev = file->private_data;
	int ret = 0, num = 0, i;
	u8 start, u8val;

	if (!wdev)
		return -ENODEV;

	if (count == 0 ||
	    count > WL2866D_IO_REG_LIMIT ||
	    count > wdev->io_buf_size / 6)
		return -ERANGE;

	mutex_lock(&wdev->lock);

	if (!READ_ONCE(wdev->on)) {
		mutex_unlock(&wdev->lock);
		return -ENODEV;
	}

	if (wdev->offset > 0xFF) {
		mutex_unlock(&wdev->lock);
		return -EINVAL;
	}

	start = (u8)wdev->offset;
	memset(wdev->io_buf, 0, count * 6);

	for (i = 0; i < (int)count; i++) {
		u8 addr = start + (u8)i;

		ret = wl2866d_read_reg_bypass(wdev, addr, &u8val);
		if (ret < 0) {
			mutex_unlock(&wdev->lock);
			return ret;
		}
		wdev->io_buf[num++] = hex_to_char(addr >> 4);
		wdev->io_buf[num++] = hex_to_char(addr);
		wdev->io_buf[num++] = ' ';
		wdev->io_buf[num++] = hex_to_char(u8val >> 4);
		wdev->io_buf[num++] = hex_to_char(u8val);
		wdev->io_buf[num++] = ' ';
	}

	mutex_unlock(&wdev->lock);

	if (copy_to_user(buf, wdev->io_buf, num))
		return -EFAULT;

	return (ssize_t)count;
}

static ssize_t wl2866d_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *offset)
{
	struct wl2866d_device *wdev = file->private_data;
	char *kbuf;
	int ret = 0, i;

	if (!wdev)
		return -ENODEV;

	if (count == 0 ||
	    count > WL2866D_IO_BUFFER_LIMIT ||
	    count % 6 != 0)
		return -EINVAL;

	kbuf = memdup_user(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	mutex_lock(&wdev->lock);

	if (!READ_ONCE(wdev->on)) {
		mutex_unlock(&wdev->lock);
		kfree(kbuf);
		return -ENODEV;
	}

	for (i = 0; i < (int)count; i += 6) {
		u8 addr = (char_to_hex(kbuf[i]) << 4) |
				    char_to_hex(kbuf[i + 1]);
		u8 val = (char_to_hex(kbuf[i + 3]) << 4) |
				    char_to_hex(kbuf[i + 4]);

		ret = wl2866d_write_reg(wdev, addr, val);
		if (ret < 0) {
			mutex_unlock(&wdev->lock);
			kfree(kbuf);
			return ret;
		}
	}
	mutex_unlock(&wdev->lock);

	kfree(kbuf);
	return count;
}

static loff_t wl2866d_llseek(struct file *file, loff_t offset, int whence)
{
	struct wl2866d_device *wdev = file->private_data;
	u32 new_offset;

	if (!wdev)
		return -ENODEV;

	mutex_lock(&wdev->lock);
	switch (whence) {
	case SEEK_CUR:
		new_offset = wdev->offset + (u32)offset;
		break;
	default:
		new_offset = 0;
		break;
	}
	wdev->offset = min(new_offset, (u32)0xFF);
	mutex_unlock(&wdev->lock);

	return file->f_pos;
}

static const struct file_operations wl2866d_fops = {
	.owner	 = THIS_MODULE,
	.open	 = wl2866d_open,
	.release = wl2866d_release,
	.llseek	 = wl2866d_llseek,
	.read	 = wl2866d_read,
	.write	 = wl2866d_write,
};

static int wl2866d_init_register(struct wl2866d_device *wdev)
{
	static const struct reg_sequence init_regs[] = {
		{ 0x00, 0x00 }, { 0x01, 0x00 }, { 0x02, 0x8f },
		{ 0x03, 0x64 }, { 0x04, 0x4b }, { 0x05, 0x80 },
		{ 0x06, 0x80 }, { 0x07, 0x00 }, { 0x08, 0x00 },
		{ 0x09, 0x00 }, { 0x0a, 0x00 }, { 0x0b, 0x00 },
		{ 0x0c, 0x00 }, { 0x0d, 0x00 }, { 0x0e, 0x00 },
	};
	int ret = regmap_multi_reg_write(wdev->regmap,
				    init_regs, ARRAY_SIZE(init_regs));
	if (ret)
		pr_err("init_register failed: %d\n", ret);
	return ret;
}

static int wl2866d_match_id(struct wl2866d_device *wdev)
{
	struct device_node *np = wdev->dev->of_node;
	int ret;

	ret = of_property_read_u8(np, "id_reg", &wdev->id_reg);
	if (ret) {
		pr_err("id_reg missing or invalid\n");
		return ret;
	}

	ret = of_property_read_u8(np, "id_val", &wdev->id_val);
	if (ret) {
		pr_err("id_val missing or invalid\n");
		return ret;
	}

	ret = of_property_read_u8(np, "id_val1", &wdev->id_val1);
	if (ret) {
		pr_err("id_val1 missing or invalid\n");
		return ret;
	}

	ret = wl2866d_read_reg(wdev, wdev->id_reg, &wdev->chip_id);
	if (ret) {
		pr_err("id read failed: %d\n", ret);
		return ret;
	}

	if (wdev->chip_id != wdev->id_val &&
	    wdev->chip_id != wdev->id_val1) {
		pr_err("id mismatch: chip_id=0x%02x expected=0x%02x/0x%02x\n",
		       wdev->chip_id, wdev->id_val, wdev->id_val1);
		return -ENODEV;
	}

	pr_info("chip_id=0x%02x\n", wdev->chip_id);
	return 0;
}

static int wl2866d_init_module_dev(struct wl2866d_device *wdev)
{
	struct device_node *np = wdev->dev->of_node;
	u32 inits[WL2866D_MAX_CONFIG_NUM * 2];
	u32 num = 0;
	int ret, i;

	ret = of_property_read_u32(np, "init_num", &num);
	if (ret) {
		pr_err("init_num missing or invalid\n");
		return ret;
	}

	if (num == 0 || num > WL2866D_MAX_CONFIG_NUM) {
		pr_err("init_num %u out of range [1, %d]\n",
		       num, WL2866D_MAX_CONFIG_NUM);
		return -EINVAL;
	}
	wdev->init_num = (u8)num;

	ret = of_property_read_u32_array(np, "inits", inits, num * 2);
	if (ret) {
		pr_err("inits array missing or invalid\n");
		return ret;
	}

	for (i = 0; i < (int)num; i++) {
		wdev->inits[i].u8Add = (u8)inits[i * 2];
		wdev->inits[i].u8Val = (u8)inits[i * 2 + 1];
		ret = wl2866d_write_reg(wdev,
					wdev->inits[i].u8Add,
					wdev->inits[i].u8Val);
		if (ret < 0) {
			pr_err("init write failed at index %d\n", i);
			return ret;
		}
	}

	return 0;
}

static int wl2866d_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct wl2866d_device *wdev;
	int ret;

	pr_info("entry\n");

	wdev = devm_kzalloc(&client->dev, sizeof(*wdev), GFP_KERNEL);
	if (!wdev)
		return -ENOMEM;

	wdev->dev = &client->dev;
	mutex_init(&wdev->lock);
	i2c_set_clientdata(client, wdev);

	wdev->regmap = devm_regmap_init_i2c(client, &wl2866d_regmap_config);
	if (IS_ERR(wdev->regmap)) {
		ret = PTR_ERR(wdev->regmap);
		pr_err("regmap init failed: %d\n", ret);
		goto err_out;
	}

	wdev->io_buf_size = WL2866D_IO_BUFFER_LIMIT;
	wdev->io_buf = devm_kcalloc(wdev->dev, 1,
				    wdev->io_buf_size, GFP_KERNEL);
	if (!wdev->io_buf) {
		ret = -ENOMEM;
		goto err_out;
	}

	wdev->en_gpiod = devm_gpiod_get(wdev->dev, "en", GPIOD_OUT_HIGH);
	if (IS_ERR(wdev->en_gpiod)) {
		ret = PTR_ERR(wdev->en_gpiod);
		pr_err("failed to get en gpio: %d\n", ret);
		goto err_out;
	}

	ret = wl2866d_match_id(wdev);
	if (ret) {
		pr_err("match_id failed, ret=%d\n", ret);
		goto err_en_off;
	}

	ret = wl2866d_init_register(wdev);
	if (ret) {
		pr_err("init_register failed, ret=%d\n", ret);
		goto err_en_off;
	}

	ret = wl2866d_init_module_dev(wdev);
	if (ret) {
		pr_err("init_module failed, ret=%d\n", ret);
		goto err_en_off;
	}

	gpiod_set_value_cansleep(wdev->en_gpiod, 0);
	snprintf(wdev->misc_name, sizeof(wdev->misc_name),
		 "wl2866d-%d-%04x", client->adapter->nr, client->addr);
	wdev->misc_dev.minor = WL2866D_MISC_MINOR;
	wdev->misc_dev.name = wdev->misc_name;
	wdev->misc_dev.fops = &wl2866d_fops;

	ret = misc_register(&wdev->misc_dev);
	if (ret) {
		pr_err("misc_register failed (%d)\n", ret);
		goto err_out;
	}

	WRITE_ONCE(wdev->on, true);
	rcu_assign_pointer(wl2866d_dom.active, wdev);

	pr_info("probe succeeded, dev=%s\n", wdev->misc_name);
	return 0;

err_en_off:
	gpiod_set_value_cansleep(wdev->en_gpiod, 0);
err_out:
	mutex_destroy(&wdev->lock);
	i2c_set_clientdata(client, NULL);
	pr_err("probe failed, ret=%d\n", ret);
	return ret;
}

static int wl2866d_remove(struct i2c_client *client)
{
	struct wl2866d_device *wdev = i2c_get_clientdata(client);

	if (!wdev)
		return 0;

	rcu_assign_pointer(wl2866d_dom.active, NULL);
	WRITE_ONCE(wdev->on, false);
	synchronize_srcu(&wl2866d_dom.srcu);
	misc_deregister(&wdev->misc_dev);
	gpiod_set_value_cansleep(wdev->en_gpiod, 0);
	mutex_destroy(&wdev->lock);
	i2c_set_clientdata(client, NULL);

	pr_info("remove succeeded\n");
	return 0;
}

static const struct of_device_id wl2866d_of_match[] = {
	{ .compatible = "ovti,wl2866d-i2c", },
	{ },
};
MODULE_DEVICE_TABLE(of, wl2866d_of_match);

static const struct i2c_device_id wl2866d_id[] = {
	{ "ovti,wl2866d-i2c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wl2866d_id);

static struct i2c_driver wl2866d_i2c_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "ovti,wl2866d-i2c",
		.of_match_table	= of_match_ptr(wl2866d_of_match),
	},
	.probe		= wl2866d_probe,
	.remove		= wl2866d_remove,
	.id_table	= wl2866d_id,
};

static int __init cam_wl2866_init_module(void)
{
	int ret;

	ret = init_srcu_struct(&wl2866d_dom.srcu);
	if (ret) {
		pr_err("init_srcu_struct failed (%d)\n", ret);
		return ret;
	}

	ret = i2c_add_driver(&wl2866d_i2c_driver);
	if (ret) {
		pr_err("i2c_add_driver failed (%d)\n", ret);
		cleanup_srcu_struct(&wl2866d_dom.srcu);
		return ret;
	}

	return 0;
}

static void __exit cam_wl2866_exit_module(void)
{
	i2c_del_driver(&wl2866d_i2c_driver);
	cleanup_srcu_struct(&wl2866d_dom.srcu);
}

subsys_initcall(cam_wl2866_init_module);
module_exit(cam_wl2866_exit_module);

MODULE_DESCRIPTION("WL2866D Power IC Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
