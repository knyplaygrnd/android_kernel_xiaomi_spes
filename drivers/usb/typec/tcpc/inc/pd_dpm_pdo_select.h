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

#ifndef __PD_DPM_PDO_SELECT_H__
#define __PD_DPM_PDO_SELECT_H__

#include "tcpci.h"

struct dpm_pdo_info_t {
	u8 type;
	u8 apdo_type;
	u8 pwr_limit;
	int vmin;
	int vmax;
	int uw;
	int ma;
};

struct dpm_rdo_info_t {
	u8 pos;
	u8 type;
	bool mismatch;

	int vmin;
	int vmax;

	union {
		u32 max_uw;
		u32 max_ma;
	};

	union {
		u32 oper_uw;
		u32 oper_ma;
	};
};

#define DPM_PDO_TYPE_FIXED	TCPM_POWER_CAP_VAL_TYPE_FIXED
#define DPM_PDO_TYPE_VAR	TCPM_POWER_CAP_VAL_TYPE_VARIABLE
#define DPM_PDO_TYPE_BAT	TCPM_POWER_CAP_VAL_TYPE_BATTERY
#define DPM_PDO_TYPE_APDO	TCPM_POWER_CAP_VAL_TYPE_AUGMENT

#define DPM_APDO_TYPE_PPS	(TCPM_POWER_CAP_APDO_TYPE_PPS)
#define DPM_APDO_TYPE_PPS_CF	(TCPM_POWER_CAP_APDO_TYPE_PPS_CF)

void dpm_extract_pdo_info(u32 pdo, struct dpm_pdo_info_t *info);

bool dpm_find_match_req_info(struct dpm_rdo_info_t *req_info,
			     struct dpm_pdo_info_t *sink,
			     int cnt, u32 *src_pdos,
			     int max_uw, u32 select_rule);

#endif	/* __PD_DPM_PDO_SELECT_H__ */
