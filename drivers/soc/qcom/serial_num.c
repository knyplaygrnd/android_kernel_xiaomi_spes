// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/mod_devicetable.h>

#define sn_readl(drvdata, off)		__raw_readl((drvdata)->base + (off))
#define fuse_readl(drvdata, off)	__raw_readl((drvdata)->fuse_base + (off))

#define SERIAL_NUM	(0x000)

struct sn_drvdata {
	void __iomem	*base;
	void __iomem	*fuse_base;
	struct device	*dev;

	u32		sn;
	u32		fuse_state1;
	u32		fuse_state2;
	u32		fuse_state3;

	struct proc_dir_entry *proc_sn;
	struct proc_dir_entry *proc_fuse;
};

static int sn_read(struct seq_file *m, void *v)
{
	struct sn_drvdata *drvdata = m->private;

	if (!drvdata)
		return -ENODEV;

	if (drvdata->sn == 0)
		drvdata->sn = sn_readl(drvdata, SERIAL_NUM);

	dev_dbg(drvdata->dev, "serial num: %x\n", drvdata->sn);
	seq_printf(m, "0x%08x\n", drvdata->sn);

	return 0;
}

static int sn_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sn_read, PDE_DATA(inode));
}

static const struct file_operations sn_fops = {
	.open		= sn_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int fuse_read(struct seq_file *m, void *v)
{
	struct sn_drvdata *drvdata = m->private;

	if (!drvdata)
		return -ENODEV;

	if (drvdata->fuse_state1 == 0)
		drvdata->fuse_state1 = fuse_readl(drvdata, SERIAL_NUM);
	if (drvdata->fuse_state2 == 0)
		drvdata->fuse_state2 = fuse_readl(drvdata, 4);
	if (drvdata->fuse_state3 == 0)
		drvdata->fuse_state3 = fuse_readl(drvdata, 8);

	dev_dbg(drvdata->dev, "fuse state: 0x%x,0x%x,0x%x\n",
		drvdata->fuse_state1, drvdata->fuse_state2, drvdata->fuse_state3);
	seq_printf(m, "0x%x,0x%x,0x%x\n",
		   drvdata->fuse_state1, drvdata->fuse_state2, drvdata->fuse_state3);

	return 0;
}

static int fuse_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fuse_read, PDE_DATA(inode));
}

static const struct file_operations fuse_fops = {
	.open		= fuse_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int sn_create_proc(struct sn_drvdata *drvdata)
{
	drvdata->proc_sn = proc_create_data("serial_num", 0, NULL, &sn_fops, drvdata);
	if (!drvdata->proc_sn)
		return -ENOMEM;

	drvdata->proc_fuse = proc_create_data("fuse_state", 0, NULL, &fuse_fops, drvdata);
	if (!drvdata->proc_fuse) {
		proc_remove(drvdata->proc_sn);
		drvdata->proc_sn = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void sn_remove_proc(struct sn_drvdata *drvdata)
{
	if (!drvdata)
		return;

	if (drvdata->proc_fuse) {
		proc_remove(drvdata->proc_fuse);
		drvdata->proc_fuse = NULL;
	}
	if (drvdata->proc_sn) {
		proc_remove(drvdata->proc_sn);
		drvdata->proc_sn = NULL;
	}
}

static int sn_fuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sn_drvdata *drvdata;
	struct resource *res;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = dev;
	platform_set_drvdata(pdev, drvdata);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sn-base");
	if (!res) {
		dev_err(dev, "missing sn-base resource\n");
		return -ENODEV;
	}

	drvdata->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->base) {
		dev_err(dev, "failed to ioremap sn-base\n");
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fuse-state");
	if (!res) {
		dev_err(dev, "missing fuse-state resource\n");
		return -ENODEV;
	}

	drvdata->fuse_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->fuse_base) {
		dev_err(dev, "failed to ioremap fuse-state\n");
		return -ENOMEM;
	}

	drvdata->sn = 0;
	drvdata->fuse_state1 = 0;
	drvdata->fuse_state2 = 0;
	drvdata->fuse_state3 = 0;

	ret = sn_create_proc(drvdata);
	if (ret) {
		dev_err(dev, "failed to create proc entries, ret=%d\n", ret);
		return ret;
	}

	drvdata->sn = sn_readl(drvdata, SERIAL_NUM);
	dev_info(dev, "serial num: 0x%08x\n", drvdata->sn);
	dev_info(dev, "SN interface initialized\n");

	return 0;
}

static int sn_fuse_remove(struct platform_device *pdev)
{
	struct sn_drvdata *drvdata = platform_get_drvdata(pdev);

	sn_remove_proc(drvdata);
	if (drvdata) {
		drvdata->sn = 0;
		drvdata->fuse_state1 = 0;
		drvdata->fuse_state2 = 0;
		drvdata->fuse_state3 = 0;
	}

	dev_info(&pdev->dev, "sn_fuse driver removed\n");
	return 0;
}

static const struct of_device_id sn_fuse_match[] = {
	{ .compatible = "qcom,sn-fuse", },
	{ },
};
MODULE_DEVICE_TABLE(of, sn_fuse_match);

static struct platform_driver sn_fuse_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "msm-sn-fuse",
		.of_match_table	= of_match_ptr(sn_fuse_match),
	},
	.probe  = sn_fuse_probe,
	.remove = sn_fuse_remove,
};

static int __init sn_fuse_init(void)
{
	return platform_driver_register(&sn_fuse_driver);
}
arch_initcall(sn_fuse_init);

static void __exit sn_fuse_exit(void)
{
	platform_driver_unregister(&sn_fuse_driver);
}
module_exit(sn_fuse_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("JTag Fuse driver");
