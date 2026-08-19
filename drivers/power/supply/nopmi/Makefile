# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(srctree)/drivers/power/supply/syv \
	      -I$(srctree)/drivers/power/supply/maxim
ccflags-$(CONFIG_TCPC_CLASS) += -I$(srctree)/drivers/usb/typec/tcpc

nopmi_chg_core-y := nopmi_chg.o nopmi_chg_jeita.o nopmi_chg_common.o
obj-$(CONFIG_NOPMI_CHARGER) += nopmi_chg_core.o
