// SPDX-License-Identifier: GPL-2.0-only
/*
 * tcpci_late_sync.c - Explicit IRQ-arm trigger for TCPC
 *
 * Replaces the CONFIG_TCPC_NOTIFIER_LATE_SYNC late_initcall_sync heuristic.
 * tcpci_late_sync_enable() is called explicitly from rt_pd_manager_probe()
 * after register_tcp_dev_notifier() succeeds, guaranteeing that all notifier
 * consumers (pd_nb, etc.) are in place before hardware IRQs are armed.
 */

#include <linux/device.h>
#include "inc/tcpci.h"

#ifdef CONFIG_USB_POWER_DELIVERY
#ifdef CONFIG_RECV_BAT_ABSENT_NOTIFY
#include "mtk_battery.h"

static int fg_bat_notifier_call(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct pd_port *pd_port = container_of(nb, struct pd_port, fg_bat_nb);
	struct tcpc_device *tcpc = pd_port->tcpc;

	switch (event) {
	case EVENT_BATTERY_PLUG_OUT:
		dev_info(&tcpc->dev, "%s: fg battery absent\n", __func__);
		schedule_work(&pd_port->fg_bat_work);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}
#endif /* CONFIG_RECV_BAT_ABSENT_NOTIFY */
#endif /* CONFIG_USB_POWER_DELIVERY */

/**
 * tcpci_late_sync_enable - Arm TCPC IRQ after all notifier consumers ready
 * @tcpc: TCPC device to arm
 *
 * Caller contract: must be called after register_tcp_dev_notifier() succeeds
 * for all valid consumers.
 *
 * Returns 0 on success, negative errno on failure.
 */
int tcpci_late_sync_enable(struct tcpc_device *tcpc)
{
	int ret;
#if defined(CONFIG_USB_POWER_DELIVERY) && defined(CONFIG_RECV_BAT_ABSENT_NOTIFY)
	struct notifier_block *fg_bat_nb;
#endif

	if (!tcpc)
		return -EINVAL;

	dev_info(&tcpc->dev, "%s: consumer ready, arming IRQ\n", __func__);
	ret = tcpc_device_irq_enable(tcpc);
	if (ret < 0) {
		dev_err(&tcpc->dev, "%s: irq enable failed: %d\n",
			__func__, ret);
		return ret;
	}

#if defined(CONFIG_USB_POWER_DELIVERY) && defined(CONFIG_RECV_BAT_ABSENT_NOTIFY)
	fg_bat_nb = &tcpc->pd_port.fg_bat_nb;
	fg_bat_nb->notifier_call = fg_bat_notifier_call;
	ret = register_battery_notifier(fg_bat_nb);
	if (ret < 0)
		dev_notice(&tcpc->dev,
			   "%s: register bat notifier fail: %d\n",
			   __func__, ret);

	ret = 0;
#endif

	return ret;
}
EXPORT_SYMBOL(tcpci_late_sync_enable);
