# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += $(ccflags-nopmi-base) \
	    -I$(srctree)/drivers/power/supply/syv
ccflags-$(CONFIG_TCPC_CLASS) += -I$(srctree)/drivers/usb/typec/tcpc

obj-$(CONFIG_CHARGER_SC8551) += sc8551_charger.o
obj-$(CONFIG_CHARGER_PD_POLICY) += pd_policy_manager.o
