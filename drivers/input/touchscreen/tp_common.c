// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/input/tp_common.h>

bool capacitive_keys_enabled;
struct kobject *touchpanel_kobj;

struct tp_feature_entry {
	const char *name;
	struct kobj_attribute kattr;
	bool registered;
};

static struct tp_feature_entry tp_features[TP_FEATURE_MAX] = {
	[TP_FEATURE_CAPACITIVE_KEYS] = { .name = "capacitive_keys" },
	[TP_FEATURE_DOUBLE_TAP] = { .name = "double_tap" },
	[TP_FEATURE_REVERSED_KEYS] = { .name = "reversed_keys" },
};

int tp_common_set_ops(enum tp_feature feature, struct tp_common_ops *ops)
{
	struct tp_feature_entry *entry;
	int rc;

	if (feature >= TP_FEATURE_MAX || !ops)
		return -EINVAL;

	if (!ops->show && !ops->store)
		return -EINVAL;

	entry = &tp_features[feature];

	if (entry->registered) {
		sysfs_remove_file(touchpanel_kobj, &entry->kattr.attr);
		entry->registered = false;
	}

	entry->kattr = (struct kobj_attribute)
		__ATTR(dummy, (S_IWUSR | S_IRUGO), ops->show, ops->store);
	entry->kattr.attr.name = entry->name;

	rc = sysfs_create_file(touchpanel_kobj, &entry->kattr.attr);
	if (!rc)
		entry->registered = true;

	return rc;
}

void tp_common_remove_ops(enum tp_feature feature)
{
	struct tp_feature_entry *entry;

	if (feature >= TP_FEATURE_MAX)
		return;

	entry = &tp_features[feature];

	if (entry->registered) {
		sysfs_remove_file(touchpanel_kobj, &entry->kattr.attr);
		entry->registered = false;
	}
}

static int __init tp_common_init(void)
{
	touchpanel_kobj = kobject_create_and_add("touchpanel", NULL);
	if (!touchpanel_kobj)
		return -ENOMEM;

	return 0;
}

static void __exit tp_common_exit(void)
{
	int i;

	for (i = 0; i < TP_FEATURE_MAX; i++)
		tp_common_remove_ops(i);

	if (touchpanel_kobj) {
		kobject_del(touchpanel_kobj);
		kobject_put(touchpanel_kobj);
		touchpanel_kobj = NULL;
	}
}

core_initcall(tp_common_init);
module_exit(tp_common_exit);
