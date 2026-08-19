// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cpufreq.h>
#include <linux/smp.h>
#include <linux/cpumask.h>

static struct proc_dir_entry *dentry;

static int cpumaxfreq_read(struct seq_file *m, void *v)
{
	struct cpufreq_policy policy;
	int cpu;
	unsigned int max_khz = 0;
	bool found = false;

	for_each_possible_cpu(cpu) {
		if (cpufreq_get_policy(&policy, cpu) == 0) {
			if (!found || policy.cpuinfo.max_freq > max_khz)
				max_khz = policy.cpuinfo.max_freq;
			found = true;
		}
	}

	if (!found)
		seq_puts(m, "N/A\n");
	else
		seq_printf(m, "%u.%02u\n", max_khz / 1000, (max_khz % 1000) / 10);

	return 0;
}

static int cpumaxfreq_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpumaxfreq_read, NULL);
}

static const struct file_operations cpumaxfreq_fops = {
	.owner = THIS_MODULE,
	.open = cpumaxfreq_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init cpumaxfreq_init(void)
{
	dentry = proc_create("cpumaxfreq", 0444, NULL, &cpumaxfreq_fops);
	if (!dentry) {
		pr_err("cpumaxfreq: failed to create /proc/cpumaxfreq\n");
		return -ENOMEM;
	}
	pr_info("cpumaxfreq: created /proc/cpumaxfreq\n");
	return 0;
}

static void __exit cpumaxfreq_exit(void)
{
	if (dentry)
		proc_remove(dentry);
	pr_info("cpumaxfreq: removed\n");
}

module_init(cpumaxfreq_init);
module_exit(cpumaxfreq_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Return the max frequency supported by any core");
