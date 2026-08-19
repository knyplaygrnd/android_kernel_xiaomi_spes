/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Richtek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __LINUX_RT1711H_H__
#define __LINUX_RT1711H_H__

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/string.h>
#include <linux/gpio/consumer.h>
#include "std_tcpci_v10.h"
#include "pd_dbg_info.h"

#ifdef CONFIG_RT_REGMAP
#include "rt-regmap.h"
#endif

#define RT1711H_NAME "rt1711h"

/*show debug message or not */
#define ENABLE_RT1711_DBG	0

struct rt1711_chip {
	struct i2c_client *client;
	struct device *dev;
#ifdef CONFIG_RT_REGMAP
	struct rt_regmap_device *m_dev;
#endif
	struct tcpc_desc *tcpc_desc;
	struct tcpc_device *tcpc;

	struct gpio_desc *irq_gpiod;
	int irq;
	int chip_id;

	struct mutex irq_lock;
	atomic_t resume_completed;
	atomic_t irq_disabled;
	atomic_t shutting_down;

	struct wakeup_source *rt1711_ws;

	bool probed;
	struct kref kref;
	struct list_head node;
	char name[32];
};

/* RT1711H Private RegMap */

#define RT1711H_REG_CONFIG_GPIO0	(0x71)

#define RT1711H_REG_PHY_CTRL1		(0x80)

#define RT1711H_REG_CLK_CTRL2		(0x87)
#define RT1711H_REG_CLK_CTRL3		(0x88)

#define RT1711H_REG_PRL_FSM_RESET	(0x8D)

#define RT1711H_REG_BMC_CTRL		(0x90)
#define RT1711H_REG_BMCIO_RXDZSEL	(0x93)
#define RT1711H_REG_VCONN_CLIMITEN	(0x95)

#define RT1711H_REG_RT_STATUS		(0x97)
#define RT1711H_REG_RT_INT		(0x98)
#define RT1711H_REG_RT_MASK		(0x99)

#define RT1711H_REG_IDLE_CTRL		(0x9B)
#define RT1711H_REG_INTRST_CTRL		(0x9C)
#define RT1711H_REG_WATCHDOG_CTRL	(0x9D)
#define RT1711H_REG_I2CRST_CTRL		(0x9E)

#define RT1711H_REG_SWRESET		(0xA0)
#define RT1711H_REG_TTCPC_FILTER	(0xA1)
#define RT1711H_REG_DRP_TOGGLE_CYCLE	(0xA2)
#define RT1711H_REG_DRP_DUTY_CTRL	(0xA3)
#define RT1711H_REG_BMCIO_RXDZEN	(0xAF)

#define RT1711H_REG_UNLOCK_PW_2		(0xF0)
#define RT1711H_REG_UNLOCK_PW_1		(0xF1)
#define RT1711H_REG_EFUSE5		(0xF6)

/*
 * Device ID
 */

#define RT1711H_DID_A	0x2170
#define RT1711H_DID_B	0x2171
#define RT1711H_DID_C	0x2172

#define RT1715_DID_D	0x2173
#define HUSB311_DID	0x0
/*
 * RT1711H_REG_PHY_CTRL1	(0x80)
 */

#define RT1711H_REG_PHY_CTRL1_SET(retry_discard, toggle_cnt, bus_idle_cnt, rx_filter) \
	(((retry_discard) << 7) | ((toggle_cnt) << 4) | \
	((bus_idle_cnt) << 2) | ((rx_filter) & 0x03))

/*
 * RT1711H_REG_CLK_CTRL2	(0x87)
 */

#define RT1711H_REG_CLK_DIV_600K_EN	BIT(7)
#define RT1711H_REG_CLK_BCLK2_EN	BIT(6)
#define RT1711H_REG_CLK_BCLK2_TG_EN	BIT(5)
#define RT1711H_REG_CLK_DIV_300K_EN	BIT(3)
#define RT1711H_REG_CLK_CK_300K_EN	BIT(2)
#define RT1711H_REG_CLK_BCLK_EN		BIT(1)
#define RT1711H_REG_CLK_BCLK_TH_EN	BIT(0)

/*
 * RT1711H_REG_CLK_CTRL3	(0x88)
 */

#define RT1711H_REG_CLK_OSCMUX_RG_EN	BIT(7)
#define RT1711H_REG_CLK_CK_24M_EN	BIT(6)
#define RT1711H_REG_CLK_OSC_RG_EN	BIT(5)
#define RT1711H_REG_CLK_DIV_2P4M_EN	BIT(4)
#define RT1711H_REG_CLK_CK_2P4M_EN	BIT(3)
#define RT1711H_REG_CLK_PCLK_EN		BIT(2)
#define RT1711H_REG_CLK_PCLK_RG_EN	BIT(1)
#define RT1711H_REG_CLK_PCLK_TG_EN	BIT(0)

/*
 * RT1711H_REG_BMC_CTRL		(0x90)
 */

#define RT1711H_REG_IDLE_EN		BIT(6)
#define RT1711H_REG_DISCHARGE_EN	BIT(5)
#define RT1711H_REG_BMCIO_LPRPRD	BIT(4)
#define RT1711H_REG_BMCIO_LPEN		BIT(3)
#define RT1711H_REG_BMCIO_BG_EN		BIT(2)
#define RT1711H_REG_VBUS_DET_EN		BIT(1)
#define RT1711H_REG_BMCIO_OSC_EN	BIT(0)

/*
 * RT1711H_REG_RT_STATUS	(0x97)
 */

#define RT1711H_REG_RA_DETACH		BIT(5)
#define RT1711H_REG_VBUS_80		BIT(1)

/*
 * RT1711H_REG_RT_INT		(0x98)
 */

#define RT1711H_REG_INT_RA_DETACH	BIT(5)
#define RT1711H_REG_INT_WATCHDOG	BIT(2)
#define RT1711H_REG_INT_VBUS_80		BIT(1)
#define RT1711H_REG_INT_WAKEUP		BIT(0)

/*
 * RT1711H_REG_RT_MASK		(0x99)
 */

#define RT1711H_REG_M_RA_DETACH		BIT(5)
#define RT1711H_REG_M_WATCHDOG		BIT(2)
#define RT1711H_REG_M_VBUS_80		BIT(1)
#define RT1711H_REG_M_WAKEUP		BIT(0)

/*
 * RT1711H_REG_IDLE_CTRL	(0x9B)
 */

#define RT1711H_REG_CK_300K_SEL		BIT(7)
#define RT1711H_REG_SHIPPING_OFF	BIT(5)
#define RT1711H_REG_ENEXTMSG		BIT(4)
#define RT1711H_REG_AUTOIDLE_EN		BIT(3)

/*
 * RT1711H_REG_EFUSE5		(0xF6)
 */

#define RT1711H_REG_M_VBUS_CAL		GENMASK(7, 5)
#define RT1711H_REG_S_VBUS_CAL		5
#define RT1711H_REG_MIN_VBUS_CAL	-4

/* timeout = (tout*2+1) * 6.4ms */

#ifdef CONFIG_USB_PD_REV30
#define RT1711H_REG_IDLE_SET(ck300, ship_dis, auto_idle, tout) \
	(((ck300) << 7) | ((ship_dis) << 5) | \
	((auto_idle) << 3) | ((tout) & 0x07) | RT1711H_REG_ENEXTMSG)
#else
#define RT1711H_REG_IDLE_SET(ck300, ship_dis, auto_idle, tout) \
	(((ck300) << 7) | ((ship_dis) << 5) | \
	((auto_idle) << 3) | ((tout) & 0x07))
#endif

/*
 * RT1711H_REG_INTRST_CTRL	(0x9C)
 */

#define RT1711H_REG_INTRST_EN		BIT(7)

/* timeout = (tout+1) * 0.2sec */
#define RT1711H_REG_INTRST_SET(en, tout) \
	(((en) << 7) | ((tout) & 0x03))

/*
 * RT1711H_REG_WATCHDOG_CTRL	(0x9D)
 */

#define RT1711H_REG_WATCHDOG_EN		BIT(7)

/* timeout = (tout+1) * 0.4sec */
#define RT1711H_REG_WATCHDOG_CTRL_SET(en, tout)	\
	(((en) << 7) | ((tout) & 0x07))

/*
 * RT1711H_REG_I2CRST_CTRL	(0x9E)
 */

#define RT1711H_REG_I2CRST_EN		BIT(7)

/* timeout = (tout+1) * 12.5ms */
#define RT1711H_REG_I2CRST_SET(en, tout)	\
	(((en) << 7) | ((tout) & 0x0f))

#if ENABLE_RT1711_DBG
#define RT1711_INFO(fmt, ...)					\
	pd_dbg_info("%s(%d): " fmt,				\
		__func__, __LINE__, ##__VA_ARGS__)
#else
#define RT1711_INFO(fmt, ...)	((void)0)
#endif

struct rt1711_chip;

/*
 * Acquire a reference to the rt1711 instance by name.
 * - Caller must pass the exact name (RT1711H_NAME).
 * - Returns NULL if device not present, being released, or name mismatch.
 * - Returned pointer is valid until rt1711_chip_put() is called.
 */
struct rt1711_chip *rt1711_chip_get(const char *name);

/*
 * Release a reference previously acquired by rt1711_chip_get().
 */
void rt1711_chip_put(struct rt1711_chip *chip);

/*
 * Read-only helper to check whether the device is probed.
 * Safe to call only after a successful rt1711_chip_get().
 */
bool rt1711_chip_is_probed(struct rt1711_chip *chip);

#endif /* __LINUX_RT1711H_H__ */
