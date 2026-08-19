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

#ifndef __PD_DBG_INFO_H__
#define __PD_DBG_INFO_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include "tcpci_config.h"

#ifdef CONFIG_PD_DBG_INFO
int pd_dbg_info(const char *fmt, ...);
void pd_dbg_info_lock(void);
void pd_dbg_info_unlock(void);
int pd_dbg_info_init(void);
void pd_dbg_info_exit(void);
#else
static inline int pd_dbg_info(const char *fmt, ...)
{
	return 0;
}

static inline void pd_dbg_info_lock(void)
{
}

static inline void pd_dbg_info_unlock(void)
{
}

static inline int pd_dbg_info_init(void)
{
	return 0;
}

static inline void pd_dbg_info_exit(void)
{
}

#endif	/* CONFIG_PD_DBG_INFO */

#endif /* __PD_DBG_INFO_H__ */
