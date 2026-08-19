/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __BOARD_ID_H__
#define __BOARD_ID_H__

void board_id_get_hwname(char *str);
int board_id_get_hwlevel(void);
int board_id_get_hwversion_product_num(void);
int board_id_get_hwversion_major_num(void);
int board_id_get_hwversion_minor_num(void);

#endif /* __BOARD_ID_H__ */
