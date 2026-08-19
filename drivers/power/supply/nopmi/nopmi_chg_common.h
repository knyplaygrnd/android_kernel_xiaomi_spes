/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nopmi_chg_common - Base types, IC detection, and stateless helpers
 *
 * This header is the foundation of the nopmi charger driver stack.
 * It deliberately contains NO resource ownership — no PSY handles,
 * no votable pointers, no struct definitions tied to a specific driver
 * instance. It is safe to include from any layer.
 *
 */

#if !defined(__NOPMI_CHG_COMMON_H__)
#define __NOPMI_CHG_COMMON_H__

#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pmic-voter.h>

enum quick_charge_type {
	QUICK_CHARGE_NORMAL = 0,
	QUICK_CHARGE_FAST,
	QUICK_CHARGE_FLASH,
	QUICK_CHARGE_TURBE,
	QUICK_CHARGE_MAX,
};

struct quick_charge {
	enum power_supply_type adap_type;
	enum quick_charge_type adap_cap;
};

extern int max77729_usbc_is_pd_verified(void);
extern int adapter_dev_get_pd_verified(void);

int nopmi_chg_is_usb_present(struct power_supply *usb_psy);

int nopmi_set_charger_ic_type(enum nopmi_charger_ic_type nopmi_type);
enum nopmi_charger_ic_type nopmi_get_charger_ic_type(void);

int nopmi_set_charge_enable(bool en);
int nopmi_get_quick_charge_type(struct power_supply *usb_psy);

#endif /* __NOPMI_CHG_COMMON_H__ */
