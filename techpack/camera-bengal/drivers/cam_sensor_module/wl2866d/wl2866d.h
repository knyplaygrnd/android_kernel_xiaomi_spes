/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __WL2866D_H__
#define __WL2866D_H__

/**
 * enum wl2866d_out - output rail identifiers.
 *
 * Valid values for the @out_iotype argument of wl2866d_power_control().
 */
enum wl2866d_out {
	OUT_DVDD1 = 0,
	OUT_DVDD2,
	OUT_AVDD1,
	OUT_AVDD2,
};

/**
 * wl2866d_power_control() - enable or disable one output rail.
 * @out_iotype:  one of OUT_DVDD1 .. OUT_AVDD2.
 * @is_power_on: 0 = disable; non-zero = enable.
 *               For OUT_DVDD2, pass 1050000 or 1200000 (uV) to select
 *               the output voltage; any other non-zero value uses the
 *               compile-time default (1.05 V).
 *
 * Returns: 0 on success, -ENODEV if no device is active, negative errno
 *          on hardware error.
 */
int wl2866d_power_control(unsigned int out_iotype, int is_power_on);

#endif /* __WL2866D_H__ */
