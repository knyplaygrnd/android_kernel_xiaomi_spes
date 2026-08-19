# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += $(ccflags-nopmi-base)
ccflags-$(CONFIG_TCPC_CLASS) += -I$(srctree)/drivers/usb/typec/tcpc

obj-$(CONFIG_CHARGER_BQ2589X) += bq2589x_charger.o
obj-$(CONFIG_FUELGAUGE_SM5602) += sm5602_fuelgauge.o
