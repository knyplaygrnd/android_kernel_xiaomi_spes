// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Richtek Inc.
 *
 * Richtek RT1711H Type-C Port Control Driver
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/cpu.h>
#include <linux/version.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/clock.h>
#include <linux/sched/rt.h>
#include "inc/pd_dbg_info.h"
#include "inc/tcpci.h"
#include "inc/rt1711h.h"

#define RT1711H_DRV_VERSION	"2.0.7_G"

#define RT1711H_IRQ_WAKE_TIME	(500) /* ms */

static LIST_HEAD(rt1711_chip_list);
static DEFINE_MUTEX(rt1711_list_lock);

#ifdef CONFIG_RT_REGMAP
RT_REG_DECL(TCPC_V10_REG_VID, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_PID, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_DID, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TYPEC_REV, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_PD_REV, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_PDIF_REV, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_ALERT, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_ALERT_MASK, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_POWER_STATUS_MASK, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_FAULT_STATUS_MASK, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TCPC_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_ROLE_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_FAULT_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_POWER_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_CC_STATUS, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_POWER_STATUS, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_FAULT_STATUS, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_COMMAND, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_MSG_HDR_INFO, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_RX_DETECT, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_RX_BYTE_CNT, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_RX_BUF_FRAME_TYPE, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_RX_HDR, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_RX_DATA, 28, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TRANSMIT, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TX_BYTE_CNT, 1, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TX_HDR, 2, RT_VOLATILE, {});
RT_REG_DECL(TCPC_V10_REG_TX_DATA, 28, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_CONFIG_GPIO0, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_PHY_CTRL1, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_CLK_CTRL2, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_CLK_CTRL3, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_PRL_FSM_RESET, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_BMC_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_BMCIO_RXDZSEL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_VCONN_CLIMITEN, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_RT_STATUS, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_RT_INT, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_RT_MASK, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_IDLE_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_INTRST_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_WATCHDOG_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_I2CRST_CTRL, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_SWRESET, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_TTCPC_FILTER, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_DRP_TOGGLE_CYCLE, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_DRP_DUTY_CTRL, 2, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_BMCIO_RXDZEN, 1, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_UNLOCK_PW_2, 2, RT_VOLATILE, {});
RT_REG_DECL(RT1711H_REG_EFUSE5, 1, RT_VOLATILE, {});

static const rt_register_map_t rt1711_chip_regmap[] = {
	RT_REG(TCPC_V10_REG_VID),
	RT_REG(TCPC_V10_REG_PID),
	RT_REG(TCPC_V10_REG_DID),
	RT_REG(TCPC_V10_REG_TYPEC_REV),
	RT_REG(TCPC_V10_REG_PD_REV),
	RT_REG(TCPC_V10_REG_PDIF_REV),
	RT_REG(TCPC_V10_REG_ALERT),
	RT_REG(TCPC_V10_REG_ALERT_MASK),
	RT_REG(TCPC_V10_REG_POWER_STATUS_MASK),
	RT_REG(TCPC_V10_REG_FAULT_STATUS_MASK),
	RT_REG(TCPC_V10_REG_TCPC_CTRL),
	RT_REG(TCPC_V10_REG_ROLE_CTRL),
	RT_REG(TCPC_V10_REG_FAULT_CTRL),
	RT_REG(TCPC_V10_REG_POWER_CTRL),
	RT_REG(TCPC_V10_REG_CC_STATUS),
	RT_REG(TCPC_V10_REG_POWER_STATUS),
	RT_REG(TCPC_V10_REG_FAULT_STATUS),
	RT_REG(TCPC_V10_REG_COMMAND),
	RT_REG(TCPC_V10_REG_MSG_HDR_INFO),
	RT_REG(TCPC_V10_REG_RX_DETECT),
	RT_REG(TCPC_V10_REG_RX_BYTE_CNT),
	RT_REG(TCPC_V10_REG_RX_BUF_FRAME_TYPE),
	RT_REG(TCPC_V10_REG_RX_HDR),
	RT_REG(TCPC_V10_REG_RX_DATA),
	RT_REG(TCPC_V10_REG_TRANSMIT),
	RT_REG(TCPC_V10_REG_TX_BYTE_CNT),
	RT_REG(TCPC_V10_REG_TX_HDR),
	RT_REG(TCPC_V10_REG_TX_DATA),
	RT_REG(RT1711H_REG_CONFIG_GPIO0),
	RT_REG(RT1711H_REG_PHY_CTRL1),
	RT_REG(RT1711H_REG_CLK_CTRL2),
	RT_REG(RT1711H_REG_CLK_CTRL3),
	RT_REG(RT1711H_REG_PRL_FSM_RESET),
	RT_REG(RT1711H_REG_BMC_CTRL),
	RT_REG(RT1711H_REG_BMCIO_RXDZSEL),
	RT_REG(RT1711H_REG_VCONN_CLIMITEN),
	RT_REG(RT1711H_REG_RT_STATUS),
	RT_REG(RT1711H_REG_RT_INT),
	RT_REG(RT1711H_REG_RT_MASK),
	RT_REG(RT1711H_REG_IDLE_CTRL),
	RT_REG(RT1711H_REG_INTRST_CTRL),
	RT_REG(RT1711H_REG_WATCHDOG_CTRL),
	RT_REG(RT1711H_REG_I2CRST_CTRL),
	RT_REG(RT1711H_REG_SWRESET),
	RT_REG(RT1711H_REG_TTCPC_FILTER),
	RT_REG(RT1711H_REG_DRP_TOGGLE_CYCLE),
	RT_REG(RT1711H_REG_DRP_DUTY_CTRL),
	RT_REG(RT1711H_REG_BMCIO_RXDZEN),
	RT_REG(RT1711H_REG_UNLOCK_PW_2),
	RT_REG(RT1711H_REG_EFUSE5),
};

#define RT1711_CHIP_REGMAP_SIZE ARRAY_SIZE(rt1711_chip_regmap)
#endif /* CONFIG_RT_REGMAP */

static void rt1711_chip_release(struct kref *k)
{
	struct rt1711_chip *chip = container_of(k, struct rt1711_chip, kref);

	pr_info("%s: rt1711_chip release %p, name=%s\n", __func__, chip, chip->name);
}

struct rt1711_chip *rt1711_chip_get(const char *name)
{
	struct rt1711_chip *chip;

	if (!name)
		return NULL;

	mutex_lock(&rt1711_list_lock);
	list_for_each_entry(chip, &rt1711_chip_list, node) {
		if (strncmp(chip->name, name, sizeof(chip->name)) == 0) {
			if (kref_get_unless_zero(&chip->kref)) {
				mutex_unlock(&rt1711_list_lock);
				return chip;
			}
			break;
		}
	}
	mutex_unlock(&rt1711_list_lock);
	return NULL;
}
EXPORT_SYMBOL_GPL(rt1711_chip_get);

void rt1711_chip_put(struct rt1711_chip *chip)
{
	if (!chip)
		return;
	kref_put(&chip->kref, rt1711_chip_release);
}
EXPORT_SYMBOL_GPL(rt1711_chip_put);

bool rt1711_chip_is_probed(struct rt1711_chip *chip)
{
	return chip ? chip->probed : false;
}
EXPORT_SYMBOL_GPL(rt1711_chip_is_probed);

static int rt1711_read_device(void *client, u32 reg, int len, void *dst)
{
	struct i2c_client *i2c = client;
	int ret = 0, count = 5;
#if ENABLE_RT1711_DBG
	u64 t1 = 0, t2 = 0;
#endif

	while (1) {
#if ENABLE_RT1711_DBG
		t1 = local_clock();
#endif
		ret = i2c_smbus_read_i2c_block_data(i2c, reg, len, dst);
#if ENABLE_RT1711_DBG
		t2 = local_clock();
		RT1711_INFO("%s: del = %lluus, reg = 0x%02X, len = %d\n",
			    __func__, (t2 - t1) / NSEC_PER_USEC, reg, len);
#endif
		if (ret < 0 && count > 1)
			count--;
		else
			break;
		udelay(100);
	}
	return ret;
}

static int rt1711_write_device(void *client, u32 reg, int len, const void *src)
{
	struct i2c_client *i2c = client;
	int ret = 0, count = 5;
#if ENABLE_RT1711_DBG
	u64 t1 = 0, t2 = 0;
#endif

	while (1) {
#if ENABLE_RT1711_DBG
		t1 = local_clock();
#endif
		ret = i2c_smbus_write_i2c_block_data(i2c, reg, len, src);
#if ENABLE_RT1711_DBG
		t2 = local_clock();
		RT1711_INFO("%s: del = %lluus, reg = %02X, len = %d\n",
			    __func__, (t2 - t1) / NSEC_PER_USEC, reg, len);
#endif
		if (ret < 0 && count > 1)
			count--;
		else
			break;
		udelay(100);
	}
	return ret;
}

static int rt1711_reg_read(struct i2c_client *i2c, u8 reg)
{
	struct rt1711_chip *chip = i2c_get_clientdata(i2c);
	u8 val = 0;
	int ret = 0;

#ifdef CONFIG_RT_REGMAP
	ret = rt_regmap_block_read(chip->m_dev, reg, 1, &val);
#else
	ret = rt1711_read_device(chip->client, reg, 1, &val);
#endif /* CONFIG_RT_REGMAP */
	if (ret < 0) {
		dev_err(chip->dev, "%s: reg read fail\n", __func__);
		return ret;
	}
	return val;
}

static int rt1711_reg_write(struct i2c_client *i2c, u8 reg, const u8 data)
{
	struct rt1711_chip *chip = i2c_get_clientdata(i2c);
	int ret = 0;

#ifdef CONFIG_RT_REGMAP
	ret = rt_regmap_block_write(chip->m_dev, reg, 1, &data);
#else
	ret = rt1711_write_device(chip->client, reg, 1, &data);
#endif /* CONFIG_RT_REGMAP */
	if (ret < 0)
		dev_err(chip->dev, "%s: reg write fail\n", __func__);
	return ret;
}

static int rt1711_block_read(struct i2c_client *i2c,
			     u8 reg, int len, void *dst)
{
	struct rt1711_chip *chip = i2c_get_clientdata(i2c);
	int ret = 0;
#ifdef CONFIG_RT_REGMAP
	ret = rt_regmap_block_read(chip->m_dev, reg, len, dst);
#else
	ret = rt1711_read_device(chip->client, reg, len, dst);
#endif /* CONFIG_RT_REGMAP */
	if (ret < 0)
		dev_err(chip->dev, "%s: block read fail\n", __func__);
	return ret;
}

static int rt1711_block_write(struct i2c_client *i2c,
			      u8 reg, int len, const void *src)
{
	struct rt1711_chip *chip = i2c_get_clientdata(i2c);
	int ret = 0;
#ifdef CONFIG_RT_REGMAP
	ret = rt_regmap_block_write(chip->m_dev, reg, len, src);
#else
	ret = rt1711_write_device(chip->client, reg, len, src);
#endif /* CONFIG_RT_REGMAP */
	if (ret < 0)
		dev_err(chip->dev, "%s: block write fail\n", __func__);
	return ret;
}

static int rt1711_write_word(struct i2c_client *client,
			     u8 reg_addr, u16 data)
{
	int ret;

	/* don't need swap */
	ret = rt1711_block_write(client, reg_addr, 2, (u8 *)&data);
	return ret;
}

static int rt1711_read_word(struct i2c_client *client,
			    u8 reg_addr, u16 *data)
{
	int ret;

	/* don't need swap */
	ret = rt1711_block_read(client, reg_addr, 2, (u8 *)data);
	return ret;
}

static inline int rt1711_i2c_write8(struct tcpc_device *tcpc, u8 reg, const u8 data)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	return rt1711_reg_write(chip->client, reg, data);
}

static inline int rt1711_i2c_write16(struct tcpc_device *tcpc, u8 reg, const u16 data)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	return rt1711_write_word(chip->client, reg, data);
}

static inline int rt1711_i2c_read8(struct tcpc_device *tcpc, u8 reg)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	return rt1711_reg_read(chip->client, reg);
}

static inline int rt1711_i2c_read16(struct tcpc_device *tcpc, u8 reg)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	u16 data;
	int ret;

	ret = rt1711_read_word(chip->client, reg, &data);
	if (ret < 0)
		return ret;
	return data;
}

#ifdef CONFIG_RT_REGMAP
static struct rt_regmap_fops rt1711_regmap_fops = {
	.read_device = rt1711_read_device,
	.write_device = rt1711_write_device,
};
#endif /* CONFIG_RT_REGMAP */

static int rt1711_chip_regmap_init(struct rt1711_chip *chip)
{
#ifdef CONFIG_RT_REGMAP
	struct rt_regmap_properties *props;
	const char *name;

	props = devm_kzalloc(chip->dev, sizeof(*props), GFP_KERNEL);
	if (!props)
		return -ENOMEM;

	name = devm_kasprintf(chip->dev, GFP_KERNEL, "rt1711-%02x",
			      chip->client->addr);
	if (!name)
		return -ENOMEM;

	props->register_num = RT1711_CHIP_REGMAP_SIZE;
	props->rm = rt1711_chip_regmap;
	props->rt_regmap_mode = RT_MULTI_BYTE |
				RT_IO_PASS_THROUGH | RT_DBG_SPECIAL;
	props->name = name;
	props->aliases = name;
	props->io_log_en = 0;

	chip->m_dev = rt_regmap_device_register(props,
						&rt1711_regmap_fops,
						chip->dev, chip->client, chip);
	if (!chip->m_dev) {
		dev_err(chip->dev, "%s: rt_regmap register fail\n", __func__);
		return -EINVAL;
	}
#endif /* CONFIG_RT_REGMAP */
	return 0;
}

static int rt1711_chip_regmap_deinit(struct rt1711_chip *chip)
{
#ifdef CONFIG_RT_REGMAP
	rt_regmap_device_unregister(chip->m_dev);
#endif /* CONFIG_RT_REGMAP */
	return 0;
}

static inline int rt1711_software_reset(struct tcpc_device *tcpc)
{
#ifdef CONFIG_RT_REGMAP
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
#endif /* CONFIG_RT_REGMAP */
	int ret;

	ret = rt1711_i2c_write8(tcpc, RT1711H_REG_SWRESET, 1);
	if (ret < 0)
		return ret;

#ifdef CONFIG_RT_REGMAP
	rt_regmap_cache_reload(chip->m_dev);
#endif /* CONFIG_RT_REGMAP */

	usleep_range(1000, 2000);
	return 0;
}

static inline int rt1711_command(struct tcpc_device *tcpc, u8 cmd)
{
	return rt1711_i2c_write8(tcpc, TCPC_V10_REG_COMMAND, cmd);
}

static int rt1711_init_vbus_cal(struct tcpc_device *tcpc)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	const u8 val_en_test_mode[] = {0x86, 0x62};
	const u8 val_dis_test_mode[] = {0x00, 0x00};
	int ret = 0;
	u8 data = 0;
	s8 cal = 0;

	ret = rt1711_block_write(chip->client, RT1711H_REG_UNLOCK_PW_2,
				 ARRAY_SIZE(val_en_test_mode), val_en_test_mode);
	if (ret < 0)
		dev_notice(chip->dev, "%s: en test mode fail(%d)\n",
			   __func__, ret);

	ret = rt1711_reg_read(chip->client, RT1711H_REG_EFUSE5);
	if (ret < 0)
		goto out;

	data = ret;
	data = (data & RT1711H_REG_M_VBUS_CAL) >> RT1711H_REG_S_VBUS_CAL;
	cal = (data & BIT(2)) ? (data | GENMASK(7, 3)) : data;
	cal -= 2;
	if (cal < RT1711H_REG_MIN_VBUS_CAL)
		cal = RT1711H_REG_MIN_VBUS_CAL;
	data = (cal << RT1711H_REG_S_VBUS_CAL) | (ret & GENMASK(4, 0));

	ret = rt1711_reg_write(chip->client, RT1711H_REG_EFUSE5, data);
out:
	ret = rt1711_block_write(chip->client, RT1711H_REG_UNLOCK_PW_2,
				 ARRAY_SIZE(val_dis_test_mode), val_dis_test_mode);
	if (ret < 0)
		dev_notice(chip->dev, "%s: dis test mode fail(%d)\n",
			   __func__, ret);

	return ret;
}

static int rt1711_init_alert_mask(struct tcpc_device *tcpc)
{
	u16 mask;
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	mask = TCPC_V10_REG_ALERT_CC_STATUS |
			TCPC_V10_REG_ALERT_POWER_STATUS;

#ifdef CONFIG_USB_POWER_DELIVERY
	/* Need to handle RX overflow */
	mask |= TCPC_V10_REG_ALERT_TX_SUCCESS |
			TCPC_V10_REG_ALERT_TX_DISCARDED |
			TCPC_V10_REG_ALERT_TX_FAILED |
			TCPC_V10_REG_ALERT_RX_HARD_RST |
			TCPC_V10_REG_ALERT_RX_STATUS |
			TCPC_V10_REG_RX_OVERFLOW;
#endif

	mask |= TCPC_REG_ALERT_FAULT;

	return rt1711_write_word(chip->client, TCPC_V10_REG_ALERT_MASK, mask);
}

static int rt1711_init_power_status_mask(struct tcpc_device *tcpc)
{
	const u8 mask = TCPC_V10_REG_POWER_STATUS_VBUS_PRES;

	return rt1711_i2c_write8(tcpc,
			TCPC_V10_REG_POWER_STATUS_MASK, mask);
}

static int rt1711_init_fault_mask(struct tcpc_device *tcpc)
{
	const u8 mask =
		TCPC_V10_REG_FAULT_STATUS_VCONN_OV |
		TCPC_V10_REG_FAULT_STATUS_VCONN_OC;

	return rt1711_i2c_write8(tcpc,
			TCPC_V10_REG_FAULT_STATUS_MASK, mask);
}

static int rt1711_init_rt_mask(struct tcpc_device *tcpc)
{
	u8 rt_mask = 0;
#ifdef CONFIG_TCPC_WATCHDOG_EN
	rt_mask |= RT1711H_REG_M_WATCHDOG;
#endif /* CONFIG_TCPC_WATCHDOG_EN */
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	rt_mask |= RT1711H_REG_M_VBUS_80;
#endif /* CONFIG_TCPC_VSAFE0V_DETECT_IC */

#ifdef CONFIG_TYPEC_CAP_RA_DETACH
	if (tcpc->tcpc_flags & TCPC_FLAGS_CHECK_RA_DETACH)
		rt_mask |= RT1711H_REG_M_RA_DETACH;
#endif /* CONFIG_TYPEC_CAP_RA_DETACH */

#ifdef CONFIG_TYPEC_CAP_LPM_WAKEUP_WATCHDOG
	if (tcpc->tcpc_flags & TCPC_FLAGS_LPM_WAKEUP_WATCHDOG)
		rt_mask |= RT1711H_REG_M_WAKEUP;
#endif	/* CONFIG_TYPEC_CAP_LPM_WAKEUP_WATCHDOG */

	return rt1711_i2c_write8(tcpc, RT1711H_REG_RT_MASK, rt_mask);
}

static irqreturn_t rt1711_hard_irq(int irq, void *data)
{
	struct rt1711_chip *chip = data;

	if (!chip)
		return IRQ_NONE;

	__pm_stay_awake(chip->rt1711_ws);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t rt1711_thread_irq(int irq, void *data)
{
	struct rt1711_chip *chip = data;

	mutex_lock(&chip->irq_lock);

	if (atomic_read(&chip->shutting_down)) {
		mutex_unlock(&chip->irq_lock);
		goto out_relax;
	}

	if (!atomic_read(&chip->resume_completed)) {
		if (!atomic_read(&chip->irq_disabled)) {
			disable_irq_nosync(chip->irq);
			atomic_set(&chip->irq_disabled, 1);
		}
		mutex_unlock(&chip->irq_lock);
		goto out_relax;
	}

	mutex_unlock(&chip->irq_lock);

	if (chip->tcpc) {
		tcpci_lock_typec(chip->tcpc);
		tcpci_alert(chip->tcpc);
		tcpci_unlock_typec(chip->tcpc);
	}

out_relax:
	__pm_relax(chip->rt1711_ws);
	return IRQ_HANDLED;
}

static int rt1711_init_alert(struct tcpc_device *tcpc)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	int irqn = 0, ret = 0;
	char *name = NULL;

	/* Clear Alert Mask & Status */
	rt1711_write_word(chip->client, TCPC_V10_REG_ALERT_MASK, 0);
	rt1711_write_word(chip->client, TCPC_V10_REG_ALERT, 0xffff);

	name = devm_kasprintf(chip->dev, GFP_KERNEL, "%s-IRQ", chip->tcpc_desc->name);
	if (!name)
		return -ENOMEM;

	dev_info(chip->dev, "%s: name = %s\n", __func__, chip->tcpc_desc->name);

	irqn = gpiod_to_irq(chip->irq_gpiod);
	if (irqn <= 0) {
		dev_err(chip->dev, "%s: gpiod_to_irq fail(%d)\n", __func__, irqn);
		return irqn;
	}

	dev_info(chip->dev, "%s: IRQ number = %d\n", __func__, irqn);

	ret = devm_request_threaded_irq(chip->dev, irqn,
					rt1711_hard_irq,
					rt1711_thread_irq,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					name, chip);
	if (ret < 0) {
		dev_notice(chip->dev, "%s: request irq fail(%d)\n",
			   __func__, ret);
		return ret;
	}
	chip->irq = irqn;

	return 0;
}

int rt1711_alert_status_clear(struct tcpc_device *tcpc, u32 mask)
{
	int ret;
	u16 mask_t1;
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	u8 mask_t2;
#endif

	/* Write 1 clear */
	mask_t1 = (u16)mask;
	if (mask_t1) {
		ret = rt1711_i2c_write16(tcpc, TCPC_V10_REG_ALERT, mask_t1);
		if (ret < 0)
			return ret;
	}

#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	mask_t2 = mask >> 16;
	if (mask_t2) {
		ret = rt1711_i2c_write8(tcpc, RT1711H_REG_RT_INT, mask_t2);
		if (ret < 0)
			return ret;
	}
#endif

	return 0;
}

static int rt1711h_set_clock_gating(struct tcpc_device *tcpc, bool en)
{
	int ret = 0;

#ifdef CONFIG_TCPC_CLOCK_GATING
	int i = 0;
	u8 clk2 = RT1711H_REG_CLK_DIV_600K_EN
		| RT1711H_REG_CLK_DIV_300K_EN | RT1711H_REG_CLK_CK_300K_EN;
	u8 clk3 = RT1711H_REG_CLK_DIV_2P4M_EN;

	if (!en) {
		clk2 |=
			RT1711H_REG_CLK_BCLK2_EN | RT1711H_REG_CLK_BCLK_EN;
		clk3 |=
			RT1711H_REG_CLK_CK_24M_EN | RT1711H_REG_CLK_PCLK_EN;
	}

	if (en) {
		for (i = 0; i < 2; i++)
			ret = rt1711_alert_status_clear(tcpc,
							TCPC_REG_ALERT_RX_ALL_MASK);
	}

	if (ret == 0)
		ret = rt1711_i2c_write8(tcpc, RT1711H_REG_CLK_CTRL2, clk2);
	if (ret == 0)
		ret = rt1711_i2c_write8(tcpc, RT1711H_REG_CLK_CTRL3, clk3);
#endif	/* CONFIG_TCPC_CLOCK_GATING */

	return ret;
}

static inline int rt1711h_init_cc_params(struct tcpc_device *tcpc, u8 cc_res)
{
	int rv = 0;

#ifdef CONFIG_USB_POWER_DELIVERY
#ifdef CONFIG_USB_PD_SNK_DFT_NO_GOOD_CRC
	u8 en, sel;
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	if (cc_res == TYPEC_CC_VOLT_SNK_DFT) {	/* 0.55 */
		en = 0;
		sel = 0x81;
	} else if (chip->chip_id >= RT1715_DID_D) {	/* 0.35 & 0.75 */
		en = 1;
		sel = 0x81;
	} else {	/* 0.4 & 0.7 */
		en = 1;
		sel = 0x80;
	}

	rv = rt1711_i2c_write8(tcpc, RT1711H_REG_BMCIO_RXDZEN, en);
	if (rv == 0)
		rv = rt1711_i2c_write8(tcpc, RT1711H_REG_BMCIO_RXDZSEL, sel);
#endif	/* CONFIG_USB_PD_SNK_DFT_NO_GOOD_CRC */
#endif	/* CONFIG_USB_POWER_DELIVERY */

	return rv;
}

static int rt1711_tcpc_init(struct tcpc_device *tcpc, bool sw_reset)
{
	int ret;
	bool retry_discard_old = false;
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	RT1711_INFO("entry\n");

	if (sw_reset) {
		ret = rt1711_software_reset(tcpc);
		if (ret < 0)
			return ret;
	}

#ifdef CONFIG_TCPC_I2CRST_EN
	if (chip->chip_id != HUSB311_DID) {
		rt1711_i2c_write8(tcpc,
				  RT1711H_REG_I2CRST_CTRL,
				  RT1711H_REG_I2CRST_SET(true, 0x0f));
	}
#endif	/* CONFIG_TCPC_I2CRST_EN */

	/* UFP Both RD setting */
	/* DRP = 0, RpVal = 0 (Default), Rd, Rd */
	rt1711_i2c_write8(tcpc, TCPC_V10_REG_ROLE_CTRL,
			  TCPC_V10_REG_ROLE_CTRL_RES_SET(0, 0, CC_RD, CC_RD));

	if (chip->chip_id == RT1711H_DID_A) {
		rt1711_i2c_write8(tcpc, TCPC_V10_REG_FAULT_CTRL,
				  TCPC_V10_REG_FAULT_CTRL_DIS_VCONN_OV);
	}

	/*
	 * CC Detect Debounce : 26.7*val us
	 * Transition window count : spec 12~20us, based on 2.4MHz
	 * DRP Toggle Cycle : 51.2 + 6.4*val ms
	 * DRP Duty Ctrl : dcSRC: /1024
	 */

	rt1711_i2c_write8(tcpc, RT1711H_REG_TTCPC_FILTER, 10);
	rt1711_i2c_write8(tcpc, RT1711H_REG_DRP_TOGGLE_CYCLE, 4);
	rt1711_i2c_write16(tcpc,
			   RT1711H_REG_DRP_DUTY_CTRL, TCPC_NORMAL_RP_DUTY);

	/* RX/TX Clock Gating (Auto Mode)*/
	if (!sw_reset)
		rt1711h_set_clock_gating(tcpc, true);

	if (!(tcpc->tcpc_flags & TCPC_FLAGS_RETRY_CRC_DISCARD))
		retry_discard_old = true;

	rt1711_i2c_write8(tcpc, RT1711H_REG_CONFIG_GPIO0, 0x80);

	/* For BIST, Change Transition Toggle Counter (Noise) from 3 to 7 */
	rt1711_i2c_write8(tcpc, RT1711H_REG_PHY_CTRL1,
			  RT1711H_REG_PHY_CTRL1_SET(retry_discard_old, 7, 0, 1));

	tcpci_alert_status_clear(tcpc, 0xffffffff);

	rt1711_init_vbus_cal(tcpc);

	rt1711_init_power_status_mask(tcpc);
	rt1711_init_alert_mask(tcpc);
	rt1711_init_fault_mask(tcpc);
	rt1711_init_rt_mask(tcpc);

	/* CK_300K from 320K, SHIPPING off, AUTOIDLE enable, TIMEOUT = 6.4ms */
	rt1711_i2c_write8(tcpc, RT1711H_REG_IDLE_CTRL,
			  RT1711H_REG_IDLE_SET(0, 1, 1, 0));
	mdelay(1);

	return 0;
}

static inline int rt1711_fault_status_vconn_ov(struct tcpc_device *tcpc)
{
	int ret;

	ret = rt1711_i2c_read8(tcpc, RT1711H_REG_BMC_CTRL);
	if (ret < 0)
		return ret;

	ret &= ~RT1711H_REG_DISCHARGE_EN;
	return rt1711_i2c_write8(tcpc, RT1711H_REG_BMC_CTRL, ret);
}

int rt1711_fault_status_clear(struct tcpc_device *tcpc, u8 status)
{
	int ret = 0;

	if (status & TCPC_V10_REG_FAULT_STATUS_VCONN_OV) {
		ret = rt1711_fault_status_vconn_ov(tcpc);
		if (ret < 0)
			return ret;
	}

	ret = rt1711_i2c_write8(tcpc, TCPC_V10_REG_FAULT_STATUS, status);
	return ret;
}

int rt1711_get_alert_mask(struct tcpc_device *tcpc, u32 *mask)
{
	int ret;
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	u8 v2;
#endif

	ret = rt1711_i2c_read16(tcpc, TCPC_V10_REG_ALERT_MASK);
	if (ret < 0)
		return ret;

	*mask = (u16)ret;

#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	ret = rt1711_i2c_read8(tcpc, RT1711H_REG_RT_MASK);
	if (ret < 0)
		return ret;

	v2 = (u8)ret;
	*mask |= v2 << 16;
#endif

	return 0;
}

int rt1711_get_alert_status(struct tcpc_device *tcpc, u32 *alert)
{
	int ret;
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	u8 v2;
#endif

	ret = rt1711_i2c_read16(tcpc, TCPC_V10_REG_ALERT);
	if (ret < 0)
		return ret;

	*alert = (u16)ret;

#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	ret = rt1711_i2c_read8(tcpc, RT1711H_REG_RT_INT);
	if (ret < 0)
		return ret;

	v2 = (u8)ret;
	*alert |= v2 << 16;
#endif

	return 0;
}

static int rt1711_get_power_status(struct tcpc_device *tcpc, u16 *pwr_status)
{
	int ret;

	ret = rt1711_i2c_read8(tcpc, TCPC_V10_REG_POWER_STATUS);
	if (ret < 0)
		return ret;

	*pwr_status = 0;

	if (ret & TCPC_V10_REG_POWER_STATUS_VBUS_PRES)
		*pwr_status |= TCPC_REG_POWER_STATUS_VBUS_PRES;

#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	ret = rt1711_i2c_read8(tcpc, RT1711H_REG_RT_STATUS);
	if (ret < 0)
		return ret;

	if (ret & RT1711H_REG_VBUS_80)
		*pwr_status |= TCPC_REG_POWER_STATUS_EXT_VSAFE0V;
#endif
	return 0;
}

int rt1711_get_fault_status(struct tcpc_device *tcpc, u8 *status)
{
	int ret;

	ret = rt1711_i2c_read8(tcpc, TCPC_V10_REG_FAULT_STATUS);
	if (ret < 0)
		return ret;
	*status = (u8)ret;
	return 0;
}

static int rt1711_get_cc(struct tcpc_device *tcpc, int *cc1, int *cc2)
{
	int status, role_ctrl, cc_role;
	bool act_as_sink, act_as_drp;

	status = rt1711_i2c_read8(tcpc, TCPC_V10_REG_CC_STATUS);
	if (status < 0)
		return status;

	role_ctrl = rt1711_i2c_read8(tcpc, TCPC_V10_REG_ROLE_CTRL);
	if (role_ctrl < 0)
		return role_ctrl;

	if (status & TCPC_V10_REG_CC_STATUS_DRP_TOGGLING) {
		*cc1 = TYPEC_CC_DRP_TOGGLING;
		*cc2 = TYPEC_CC_DRP_TOGGLING;
		return 0;
	}

	*cc1 = TCPC_V10_REG_CC_STATUS_CC1(status);
	*cc2 = TCPC_V10_REG_CC_STATUS_CC2(status);

	act_as_drp = TCPC_V10_REG_ROLE_CTRL_DRP & role_ctrl;

	if (act_as_drp) {
		act_as_sink = TCPC_V10_REG_CC_STATUS_DRP_RESULT(status);
	} else {
		cc_role = TCPC_V10_REG_CC_STATUS_CC1(role_ctrl);
		if (cc_role == TYPEC_CC_RP)
			act_as_sink = false;
		else
			act_as_sink = true;
	}

	/*
	 * If status is not open, then OR in termination to convert to
	 * enum tcpc_cc_voltage_status.
	 */

	if (*cc1 != TYPEC_CC_VOLT_OPEN)
		*cc1 |= (act_as_sink << 2);

	if (*cc2 != TYPEC_CC_VOLT_OPEN)
		*cc2 |= (act_as_sink << 2);

	rt1711h_init_cc_params(tcpc,
			       (u8)tcpc->typec_polarity ? *cc2 : *cc1);

	return 0;
}

#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
static int rt1711_enable_vsafe0v_detect(struct tcpc_device *tcpc, bool enable)
{
	int ret = rt1711_i2c_read8(tcpc, RT1711H_REG_RT_MASK);

	if (ret < 0)
		return ret;

	if (enable)
		ret |= RT1711H_REG_M_VBUS_80;
	else
		ret &= ~RT1711H_REG_M_VBUS_80;

	return rt1711_i2c_write8(tcpc, RT1711H_REG_RT_MASK, (u8)ret);
}
#endif /* CONFIG_TCPC_VSAFE0V_DETECT_IC */

static int rt1711_set_cc(struct tcpc_device *tcpc, int pull)
{
	int ret;
	u8 data;
	int rp_lvl = TYPEC_CC_PULL_GET_RP_LVL(pull), pull1, pull2;

	RT1711_INFO("entry\n");
	pull = TYPEC_CC_PULL_GET_RES(pull);
	if (pull == TYPEC_CC_DRP) {
		data = TCPC_V10_REG_ROLE_CTRL_RES_SET(1, rp_lvl, TYPEC_CC_RD, TYPEC_CC_RD);

		ret = rt1711_i2c_write8(tcpc, TCPC_V10_REG_ROLE_CTRL, data);
		if (ret == 0) {
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
			rt1711_enable_vsafe0v_detect(tcpc, false);
#endif /* CONFIG_TCPC_VSAFE0V_DETECT_IC */
			ret = rt1711_command(tcpc, TCPM_CMD_LOOK_CONNECTION);
		}
	} else {
#ifdef CONFIG_USB_POWER_DELIVERY
		if (pull == TYPEC_CC_RD && tcpc->pd_wait_pr_swap_complete)
			rt1711h_init_cc_params(tcpc, TYPEC_CC_VOLT_SNK_DFT);
#endif	/* CONFIG_USB_POWER_DELIVERY */

		pull1 = pull2 = pull;

		if ((pull == TYPEC_CC_RP_DFT ||
		     pull == TYPEC_CC_RP_1_5 ||
		     pull == TYPEC_CC_RP_3_0) &&
		     tcpc->typec_is_attached_src) {
			if (tcpc->typec_polarity)
				pull1 = TYPEC_CC_OPEN;
			else
				pull2 = TYPEC_CC_OPEN;
		}
		data = TCPC_V10_REG_ROLE_CTRL_RES_SET(0, rp_lvl, pull1, pull2);
		ret = rt1711_i2c_write8(tcpc, TCPC_V10_REG_ROLE_CTRL, data);
	}

	return 0;
}

static int rt1711_set_polarity(struct tcpc_device *tcpc, int polarity)
{
	int data;

	data = rt1711h_init_cc_params(tcpc,
				      tcpc->typec_remote_cc[polarity]);
	if (data)
		return data;

	data = rt1711_i2c_read8(tcpc, TCPC_V10_REG_TCPC_CTRL);
	if (data < 0)
		return data;

	data &= ~TCPC_V10_REG_TCPC_CTRL_PLUG_ORIENT;
	data |= polarity ? TCPC_V10_REG_TCPC_CTRL_PLUG_ORIENT : 0;

	return rt1711_i2c_write8(tcpc, TCPC_V10_REG_TCPC_CTRL, data);
}

static int rt1711_set_low_rp_duty(struct tcpc_device *tcpc, bool low_rp)
{
	u16 duty = low_rp ? TCPC_LOW_RP_DUTY : TCPC_NORMAL_RP_DUTY;

	return rt1711_i2c_write16(tcpc, RT1711H_REG_DRP_DUTY_CTRL, duty);
}

static int rt1711_set_vconn(struct tcpc_device *tcpc, int enable)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);

	int rv;
	int data;

	data = rt1711_i2c_read8(tcpc, TCPC_V10_REG_POWER_CTRL);
	if (data < 0)
		return data;

	data &= ~TCPC_V10_REG_POWER_CTRL_VCONN;
	data |= enable ? TCPC_V10_REG_POWER_CTRL_VCONN : 0;

	if (chip->chip_id == HUSB311_DID) {
		data &= ~TCPC_V10_REG_POWER_CTRL_VCONN;
		pr_info("%s: write 0x%x to HUSB311_POWER_CTRL_VCONNL, off vconn\n",
			__func__, data);
	}

	rv = rt1711_i2c_write8(tcpc, TCPC_V10_REG_POWER_CTRL, data);
	if (rv < 0)
		return rv;

	return rt1711_i2c_write8(tcpc, RT1711H_REG_IDLE_CTRL,
		RT1711H_REG_IDLE_SET(0, 1, enable ? 0 : 1, 0));
}

#ifdef CONFIG_TCPC_LOW_POWER_MODE
static int rt1711_is_low_power_mode(struct tcpc_device *tcpc)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	int rv;

	rv = rt1711_i2c_read8(tcpc, RT1711H_REG_BMC_CTRL);

	if (rv < 0)
		return rv;

	if (chip->chip_id == HUSB311_DID) {
		pr_info("%s: read HUSB311_REG_BMC_CTRL = 0x%x\n", __func__, rv);
		return ((rv & RT1711H_REG_BMCIO_OSC_EN) == 0);
	}

	return (rv & RT1711H_REG_BMCIO_LPEN) != 0;
}

static int rt1711_set_low_power_mode(struct tcpc_device *tcpc, bool en, int pull)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	int ret = 0;
	u8 data;

	ret = rt1711_i2c_write8(tcpc, RT1711H_REG_IDLE_CTRL,
				RT1711H_REG_IDLE_SET(0, 1, en ? 0 : 1, 0));
	if (ret < 0)
		return ret;
#ifdef CONFIG_TCPC_VSAFE0V_DETECT_IC
	rt1711_enable_vsafe0v_detect(tcpc, !en);
#endif /* CONFIG_TCPC_VSAFE0V_DETECT_IC */
	if (en) {
		data = RT1711H_REG_BMCIO_LPEN;

		if (pull & TYPEC_CC_RP)
			data |= RT1711H_REG_BMCIO_LPRPRD;

#ifdef CONFIG_TYPEC_CAP_NORP_SRC
		data |= RT1711H_REG_BMCIO_BG_EN | RT1711H_REG_VBUS_DET_EN;
#endif
		if (chip->chip_id == HUSB311_DID) {
			data &= ~RT1711H_REG_BMCIO_OSC_EN;
			pr_info("%s: write HUSB311_REG_BMC_CTRL = 0x%x\n",
				__func__, data);
		}

	} else {
		data = RT1711H_REG_BMCIO_BG_EN |
			RT1711H_REG_VBUS_DET_EN | RT1711H_REG_BMCIO_OSC_EN;

		if (chip->chip_id == HUSB311_DID) {
			data |= RT1711H_REG_BMCIO_OSC_EN;
			pr_info("%s: write HUSB311_REG_BMC_CTRL = 0x%x\n",
				__func__, data);
		}
	}

	return rt1711_i2c_write8(tcpc, RT1711H_REG_BMC_CTRL, data);
}
#endif	/* CONFIG_TCPC_LOW_POWER_MODE */

#ifdef CONFIG_TCPC_WATCHDOG_EN
int rt1711h_set_watchdog(struct tcpc_device *tcpc, bool en)
{
	u8 data = RT1711H_REG_WATCHDOG_CTRL_SET(en, 7);

	return rt1711_i2c_write8(tcpc,
		RT1711H_REG_WATCHDOG_CTRL, data);
}
#endif	/* CONFIG_TCPC_WATCHDOG_EN */

#ifdef CONFIG_TCPC_INTRST_EN
static int rt1711h_set_intrst(struct tcpc_device *tcpc, bool en)
{
	return rt1711_i2c_write8(tcpc,
		RT1711H_REG_INTRST_CTRL, RT1711H_REG_INTRST_SET(en, 3));
}
#endif	/* CONFIG_TCPC_INTRST_EN */

static int rt1711_tcpc_deinit(struct tcpc_device *tcpc)
{
#ifdef CONFIG_RT_REGMAP
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
#endif /* CONFIG_RT_REGMAP */

#ifdef CONFIG_TCPC_SHUTDOWN_CC_DETACH
	rt1711_set_cc(tcpc, TYPEC_CC_DRP);
	rt1711_set_cc(tcpc, TYPEC_CC_OPEN);

	if (chip->chip_id == HUSB311_DID) {
		tcpci_alert_status_clear(tcpc, 0xffffff);//0x10 0x11 0x98

		rt1711_write_word(chip->client, TCPC_V10_REG_ALERT_MASK, 0x0); //0x12 0x13
		rt1711_i2c_write8(tcpc, TCPC_V10_REG_POWER_STATUS_MASK, 0x0); //0x14
		rt1711_i2c_write8(tcpc, RT1711H_REG_RT_MASK, 0x0); //0x99
		rt1711_i2c_write8(tcpc, RT1711H_REG_BMC_CTRL, 0x0); //0x90
	} else {
		rt1711_i2c_write8(tcpc,
				  RT1711H_REG_I2CRST_CTRL,
				  RT1711H_REG_I2CRST_SET(true, 4));
	}

	rt1711_i2c_write8(tcpc,
			  RT1711H_REG_INTRST_CTRL,
			  RT1711H_REG_INTRST_SET(true, 0));
#else
	rt1711_i2c_write8(tcpc, RT1711H_REG_SWRESET, 1);
#endif /* CONFIG_TCPC_SHUTDOWN_CC_DETACH */

#ifdef CONFIG_RT_REGMAP
	rt_regmap_cache_reload(chip->m_dev);
#endif /* CONFIG_RT_REGMAP */

	return 0;
}

#ifdef CONFIG_USB_POWER_DELIVERY
static int rt1711_set_msg_header(struct tcpc_device *tcpc,
				 u8 power_role, u8 data_role)
{
	u8 msg_hdr = TCPC_V10_REG_MSG_HDR_INFO_SET(data_role, power_role);

	return rt1711_i2c_write8(tcpc, TCPC_V10_REG_MSG_HDR_INFO, msg_hdr);
}

static int rt1711_protocol_reset(struct tcpc_device *tcpc)
{
	rt1711_i2c_write8(tcpc, RT1711H_REG_PRL_FSM_RESET, 0);
	mdelay(1);
	rt1711_i2c_write8(tcpc, RT1711H_REG_PRL_FSM_RESET, 1);

	return 0;
}

static int rt1711_set_rx_enable(struct tcpc_device *tcpc, u8 enable)
{
	int ret = 0;

	if (enable)
		ret = rt1711h_set_clock_gating(tcpc, false);

	if (ret == 0)
		ret = rt1711_i2c_write8(tcpc, TCPC_V10_REG_RX_DETECT, enable);

	if (ret == 0 && !enable) {
		rt1711_protocol_reset(tcpc);
		ret = rt1711h_set_clock_gating(tcpc, true);
	}

	return ret;
}

static int rt1711_get_message(struct tcpc_device *tcpc, u32 *payload,
			      u16 *msg_head, enum tcpm_transmit_type *frame_type)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	int rv = 0;
	u8 cnt = 0, buf[4];

	rv = rt1711_block_read(chip->client, TCPC_V10_REG_RX_BYTE_CNT, 4, buf);
	if (rv < 0)
		return rv;
	cnt = buf[0];
	*frame_type = buf[1];
	*msg_head = le16_to_cpu(*(u16 *)&buf[2]);

	/* TCPC 1.0 ==> no need to subtract the size of msg_head */
	if (cnt > 3) {
		cnt -= 3; /* MSG_HDR */
		rv = rt1711_block_read(chip->client, TCPC_V10_REG_RX_DATA, cnt, payload);
	}

	return rv;
}

static int rt1711_set_bist_carrier_mode(struct tcpc_device *tcpc, u8 pattern)
{
	/* Don't support this function */
	return 0;
}

#ifdef CONFIG_USB_PD_RETRY_CRC_DISCARD
static int rt1711_retransmit(struct tcpc_device *tcpc)
{
	return rt1711_i2c_write8(tcpc, TCPC_V10_REG_TRANSMIT,
			TCPC_V10_REG_TRANSMIT_SET(tcpc->pd_retry_count, TCPC_TX_SOP));
}
#endif

#pragma pack(push, 1)
struct tcpc_transmit_packet {
	u8 cnt;
	u16 msg_header;
	u8 data[sizeof(u32) * 7];
};

#pragma pack(pop)

static int rt1711_transmit(struct tcpc_device *tcpc,
			   enum tcpm_transmit_type type, u16 header, const u32 *data)
{
	struct rt1711_chip *chip = tcpc_get_dev_data(tcpc);
	int rv;
	int data_cnt;
	struct tcpc_transmit_packet packet;

	if (type < TCPC_TX_HARD_RESET) {
		data_cnt = sizeof(u32) * PD_HEADER_CNT(header);

		packet.cnt = data_cnt + sizeof(u16);
		packet.msg_header = header;

		if (data_cnt > 0)
			memcpy(packet.data, (u8 *)data, data_cnt);

		rv = rt1711_block_write(chip->client,
					TCPC_V10_REG_TX_BYTE_CNT,
					packet.cnt + 1, (u8 *)&packet);
		if (rv < 0)
			return rv;
	}

	rv = rt1711_i2c_write8(tcpc, TCPC_V10_REG_TRANSMIT,
			       TCPC_V10_REG_TRANSMIT_SET(tcpc->pd_retry_count, type));
	return rv;
}

static int rt1711_set_bist_test_mode(struct tcpc_device *tcpc, bool en)
{
	int data;

	data = rt1711_i2c_read8(tcpc, TCPC_V10_REG_TCPC_CTRL);
	if (data < 0)
		return data;

	data &= ~TCPC_V10_REG_TCPC_CTRL_BIST_TEST_MODE;
	data |= en ? TCPC_V10_REG_TCPC_CTRL_BIST_TEST_MODE : 0;

	return rt1711_i2c_write8(tcpc, TCPC_V10_REG_TCPC_CTRL, data);
}
#endif /* CONFIG_USB_POWER_DELIVERY */

static struct tcpc_ops rt1711_tcpc_ops = {
	.init = rt1711_tcpc_init,
	.alert_status_clear = rt1711_alert_status_clear,
	.fault_status_clear = rt1711_fault_status_clear,
	.get_alert_mask = rt1711_get_alert_mask,
	.get_alert_status = rt1711_get_alert_status,
	.get_power_status = rt1711_get_power_status,
	.get_fault_status = rt1711_get_fault_status,
	.get_cc = rt1711_get_cc,
	.set_cc = rt1711_set_cc,
	.set_polarity = rt1711_set_polarity,
	.set_low_rp_duty = rt1711_set_low_rp_duty,
	.set_vconn = rt1711_set_vconn,
	.deinit = rt1711_tcpc_deinit,

#ifdef CONFIG_TCPC_LOW_POWER_MODE
	.is_low_power_mode = rt1711_is_low_power_mode,
	.set_low_power_mode = rt1711_set_low_power_mode,
#endif	/* CONFIG_TCPC_LOW_POWER_MODE */

#ifdef CONFIG_TCPC_WATCHDOG_EN
	.set_watchdog = rt1711h_set_watchdog,
#endif	/* CONFIG_TCPC_WATCHDOG_EN */

#ifdef CONFIG_TCPC_INTRST_EN
	.set_intrst = rt1711h_set_intrst,
#endif	/* CONFIG_TCPC_INTRST_EN */

#ifdef CONFIG_USB_POWER_DELIVERY
	.set_msg_header = rt1711_set_msg_header,
	.set_rx_enable = rt1711_set_rx_enable,
	.protocol_reset = rt1711_protocol_reset,
	.get_message = rt1711_get_message,
	.transmit = rt1711_transmit,
	.set_bist_test_mode = rt1711_set_bist_test_mode,
	.set_bist_carrier_mode = rt1711_set_bist_carrier_mode,
#endif	/* CONFIG_USB_POWER_DELIVERY */

#ifdef CONFIG_USB_PD_RETRY_CRC_DISCARD
	.retransmit = rt1711_retransmit,
#endif	/* CONFIG_USB_PD_RETRY_CRC_DISCARD */
};

static int rt_parse_dt(struct rt1711_chip *chip, struct device *dev)
{
	chip->irq_gpiod = devm_gpiod_get(dev, "rt1711pd,intr", GPIOD_IN);
	if (IS_ERR(chip->irq_gpiod)) {
		dev_err(dev, "%s: failed to get IRQ GPIO: %ld\n",
			__func__, PTR_ERR(chip->irq_gpiod));
		return PTR_ERR(chip->irq_gpiod);
	}

	return 0;
}

/*
 * In some platform pr_info may spend too much time on printing debug message.
 * So we use this function to test the printk performance.
 * If your platform cannot not pass this check function, please config
 * PD_DBG_INFO, this will provide the threaded debug message for you.
 */
#if TCPC_ENABLE_ANYMSG
static void check_printk_performance(void)
{
	int i;
	u64 t1, t2;
	u32 nsrem;

#ifdef CONFIG_PD_DBG_INFO
	for (i = 0; i < 10; i++) {
		t1 = local_clock();
		pd_dbg_info("%d\n", i);
		t2 = local_clock();
		t2 -= t1;
		nsrem = do_div(t2, 1000000000);
		pd_dbg_info("pd_dbg_info : t2-t1 = %lu\n",
			    (unsigned long)nsrem / 1000);
	}
	for (i = 0; i < 10; i++) {
		t1 = local_clock();
		pr_info("%d\n", i);
		t2 = local_clock();
		t2 -= t1;
		nsrem = do_div(t2, 1000000000);
		pr_info("pr_info : t2-t1 = %lu\n",
			(unsigned long)nsrem / 1000);
	}
#else
	for (i = 0; i < 10; i++) {
		t1 = local_clock();
		pr_info("%d\n", i);
		t2 = local_clock();
		t2 -= t1;
		nsrem = do_div(t2, 1000000000);
		pr_info("t2-t1 = %lu\n",
			(unsigned long)nsrem / 1000);
		PD_BUG_ON(nsrem > 100 * 1000);
	}
#endif /* CONFIG_PD_DBG_INFO */
}
#endif /* TCPC_ENABLE_ANYMSG */

static int rt1711_tcpcdev_init(struct rt1711_chip *chip, struct device *dev)
{
	struct tcpc_desc *desc;
	struct device_node *np = dev->of_node;
	u32 val;
	char *name = "default";

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	if (of_property_read_u32(np, "rt-tcpc,role_def", &val) >= 0) {
		if (val >= TYPEC_ROLE_NR)
			desc->role_def = TYPEC_ROLE_DRP;
		else
			desc->role_def = val;
	} else {
		dev_info(dev, "%s: use default Role DRP\n", __func__);
		desc->role_def = TYPEC_ROLE_DRP;
	}

	if (of_property_read_u32(np, "rt-tcpc,notifier_supply_num", &val) >= 0) {
		if (val < 0)
			desc->notifier_supply_num = 0;
		else
			desc->notifier_supply_num = val;
	} else {
		desc->notifier_supply_num = 0;
	}

	if (of_property_read_u32(np, "rt-tcpc,rp_level", &val) >= 0) {
		switch (val) {
		case 0: /* RP Default */
			desc->rp_lvl = TYPEC_CC_RP_DFT;
			break;
		case 1: /* RP 1.5V */
			desc->rp_lvl = TYPEC_CC_RP_1_5;
			break;
		case 2: /* RP 3.0V */
			desc->rp_lvl = TYPEC_CC_RP_3_0;
			break;
		default:
			break;
		}
	}

#ifdef CONFIG_TCPC_VCONN_SUPPLY_MODE
	if (of_property_read_u32(np, "rt-tcpc,vconn_supply", &val) >= 0) {
		if (val >= TCPC_VCONN_SUPPLY_NR)
			desc->vconn_supply = TCPC_VCONN_SUPPLY_ALWAYS;
		else
			desc->vconn_supply = val;
	} else {
		dev_info(dev, "%s: use default VconnSupply\n", __func__);
		desc->vconn_supply = TCPC_VCONN_SUPPLY_ALWAYS;
	}
#endif	/* CONFIG_TCPC_VCONN_SUPPLY_MODE */

	if (of_property_read_string(np, "rt-tcpc,name",
				    (char const **)&name) < 0) {
		dev_info(dev, "%s: use default name\n", __func__);
	}

	name = devm_kasprintf(dev, GFP_KERNEL, "%s", name);
	if (!name)
		return -ENOMEM;

	desc->name = name;
	chip->tcpc_desc = desc;

	chip->tcpc = tcpc_device_register(dev,
					  desc, &rt1711_tcpc_ops, chip);
	if (IS_ERR(chip->tcpc))
		return -EINVAL;

	chip->tcpc->tcpc_flags = TCPC_FLAGS_LPM_WAKEUP_WATCHDOG |
			TCPC_FLAGS_VCONN_SAFE5V_ONLY;

	if (chip->chip_id > RT1711H_DID_B || chip->chip_id == HUSB311_DID)
		chip->tcpc->tcpc_flags |= TCPC_FLAGS_CHECK_RA_DETACH;

#ifdef CONFIG_USB_PD_RETRY_CRC_DISCARD
	if (chip->chip_id > RT1715_DID_D)
		chip->tcpc->tcpc_flags |= TCPC_FLAGS_RETRY_CRC_DISCARD;
#endif /* CONFIG_USB_PD_RETRY_CRC_DISCARD */

#ifdef CONFIG_USB_PD_REV30
	if (chip->chip_id >= RT1715_DID_D || chip->chip_id == HUSB311_DID)
		chip->tcpc->tcpc_flags |= TCPC_FLAGS_PD_REV30;

	if (chip->tcpc->tcpc_flags & TCPC_FLAGS_PD_REV30)
		dev_info(dev, "%s: PD_REV30\n", __func__);
	else
		dev_info(dev, "%s: PD_REV20\n", __func__);
#endif	/* CONFIG_USB_PD_REV30 */
	chip->tcpc->tcpc_flags |= TCPC_FLAGS_ALERT_V10;

	return 0;
}

#define RICHTEK_1711_VID	0x29cf
#define RICHTEK_1711_PID	0x1711

#define HUSB311_VID	0x2e99
#define HUSB311_PID	0x0311
u16 cc_vendor_pid;

static inline int rt1711h_check_revision(struct i2c_client *client)
{
	u16 vid, pid, did;
	int ret;
	u8 data = 1;

	ret = rt1711_read_device(client, TCPC_V10_REG_VID, 2, &vid);
	if (ret < 0) {
		dev_err(&client->dev, "%s: read chip ID fail(%d)\n", __func__, ret);
		return -EIO;
	}

	if (vid != RICHTEK_1711_VID && vid != HUSB311_VID) {
		dev_err(&client->dev, "%s: failed, VID=0x%04x\n", __func__, vid);
		return -ENODEV;
	}

	ret = rt1711_read_device(client, TCPC_V10_REG_PID, 2, &pid);
	if (ret < 0) {
		dev_err(&client->dev, "%s: read product ID fail(%d)\n", __func__, ret);
		return -EIO;
	}

	if (pid != RICHTEK_1711_PID && pid != HUSB311_PID) {
		dev_err(&client->dev, "%s: failed, PID=0x%04x\n", __func__, pid);
		return -ENODEV;
	}

	cc_vendor_pid = pid;
	dev_info(&client->dev, "%s: vid=0x%04x, pid=0x%04x\n", __func__, vid, pid);

	ret = rt1711_write_device(client, RT1711H_REG_SWRESET, 1, &data);
	if (ret < 0)
		return ret;

	usleep_range(1000, 2000);

	ret = rt1711_read_device(client, TCPC_V10_REG_DID, 2, &did);
	if (ret < 0) {
		dev_err(&client->dev, "%s: read device ID fail(%d)\n", __func__, ret);
		return -EIO;
	}

	return did;
}

static int rt1711_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct rt1711_chip *chip;
	int ret = 0, chip_id;
	bool use_dt = client->dev.of_node;

	dev_info(&client->dev, "%s: entry, (%s)\n", __func__, RT1711H_DRV_VERSION);
	if (i2c_check_functionality(client->adapter,
				    I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_BYTE_DATA))
		dev_info(&client->dev, "%s: I2C functionality: OK...\n", __func__);
	else
		dev_info(&client->dev, "%s: I2C functionality: FAILURE...\n", __func__);

	chip_id = rt1711h_check_revision(client);
	if (chip_id < 0)
		return chip_id;

#if TCPC_ENABLE_ANYMSG
	check_printk_performance();
#endif /* TCPC_ENABLE_ANYMSG */

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (use_dt) {
		ret = rt_parse_dt(chip, &client->dev);
		if (ret < 0)
			goto err_free;
	} else {
		dev_err(&client->dev, "%s: no dts node\n", __func__);
		ret = -ENODEV;
		goto err_free;
	}
	chip->dev = &client->dev;
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->chip_id = chip_id;
	dev_info(&client->dev, "%s: chipID = 0x%0x\n", __func__, chip_id);

	mutex_init(&chip->irq_lock);
	atomic_set(&chip->resume_completed, 1);
	atomic_set(&chip->irq_disabled, 0);
	atomic_set(&chip->shutting_down, 0);

	kref_init(&chip->kref);
	INIT_LIST_HEAD(&chip->node);
	strscpy(chip->name, RT1711H_NAME, sizeof(chip->name));

	chip->rt1711_ws = wakeup_source_register(NULL, "rt1711_wake");
	if (IS_ERR_OR_NULL(chip->rt1711_ws)) {
		ret = IS_ERR(chip->rt1711_ws) ? PTR_ERR(chip->rt1711_ws) : -ENOMEM;
		dev_err(chip->dev, "%s: wakeup_source_register failed: %d\n", __func__, ret);
		goto err_ws_reg;
	}

	ret = rt1711_chip_regmap_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "%s: regmap init fail\n", __func__);
		goto err_regmap_init;
	}

	ret = rt1711_tcpcdev_init(chip, &client->dev);
	if (ret < 0) {
		dev_err(&client->dev, "%s: tcpc dev init fail\n", __func__);
		goto err_tcpc_reg;
	}

	ret = rt1711_init_alert(chip->tcpc);
	if (ret < 0) {
		dev_err(&client->dev, "%s: init alert fail\n", __func__);
		goto err_irq_init;
	} else {
		enable_irq_wake(chip->irq);
	}

	mutex_lock(&rt1711_list_lock);
	list_add_tail(&chip->node, &rt1711_chip_list);
	chip->probed = true;
	mutex_unlock(&rt1711_list_lock);

	dev_info(&client->dev, "%s: success\n", __func__);
	return 0;

err_irq_init:
	tcpc_device_unregister(chip->dev, chip->tcpc);
err_tcpc_reg:
	rt1711_chip_regmap_deinit(chip);
err_regmap_init:
	if (chip->rt1711_ws) {
		wakeup_source_unregister(chip->rt1711_ws);
		chip->rt1711_ws = NULL;
	}
err_ws_reg:
	mutex_destroy(&chip->irq_lock);
	i2c_set_clientdata(client, NULL);
err_free:
	dev_err(&client->dev, "%s: failed\n", __func__);
	return ret;
}

static int rt1711_i2c_remove(struct i2c_client *client)
{
	struct rt1711_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return 0;

	if (chip->irq) {
		disable_irq_wake(chip->irq);
		disable_irq(chip->irq);
		synchronize_irq(chip->irq);
	}

	tcpc_device_unregister(chip->dev, chip->tcpc);
	rt1711_chip_regmap_deinit(chip);

	if (chip->rt1711_ws) {
		wakeup_source_unregister(chip->rt1711_ws);
		chip->rt1711_ws = NULL;
	}

	mutex_lock(&rt1711_list_lock);
	chip->probed = false;
	list_del_init(&chip->node);
	mutex_unlock(&rt1711_list_lock);
	rt1711_chip_put(chip);

	mutex_destroy(&chip->irq_lock);
	i2c_set_clientdata(client, NULL);

	return 0;
}

static void rt1711_shutdown(struct i2c_client *client)
{
	struct rt1711_chip *chip = i2c_get_clientdata(client);

	/* Please reset IC here */
	if (chip) {
		mutex_lock(&chip->irq_lock);
		atomic_set(&chip->shutting_down, 1);
		mutex_unlock(&chip->irq_lock);
		if (chip->irq) {
			disable_irq_wake(chip->irq);
			disable_irq(chip->irq);
			synchronize_irq(chip->irq);
		}
		tcpm_shutdown(chip->tcpc);
	} else {
		i2c_smbus_write_byte_data(client, RT1711H_REG_SWRESET, 0x01);
	}
}

#ifdef CONFIG_PM
static int rt1711_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt1711_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return 0;

	atomic_set(&chip->resume_completed, 0);
	synchronize_irq(chip->irq);

	dev_info(dev, "%s: Suspend successfully!\n", __func__);

	return 0;
}

static int rt1711_i2c_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt1711_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return 0;

	if (atomic_read(&chip->irq_disabled)) {
		dev_err(dev, "%s: Aborting suspend_noirq: IRQ disabled\n", __func__);
		return -EBUSY;
	}

	return 0;
}

static int rt1711_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt1711_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return 0;

	/* Makesure mem operand is done before storing resume_completed */
	smp_wmb();
	atomic_set(&chip->resume_completed, 1);

	if (atomic_read(&chip->irq_disabled)) {
		enable_irq(chip->irq);
		atomic_set(&chip->irq_disabled, 0);
	}

	dev_info(dev, "%s: Resume successfully!\n", __func__);

	return 0;
}

static const struct dev_pm_ops rt1711_pm_ops = {
	.suspend	= rt1711_i2c_suspend,
	.resume	= rt1711_i2c_resume,
	.suspend_noirq	= rt1711_i2c_suspend_noirq,
};

#define RT1711_PM_OPS (&rt1711_pm_ops)
#else
#define RT1711_PM_OPS (NULL)
#endif /* CONFIG_PM */

static const struct of_device_id rt_match_table[] = {
	{ .compatible = "richtek,rt1711h", },
	{ .compatible = "richtek,rt1715", },
	{ .compatible = "richtek,rt1716", },
	{ },
};
MODULE_DEVICE_TABLE(of, rt_match_table);

static const struct i2c_device_id rt1711_id_table[] = {
	{ "rt1711h", 0 },
	{ "rt1715", 0 },
	{ "rt1716", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt1711_id_table);

static struct i2c_driver rt1711_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "usb_type_c",
		.of_match_table = of_match_ptr(rt_match_table),
		.pm		= RT1711_PM_OPS,
	},
	.id_table	= rt1711_id_table,
	.probe		= rt1711_i2c_probe,
	.remove	= rt1711_i2c_remove,
	.shutdown	= rt1711_shutdown,
};

module_i2c_driver(rt1711_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jeff Chang <jeff_chang@richtek.com>");
MODULE_DESCRIPTION("RT1711 TCPC Driver");
MODULE_VERSION(RT1711H_DRV_VERSION);

/**** Release Note ****
 * 2.0.7_G
 * (1) Revise suspend/resume flow for IRQ
 *
 * 2.0.6_G
 * (1) Revert Vconn OC to shutdown mode
 * (2) Revise IRQ handling
 *
 * 2.0.5_G
 * (1) Utilize rt-regmap to reduce I2C accesses
 * (2) Decrease VBUS present threshold (VBUS_CAL) by 60mV (2LSBs)
 *
 * 2.0.4_G
 * (1) Mask vSafe0V IRQ before entering low power mode
 * (2) Disable auto idle mode before entering low power mode
 * (3) Reset Protocol FSM and clear RX alerts twice before clock gating
 *
 * 2.0.3_G
 * (1) Single Rp as Attatched.SRC for Ellisys TD.4.9.4
 *
 * 2.0.2_G
 * (1) Replace wake_lock with wakeup_source
 * (2) Move down the shipping off
 * (3) Add support for NoRp.SRC
 * (4) Reg0x71[7] = 1'b1 to workaround unstable VDD Iq in low power mode
 * (5) Add get_alert_mask of tcpc_ops
 *
 * 2.0.1_G
 * First released PD3.0 Driver
 */
