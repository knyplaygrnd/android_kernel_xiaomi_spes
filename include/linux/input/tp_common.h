/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TP_COMMON_H__
#define __TP_COMMON_H__

#include <linux/kobject.h>

extern bool capacitive_keys_enabled;
extern struct kobject *touchpanel_kobj;

struct tp_common_ops {
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);
};

/* Enum identifier type */
enum tp_feature {
	TP_FEATURE_CAPACITIVE_KEYS,
	TP_FEATURE_DOUBLE_TAP,
	TP_FEATURE_REVERSED_KEYS,
	TP_FEATURE_MAX,
};

int tp_common_set_ops(enum tp_feature feature, struct tp_common_ops *ops);
void tp_common_remove_ops(enum tp_feature feature);

#endif /* _TP_COMMON_H */
