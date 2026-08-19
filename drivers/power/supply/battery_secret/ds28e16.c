// SPDX-License-Identifier: GPL-2.0-only
/*
 * ds28e16 - DeepCover Secure Authenticator driver
 *
 * Copyright (C) 2015 Maxim Integrated Products, Inc.
 * Copyright (C) 2022 Xiaomi Inc.
 *
 * Communicates with the DS28E16 over a bit-bang 1-Wire bus (onewire_gpio).
 * Authentication state is cached per-device instance for the lifetime of the
 * driver binding.
 *
 */
#define pr_fmt(fmt) "[ds28e16]: %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "ds28e16.h"
#include "onewire_gpio.h"
#include "sha384_software.h"

/**
 * struct ds28e16_data - all runtime state for one DS28E16 instance.
 */
struct ds28e16_data {
	struct platform_device *pdev;
	struct device *dev;
	int version;

	struct power_supply *verify_psy;
	struct power_supply_desc verify_psy_d;

	struct delayed_work authentic_work;

	bool batt_verified;
	bool romid_verified;
	int mi_auth_result;

	int page0_val;

	struct mutex cmd_lock;
	struct mutex cfg_lock;

	u8 last_result_byte;
	u8 manid[2];

	u8 flag_mi_romid;
	u8 mi_romid[8];

	bool flag_mi_status;
	u8 mi_status[7];

	bool flag_mi_page0_data;
	u8 mi_page0_data[16];

	bool flag_mi_page1_data;
	u8 mi_page1_data[16];

	bool flag_mi_counter;
	u8 mi_counter[16];

	u8 session_seed[32];
	u8 s_secret[32];
	u8 challenge[32];

	bool auth_anon;		/* true = anonymous mode */
	bool auth_bdconst;	/* true = use binding data constant */
	int pagenumber;		/* page queried during authentication */

	u32 attr_trytimes;

	int auth_retry_count;

	char ow_bus_label[32];
	struct onewire_bus *ow_bus;
};

/*
 * crc_low_first - Dallas/Maxim 1-Wire CRC-8 (polynomial 0x8C, LSB first).
 * Used to verify the 8-byte ROM ID.
 */
static u8 crc_low_first(const u8 *ptr, u8 len)
{
	u8 i, crc = 0x00;

	while (len--) {
		crc ^= *ptr++;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x01) ? (crc >> 1) ^ 0x8c : (crc >> 1);
	}
	return crc;
}

/*
 * docrc16 - update a CRC-16 (Dallas variant) with one byte.
 *
 * Pure function: takes the current crc value and the new data byte,
 * returns the updated crc.
 */
static const u8 oddparity[16] = {
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0
};

static u16 docrc16(u16 crc16, u16 data)
{
	data = (data ^ (crc16 & 0xff)) & 0xff;
	crc16 >>= 8;
	if (oddparity[data & 0xf] ^ oddparity[data >> 4])
		crc16 ^= 0xc001;
	data <<= 6;
	crc16 ^= data;
	data <<= 1;
	crc16 ^= data;
	return crc16;
}

/*
 * Thin inline helpers that forward calls through the acquired
 * struct onewire_bus pointer.
 *
 * All callers must hold ds->cmd_lock.
 */
static inline u8 ds_ow_reset(struct ds28e16_data *ds)
{
	return ds->ow_bus->ops->reset(ds->ow_bus);
}

static inline u8 ds_ow_read_byte(struct ds28e16_data *ds)
{
	return ds->ow_bus->ops->read_byte(ds->ow_bus);
}

static inline void ds_ow_write_byte(struct ds28e16_data *ds, u8 val)
{
	ds->ow_bus->ops->write_byte(ds->ow_bus, val);
}

static inline void ds_ow_software_reset(struct ds28e16_data *ds)
{
	ds->ow_bus->ops->software_reset(ds->ow_bus);
}

/**
 * ds28e16_standard_cmd_flow - execute one DS28E16 function command.
 *
 * @ds:        device instance
 * @write_buf: command frame: [length_byte, cmd, params...]
 * @write_len: total bytes in write_buf
 * @delay_ms:  strong pull-up hold time in ms; 0 = no pull-up needed
 * @read_buf:  output buffer (caller must size to *read_len bytes)
 * @read_len:  in: expected response length; out: actual response length
 *
 * Acquires and releases ds->cmd_lock internally.  Callers must NOT hold
 * cmd_lock when calling this function.
 *
 * On any bus or CRC error the bus is reset before the lock is released,
 * leaving the line in a known idle state for the next transaction.
 *
 * Returns DS_TRUE on success, DS_FALSE / ERROR_NO_DEVICE on failure.
 *
 * Wire sequence:
 *    <Reset> <SKIP ROM> <START(0x66)> <write_buf> <CRC16-check>
 *    [RELEASE + delay] <dummy> <len> <result+data> <CRC16-check>
 */
static int ds28e16_standard_cmd_flow(struct ds28e16_data *ds,
				     u8 *write_buf, int write_len,
				     int delay_ms,
				     u8 *read_buf, int *read_len)
{
	u8 buf[128];
	u16 crc16;
	int i, buf_len = 0;
	int expected_read_len = *read_len;

	mutex_lock(&ds->cmd_lock);

	if (ds_ow_reset(ds) != 0) {
		ds_ow_software_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		pr_err("reset: no device\n");
		return ERROR_NO_DEVICE;
	}

	ds_ow_write_byte(ds, CMD_SKIP_ROM);

	buf[buf_len++] = CMD_START;
	memcpy(&buf[buf_len], write_buf, write_len);
	buf_len += write_len;

	for (i = 0; i < buf_len; i++)
		ds_ow_write_byte(ds, buf[i]);

	buf[buf_len++] = ds_ow_read_byte(ds);
	buf[buf_len++] = ds_ow_read_byte(ds);

	crc16 = 0;
	for (i = 0; i < buf_len; i++)
		crc16 = docrc16(crc16, buf[i]);

	if (crc16 != 0xB001) {
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		pr_err("TX CRC error\n");
		return DS_FALSE;
	}

	if (delay_ms > 0) {
		ds_ow_write_byte(ds, CMD_RELEASE_BYTE);
		msleep(delay_ms);
	}

	ds_ow_read_byte(ds);
	*read_len = ds_ow_read_byte(ds);

	if (*read_len != expected_read_len) {
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		pr_err("length mismatch: got %d expected %d\n",
		       *read_len, expected_read_len);
		return DS_FALSE;
	}

	buf_len = *read_len + 2;
	for (i = 0; i < buf_len; i++)
		buf[i] = ds_ow_read_byte(ds);

	crc16 = 0;
	crc16 = docrc16(crc16, (u16)*read_len);
	for (i = 0; i < buf_len; i++)
		crc16 = docrc16(crc16, buf[i]);

	if (crc16 != 0xB001) {
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		pr_err("RX CRC error\n");
		return DS_FALSE;
	}

	memcpy(read_buf, buf, *read_len);
	mutex_unlock(&ds->cmd_lock);
	return DS_TRUE;
}

/**
 * ds28e16_read_romid - read the 8-byte 1-Wire ROM ID from the device.
 *
 * Serves the cached value if a valid read has been performed previously.
 * On a fresh read the CRC-8 is verified and the family + custom-ID bytes
 * are checked to confirm a DS28E16.
 *
 * The cache check, bus transaction, and cache update all run under cmd_lock
 * to prevent a concurrent reader from observing a partially-updated mi_romid.
 */
static int ds28e16_read_romid(struct ds28e16_data *ds, u8 *romid)
{
	u8 i, crc;

	mutex_lock(&ds->cmd_lock);

	if (ds->flag_mi_romid == 2 && ds->mi_romid[0] != 0x1f) {
		memcpy(romid, ds->mi_romid, 8);
		mutex_unlock(&ds->cmd_lock);
		return DS_TRUE;
	}

	if (ds_ow_reset(ds) != 0) {
		ds_ow_software_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		pr_err("reset: no device\n");
		return ERROR_NO_DEVICE;
	}

	ds_ow_write_byte(ds, CMD_READ_ROM);
	usleep_range(10, 20);
	for (i = 0; i < 8; i++)
		romid[i] = ds_ow_read_byte(ds);

	crc = crc_low_first(romid, 7);
	if (crc != romid[7]) {
		mutex_unlock(&ds->cmd_lock);
		pr_err("ROM ID CRC mismatch\n");
		return DS_FALSE;
	}

	memcpy(ds->mi_romid, romid, 8);

	if (ds->mi_romid[0] == FAMILY_CODE &&
	    ds->mi_romid[6] == CUSTOM_ID_MSB &&
	    (ds->mi_romid[5] & 0xf0) == CUSTOM_ID_LSB)
		ds->flag_mi_romid = 2;
	else
		ds->flag_mi_romid = 1;

	pr_debug("RomID: %8phC\n", romid);

	mutex_unlock(&ds->cmd_lock);
	return DS_TRUE;
}

/**
 * ds28e16_virtual_cmd_read_status - raw 1-Wire sequence used to wake the
 * device during ROM ID retry recovery.
 */
static void ds28e16_virtual_cmd_read_status(struct ds28e16_data *ds)
{
	int i;

	mutex_lock(&ds->cmd_lock);
	ds_ow_reset(ds);
	ds_ow_write_byte(ds, CMD_SKIP_ROM);
	ds_ow_write_byte(ds, CMD_START);
	ds_ow_write_byte(ds, 0x01);
	ds_ow_write_byte(ds, CMD_READ_STATUS);
	ds_ow_read_byte(ds);
	ds_ow_read_byte(ds);
	ds_ow_write_byte(ds, CMD_RELEASE_BYTE);
	msleep(50);
	for (i = 0; i < 11; i++)
		ds_ow_read_byte(ds);
	ds_ow_reset(ds);
	mutex_unlock(&ds->cmd_lock);
}

/**
 * ds28e16_cmd_read_status - read the 7-byte status / protection register.
 *
 * Byte layout returned (0-indexed from data[0]):
 *    [0..1]  page protection for page 0 and page 1
 *    [2]     counter page protection
 *    [3]     reserved
 *    [4]     MANID byte 0
 *    [5]     MANID byte 1
 *    [6]     chip version
 *
 * Callers must NOT hold cmd_lock.
 */
static int ds28e16_cmd_read_status(struct ds28e16_data *ds, u8 *data)
{
	u8 write_buf[4];
	u8 read_buf[16];
	int write_len = 0;
	int read_len = 7;

	mutex_lock(&ds->cmd_lock);
	if (ds->flag_mi_status) {
		memcpy(data, ds->mi_status, 7);
		mutex_unlock(&ds->cmd_lock);
		return DS_TRUE;
	}
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 1;
	write_buf[write_len++] = CMD_READ_STATUS;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_READ,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	memcpy(data, &read_buf[1], 7);

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	memcpy(ds->mi_status, data, 7);
	ds->manid[0] = data[4];
	ds->flag_mi_status = true;
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_read_memory - read one user-memory or counter page.
 *
 * @pg:   0 = page 0, 1 = page 1, 2 = decrement counter page
 * @data: output buffer; must be at least 16 bytes
 *
 * The device returns 33 bytes: 1 result byte + 16 data bytes +
 * 16 zero-padding bytes. Only the first 16 data bytes are copied
 * to the caller's buffer.
 *
 * Callers must NOT hold cmd_lock.
 */
static int ds28e16_cmd_read_memory(struct ds28e16_data *ds,
				   int pg, u8 *data)
{
	u8 write_buf[4];
	u8 read_buf[64];
	int write_len = 0;
	int read_len = 33;
	u8 pagenum = (u8)pg & 0x03;

	mutex_lock(&ds->cmd_lock);
	switch (pagenum) {
	case 0x00:
		if (ds->flag_mi_page0_data) {
			memcpy(data, ds->mi_page0_data, 16);
			mutex_unlock(&ds->cmd_lock);
			return DS_TRUE;
		}
		break;
	case 0x01:
		if (ds->flag_mi_page1_data) {
			memcpy(data, ds->mi_page1_data, 16);
			mutex_unlock(&ds->cmd_lock);
			return DS_TRUE;
		}
		break;
	case 0x02:
		if (ds->flag_mi_counter) {
			memcpy(data, ds->mi_counter, 16);
			mutex_unlock(&ds->cmd_lock);
			return DS_TRUE;
		}
		break;
	default:
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 2;
	write_buf[write_len++] = CMD_READ_MEM;
	write_buf[write_len++] = pagenum;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_READ,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_len != 33) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	if (read_buf[0] != RESULT_SUCCESS &&
	    !(read_buf[0] == 0x55 && pagenum == 0x02)) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	memcpy(data, &read_buf[1], 16);

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	switch (pagenum) {
	case 0x00:
		ds->flag_mi_page0_data = true;
		memcpy(ds->mi_page0_data, data, 16);
		break;
	case 0x01:
		ds->flag_mi_page1_data = true;
		memcpy(ds->mi_page1_data, data, 16);
		break;
	case 0x02:
		ds->flag_mi_counter = true;
		memcpy(ds->mi_counter, data, 16);
		break;
	}
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_write_memory - write 16 bytes to a user-memory page.
 *
 * Invalidates the corresponding cache on success.
 */
static int ds28e16_cmd_write_memory(struct ds28e16_data *ds,
				    int pg, u8 *data)
{
	u8 write_buf[32];
	u8 read_buf[4];
	int write_len = 0;
	int read_len = 1;
	u8 pagenum = (u8)pg & 0x03;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 18;
	write_buf[write_len++] = CMD_WRITE_MEM;
	write_buf[write_len++] = pagenum;
	memcpy(&write_buf[write_len], data, 16);
	write_len += 16;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_WRITE,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_len != 1 || read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	switch (pagenum) {
	case 0x00:
		ds->flag_mi_page0_data = false;
		memset(ds->mi_page0_data, 0x00, 16);
		break;
	case 0x01:
		ds->flag_mi_page1_data = false;
		memset(ds->mi_page1_data, 0x00, 16);
		break;
	case 0x02:
		ds->flag_mi_counter = false;
		memset(ds->mi_counter, 0x00, 16);
		break;
	}
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_decrement_counter - decrement the 17-bit monotonic counter.
 */
static __maybe_unused int ds28e16_cmd_decrement_counter(struct ds28e16_data *ds)
{
	u8 write_buf[4];
	u8 read_buf[4];
	int write_len = 0;
	int read_len = 1;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 1;
	write_buf[write_len++] = CMD_DECREMENT_CNT;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      50, read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_len != 1 || read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_set_page_protection - set read/write protection on a page.
 */
static __maybe_unused int ds28e16_cmd_set_page_protection(struct ds28e16_data *ds,
							  int page,
							  u8 prot)
{
	u8 write_buf[8];
	u8 read_buf[4];
	int write_len = 0;
	int read_len = 1;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 3;
	write_buf[write_len++] = CMD_SET_PAGE_PROT;
	write_buf[write_len++] = page & 0x03;
	write_buf[write_len++] = prot & 0x03;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_WRITE,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_len != 1 || read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_device_disable - permanently disable the device.
 *
 * @op:       operation code (0x0F = disable)
 * @password: 8-byte authorisation password
 *
 * WARNING: irreversible.
 */
static __maybe_unused int ds28e16_cmd_device_disable(struct ds28e16_data *ds,
						     int op,
						     u8 *password)
{
	u8 write_buf[16];
	u8 read_buf[4];
	int write_len = 0;
	int read_len = 1;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 10;
	write_buf[write_len++] = CMD_DISABLE_DEVICE;
	write_buf[write_len++] = op & 0x0F;
	memcpy(&write_buf[write_len], password, 8);
	write_len += 8;

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_WRITE,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_len != 1 || read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

/**
 * ds28e16_cmd_compute_read_page_auth - request a device MAC.
 *
 * @anon:      if non-zero, substitute 0xFF for the ROM ID in the hash input
 * @pg:        page number whose data is included in the MAC
 * @challenge: 32-byte nonce
 * @hmac:      output - 32-byte HMAC-SHA3-256 computed by the device
 */
static int ds28e16_cmd_compute_read_page_auth(struct ds28e16_data *ds,
					      int anon, int pg,
					      u8 *challenge,
					      u8 *hmac)
{
	u8 write_buf[40];
	u8 read_buf[40];
	int write_len = 0;
	int read_len = 33;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	write_buf[write_len++] = 35;
	write_buf[write_len++] = CMD_COMP_READ_AUTH;
	write_buf[write_len] = pg & 0x03;
	if (anon)
		write_buf[write_len] |= 0xE0;
	write_len++;
	write_buf[write_len++] = 0x02;
	memcpy(&write_buf[write_len], challenge, 32);
	write_len += 32;

	pr_debug("write_buf: %35ph\n", write_buf);

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_WRITE,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	mutex_unlock(&ds->cmd_lock);

	memcpy(hmac, &read_buf[1], 32);

	pr_debug("device HMAC: %32ph\n", hmac);
	return DS_TRUE;
}

/**
 * ds28e16_cmd_compute_s_secret - instruct the device to compute its
 * session secret from the partial secret (session seed) provided by
 * the host.
 *
 * @anon:     anonymous mode flag
 * @bdconst:  use binding data constant flag
 * @pg:       page number to bind the secret to
 * @partial:  32-byte partial secret (session seed)
 */
static int ds28e16_cmd_compute_s_secret(struct ds28e16_data *ds,
					int anon, int bdconst,
					int pg, u8 *partial)
{
	u8 write_buf[40];
	u8 read_buf[4];
	int write_len = 0;
	int read_len = 1;
	int param = pg & 0x03;

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = RESULT_FAIL_NONE;
	mutex_unlock(&ds->cmd_lock);

	if (bdconst)
		param |= 0x04;
	if (anon)
		param |= 0xE0;

	write_buf[write_len++] = 35;
	write_buf[write_len++] = CMD_COMP_S_SECRET;
	write_buf[write_len++] = (u8)param;
	write_buf[write_len++] = 0x08;
	memcpy(&write_buf[write_len], partial, 32);
	write_len += 32;

	pr_debug("write_buf: %35ph\n", write_buf);

	if (ds28e16_standard_cmd_flow(ds, write_buf, write_len,
				      DELAY_DS28E16_EE_WRITE,
				      read_buf, &read_len) != DS_TRUE)
		return DS_FALSE;

	if (read_buf[0] != RESULT_SUCCESS) {
		mutex_lock(&ds->cmd_lock);
		ds_ow_reset(ds);
		mutex_unlock(&ds->cmd_lock);
		return DS_FALSE;
	}

	mutex_lock(&ds->cmd_lock);
	ds->last_result_byte = read_buf[0];
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

static int ds28e16_read_romid_retry(struct ds28e16_data *ds, u8 *romid)
{
	int i;

	for (i = 0;
	    i < 5 && (ds->mi_romid[0] == 0x1f || ds->mi_romid[0] == 0x00);
	    i++) {
		pr_info("ROM ID recovery attempt %d\n", i);
		ds28e16_virtual_cmd_read_status(ds);
		ds28e16_read_romid(ds, romid);
		usleep_range(100, 200);
	}

	for (i = 0; i < GET_ROM_ID_RETRY; i++) {
		pr_info("read ROM ID attempt %d\n", i);
		if (ds28e16_read_romid(ds, romid) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

static int ds28e16_get_page_status_retry(struct ds28e16_data *ds, u8 *data)
{
	int i;

	for (i = 0; i < GET_BLOCK_STATUS_RETRY; i++) {
		pr_info("read page status attempt %d\n", i);
		if (ds28e16_cmd_read_status(ds, data) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

static int ds28e16_get_page_data_retry(struct ds28e16_data *ds,
				       int page, u8 *data)
{
	int i;

	if (page >= MAX_PAGENUM)
		return DS_FALSE;

	for (i = 0; i < GET_USER_MEMORY_RETRY; i++) {
		pr_debug("read page %d attempt %d\n", page, i);
		if (ds28e16_cmd_read_memory(ds, page, data) == DS_TRUE) {
			pr_debug("page %d data: %16ph\n", page, data);
			return DS_TRUE;
		}
	}

	return DS_FALSE;
}

static int ds28e16_cmd_compute_s_secret_retry(struct ds28e16_data *ds,
					      int anon, int bdconst,
					      int pg, u8 *partial)
{
	int i;

	if (pg >= MAX_PAGENUM)
		return DS_FALSE;

	for (i = 0; i < GET_S_SECRET_RETRY; i++) {
		if (ds28e16_cmd_compute_s_secret(ds, anon, bdconst,
						 pg, partial) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

static int ds28e16_cmd_compute_read_page_auth_retry(struct ds28e16_data *ds,
						    int anon, int pg,
						    u8 *challenge, u8 *hmac)
{
	int i;

	if (pg >= MAX_PAGENUM)
		return DS_FALSE;

	for (i = 0; i < GET_MAC_RETRY; i++) {
		if (ds28e16_cmd_compute_read_page_auth(ds,
						       anon, pg, challenge, hmac) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

/**
 * ds28e16_authenticate - full HMAC-SHA3-256 challenge-response authentication.
 *
 * Snapshots all volatile configuration (auth params, session_seed, s_secret,
 * challenge) under cfg_lock at the start, then releases cfg_lock before
 * performing any bus I/O.  This prevents a deadlock between cfg_lock and
 * cmd_lock while still providing a consistent view of the parameters for one
 * full authentication round.
 *
 * Returns DS_TRUE on success, or an ERROR_* code on failure.
 */
static int ds28e16_authenticate(struct ds28e16_data *ds)
{
	int auth_anon, auth_bdconst, pagenumber;
	u8 session_seed[32];
	u8 s_secret[32];
	u8 challenge[32];
	u8 page_data[16];
	u8 mac_from_device[32];
	u8 mac_computed[32];
	u8 status_chip[16];
	u8 msg[128];
	int msg_len = 0;
	int i;

	if (READ_ONCE(ds->mi_auth_result) == DS_TRUE)
		return DS_TRUE;

	mutex_lock(&ds->cfg_lock);
	auth_anon = ds->auth_anon;
	auth_bdconst = ds->auth_bdconst;
	pagenumber = ds->pagenumber;
	memcpy(session_seed, ds->session_seed, 32);
	memcpy(s_secret, ds->s_secret, 32);
	memcpy(challenge, ds->challenge, 32);
	mutex_unlock(&ds->cfg_lock);

	if (ds28e16_read_romid_retry(ds, ds->mi_romid) != DS_TRUE) {
		pr_err("read_romid_retry failed\n");
		return ERROR_R_ROMID;
	}

	if (ds28e16_get_page_status_retry(ds, status_chip) == DS_TRUE) {
		mutex_lock(&ds->cmd_lock);
		ds->manid[0] = status_chip[4];
		mutex_unlock(&ds->cmd_lock);
	} else {
		pr_err("read_status failed\n");
		return ERROR_R_STATUS;
	}

	if (!ds28e16_cmd_compute_s_secret_retry(ds,
						auth_anon, auth_bdconst,
						0, session_seed)) {
		pr_err("compute_s_secret failed\n");
		return ERROR_S_SECRET;
	}

	if (!ds28e16_cmd_compute_read_page_auth_retry(ds,
						      auth_anon, pagenumber,
						      challenge, mac_from_device)) {
		pr_err("compute_read_page_auth failed\n");
		return ERROR_COMPUTE_MAC;
	}

	pr_debug("session_seed: %32ph\n", session_seed);
	pr_debug("s_secret: %32ph\n", s_secret);
	pr_debug("challenge: %32ph\n", challenge);
	pr_debug("device MAC: %32ph\n", mac_from_device);

	if (ds28e16_get_page_data_retry(ds, pagenumber, page_data) != DS_TRUE) {
		pr_err("read_memory failed\n");
		return ERROR_R_PAGEDATA;
	}

	if (auth_anon != ANONYMOUS)
		memcpy(&msg[msg_len], ds->mi_romid, 8);
	else
		memset(&msg[msg_len], 0xff, 8);
	msg_len += 8;

	memcpy(&msg[msg_len], page_data, 16);
	msg_len += 16;
	memset(&msg[msg_len], 0x00, 16);
	msg_len += 16;

	memcpy(&msg[msg_len], challenge, 32);
	msg_len += 32;

	msg[msg_len++] = pagenumber & 0x03;

	mutex_lock(&ds->cmd_lock);
	memcpy(&msg[msg_len], ds->manid, 2);
	mutex_unlock(&ds->cmd_lock);
	msg_len += 2;

	pr_debug("host MAC input: %80ph\n", msg);

	sha3_256_hmac(s_secret, 32, msg, msg_len, mac_computed);

	pr_debug("host MAC: %32ph\n", mac_computed);

	for (i = 0; i < 32; i++) {
		if (mac_computed[i] != mac_from_device[i])
			break;
	}

	if (i != 32) {
		pr_err("MAC mismatch at byte %d\n", i);
		mutex_lock(&ds->cmd_lock);
		ds->flag_mi_page1_data = false;
		mutex_unlock(&ds->cmd_lock);
		return ERROR_UNMATCH_MAC;
	}

	pr_info("authentication successful\n");
	WRITE_ONCE(ds->mi_auth_result, DS_TRUE);
	return DS_TRUE;
}

static int ds28e16_get_chip_ok(struct ds28e16_data *ds, int *val)
{
	int ret;

	*val = 0;

	if (READ_ONCE(ds->romid_verified)) {
		*val = 1;
		return DS_TRUE;
	}

	ret = ds28e16_read_romid_retry(ds, ds->mi_romid);
	if (ret != DS_TRUE) {
		pr_err("read_romid_retry failed\n");
		return -EAGAIN;
	}

	mutex_lock(&ds->cmd_lock);
	if (ds->mi_romid[0] == FAMILY_CODE &&
	    ds->mi_romid[6] == CUSTOM_ID_MSB &&
	    (ds->mi_romid[5] & 0xf0) == CUSTOM_ID_LSB) {
		*val = 1;
		WRITE_ONCE(ds->romid_verified, true);
	}
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

static int ds28e16_get_page0_data(struct ds28e16_data *ds,
				  u8 *buf, int buf_len)
{
	int ret;

	if (buf_len < 16)
		return -EINVAL;

	ret = ds28e16_get_page_data_retry(ds, 0, buf);
	if (ret != DS_TRUE) {
		pr_err("get_page_data_retry(0) failed\n");
		return -EAGAIN;
	}

	mutex_lock(&ds->cmd_lock);
	ds->page0_val = buf[0];
	mutex_unlock(&ds->cmd_lock);

	return DS_TRUE;
}

static enum power_supply_property verify_props[] = {
	POWER_SUPPLY_PROP_ROMID,
	POWER_SUPPLY_PROP_DS_STATUS,
	POWER_SUPPLY_PROP_PAGENUMBER,
	POWER_SUPPLY_PROP_PAGEDATA,
	POWER_SUPPLY_PROP_AUTHEN_RESULT,
	POWER_SUPPLY_PROP_SESSION_SEED,
	POWER_SUPPLY_PROP_S_SECRET,
	POWER_SUPPLY_PROP_CHALLENGE,
	POWER_SUPPLY_PROP_AUTH_ANON,
	POWER_SUPPLY_PROP_AUTH_BDCONST,
	POWER_SUPPLY_PROP_PAGE0_DATA,
	POWER_SUPPLY_PROP_PAGE1_DATA,
	POWER_SUPPLY_PROP_VERIFY_MODEL_NAME,
	POWER_SUPPLY_PROP_CHIP_OK,
};

static int verify_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct ds28e16_data *ds = power_supply_get_drvdata(psy);
	u8 buf[50];
	int tmp, ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_VERIFY_MODEL_NAME:
		ret = ds28e16_read_romid_retry(ds, ds->mi_romid);
		val->strval = (ret == DS_TRUE) ? "ds28e16" : "unknown";
		break;

	case POWER_SUPPLY_PROP_AUTHEN_RESULT:
		val->intval = READ_ONCE(ds->batt_verified) ? 1 : 0;
		break;

	case POWER_SUPPLY_PROP_SESSION_SEED:
		mutex_lock(&ds->cfg_lock);
		memcpy(val->arrayval, ds->session_seed, 32);
		mutex_unlock(&ds->cfg_lock);
		break;

	case POWER_SUPPLY_PROP_S_SECRET:
		mutex_lock(&ds->cfg_lock);
		memcpy(val->arrayval, ds->s_secret, 32);
		mutex_unlock(&ds->cfg_lock);
		break;

	case POWER_SUPPLY_PROP_CHALLENGE:
		mutex_lock(&ds->cfg_lock);
		memcpy(val->arrayval, ds->challenge, 32);
		mutex_unlock(&ds->cfg_lock);
		break;

	case POWER_SUPPLY_PROP_PAGENUMBER:
		mutex_lock(&ds->cfg_lock);
		val->intval = ds->pagenumber;
		mutex_unlock(&ds->cfg_lock);
		break;

	case POWER_SUPPLY_PROP_ROMID:
		ret = ds28e16_read_romid_retry(ds, ds->mi_romid);
		if (ret != DS_TRUE)
			return -EAGAIN;
		mutex_lock(&ds->cmd_lock);
		memcpy(val->arrayval, ds->mi_romid, 8);
		mutex_unlock(&ds->cmd_lock);
		break;

	case POWER_SUPPLY_PROP_CHIP_OK:
		if (READ_ONCE(ds->romid_verified)) {
			val->intval = 1;
			break;
		}
		ret = ds28e16_get_chip_ok(ds, &tmp);
		if (ret != DS_TRUE)
			return -EAGAIN;
		val->intval = tmp ? 1 : 0;
		break;

	case POWER_SUPPLY_PROP_DS_STATUS:
		ret = ds28e16_get_page_status_retry(ds, buf);
		if (ret != DS_TRUE)
			return -EAGAIN;
		memcpy(val->arrayval, buf, 7);
		break;

	case POWER_SUPPLY_PROP_PAGEDATA:
		mutex_lock(&ds->cfg_lock);
		tmp = ds->pagenumber;
		mutex_unlock(&ds->cfg_lock);
		ret = ds28e16_get_page_data_retry(ds, tmp, buf);
		if (ret != DS_TRUE)
			return -EAGAIN;
		memcpy(val->arrayval, buf, 16);
		break;

	case POWER_SUPPLY_PROP_PAGE0_DATA:
		ret = ds28e16_get_page0_data(ds, buf, sizeof(buf));
		if (ret != DS_TRUE)
			return -EAGAIN;
		memcpy(val->arrayval, buf, 16);
		break;

	case POWER_SUPPLY_PROP_PAGE1_DATA:
		ret = ds28e16_get_page_data_retry(ds, 1, buf);
		if (ret != DS_TRUE)
			return -EAGAIN;
		memcpy(val->arrayval, buf, 16);
		break;

	default:
		pr_debug("unsupported property %d\n", psp);
		return -ENODATA;
	}

	return 0;
}

static int verify_set_property(struct power_supply *psy,
			       enum power_supply_property prop,
			       const union power_supply_propval *val)
{
	struct ds28e16_data *ds = power_supply_get_drvdata(psy);

	mutex_lock(&ds->cfg_lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_PAGENUMBER:
		ds->pagenumber = val->intval;
		break;
	case POWER_SUPPLY_PROP_AUTH_ANON:
		ds->auth_anon = !!val->intval;
		break;
	case POWER_SUPPLY_PROP_AUTH_BDCONST:
		ds->auth_bdconst = !!val->intval;
		break;
	default:
		mutex_unlock(&ds->cfg_lock);
		pr_debug("unsupported property %d\n", prop);
		return -ENODATA;
	}
	mutex_unlock(&ds->cfg_lock);

	return 0;
}

static int verify_prop_is_writeable(struct power_supply *psy,
				    enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_PAGENUMBER:
	case POWER_SUPPLY_PROP_AUTH_ANON:
	case POWER_SUPPLY_PROP_AUTH_BDCONST:
		return 1;
	default:
		return 0;
	}
}

static int verify_psy_register(struct ds28e16_data *ds)
{
	struct power_supply_config cfg = {};

	ds->verify_psy_d.name = "batt_verify";
	ds->verify_psy_d.type = POWER_SUPPLY_TYPE_BATTERY_VERIFY;
	ds->verify_psy_d.properties = verify_props;
	ds->verify_psy_d.num_properties = ARRAY_SIZE(verify_props);
	ds->verify_psy_d.get_property = verify_get_property;
	ds->verify_psy_d.set_property = verify_set_property;
	ds->verify_psy_d.property_is_writeable = verify_prop_is_writeable;

	cfg.drv_data = ds;
	cfg.of_node = ds->dev->of_node;
	cfg.num_supplicants = 0;

	ds->verify_psy = devm_power_supply_register(ds->dev,
						    &ds->verify_psy_d, &cfg);
	if (IS_ERR(ds->verify_psy)) {
		pr_err("devm_power_supply_register failed\n");
		return PTR_ERR(ds->verify_psy);
	}

	pr_info("%s registered\n", ds->verify_psy_d.name);
	return 0;
}

static ssize_t ds_auth_result_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int result = ds28e16_authenticate(ds);

	switch (result) {
	case DS_TRUE:
		return scnprintf(buf, PAGE_SIZE, "Authenticate success\n");
	case ERROR_R_STATUS:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: ERROR_R_STATUS\n");
	case ERROR_UNMATCH_MAC:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: MAC mismatch\n");
	case ERROR_R_ROMID:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: ERROR_R_ROMID\n");
	case ERROR_COMPUTE_MAC:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: ERROR_COMPUTE_MAC\n");
	case ERROR_S_SECRET:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: ERROR_S_SECRET\n");
	default:
		return scnprintf(buf, PAGE_SIZE,
				 "Authenticate failed: unknown (%d)\n", result);
	}
}

static ssize_t ds_romid_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 romid[8] = {};
	u32 tries;
	int i, count = 0;
	int status;

	mutex_lock(&ds->cfg_lock);
	tries = ds->attr_trytimes;
	mutex_unlock(&ds->cfg_lock);

	for (i = 0; i < tries; i++) {
		status = ds28e16_read_romid_retry(ds, romid);
		if (status == DS_TRUE)
			count++;
		usleep_range(1000, 1200);
	}

	return scnprintf(buf, PAGE_SIZE,
			 "Success=%d RomID=%8phC\n", count, romid);
}

static ssize_t ds_pagenumber_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val;

	mutex_lock(&ds->cfg_lock);
	val = ds->pagenumber;
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ds_pagenumber_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val >= 0 && val <= 3) {
		mutex_lock(&ds->cfg_lock);
		ds->pagenumber = val;
		mutex_unlock(&ds->cfg_lock);
	}

	return count;
}

static ssize_t ds_pagedata_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 pagedata[16] = {};
	u32 tries;
	int pagenum, i, count = 0;

	mutex_lock(&ds->cfg_lock);
	tries = ds->attr_trytimes;
	pagenum = ds->pagenumber;
	mutex_unlock(&ds->cfg_lock);

	for (i = 0; i < tries; i++) {
		if (ds28e16_get_page_data_retry(ds, pagenum,
						pagedata) == DS_TRUE)
			count++;
		usleep_range(1000, 1200);
	}

	return scnprintf(buf, PAGE_SIZE,
			 "Success=%d data=%16ph\n", count, pagedata);
}

static ssize_t ds_pagedata_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 pagedata[16] = {};
	int pagenum;

	if (sscanf(buf,
		   "%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx",
		   &pagedata[0], &pagedata[1], &pagedata[2], &pagedata[3],
		   &pagedata[4], &pagedata[5], &pagedata[6], &pagedata[7],
		   &pagedata[8], &pagedata[9], &pagedata[10], &pagedata[11],
		   &pagedata[12], &pagedata[13], &pagedata[14], &pagedata[15])
	    != 16)
		return -EINVAL;

	mutex_lock(&ds->cfg_lock);
	pagenum = ds->pagenumber;
	mutex_unlock(&ds->cfg_lock);

	pr_debug("write page %d: %16ph\n", pagenum, pagedata);

	if (ds28e16_cmd_write_memory(ds, pagenum, pagedata) != DS_TRUE)
		pr_err("write_memory failed\n");

	return count;
}

static ssize_t ds_time_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u32 val;

	mutex_lock(&ds->cfg_lock);
	val = ds->attr_trytimes;
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t ds_time_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 0) {
		mutex_lock(&ds->cfg_lock);
		ds->attr_trytimes = val;
		mutex_unlock(&ds->cfg_lock);
	}

	return count;
}

static ssize_t ds_session_seed_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	mutex_lock(&ds->cfg_lock);
	memcpy(tmp, ds->session_seed, 32);
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%32ph\n", tmp);
}

static ssize_t ds_session_seed_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	if (sscanf(buf,
		   "%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx",
		   &tmp[0], &tmp[1], &tmp[2], &tmp[3],
		   &tmp[4], &tmp[5], &tmp[6], &tmp[7],
		   &tmp[8], &tmp[9], &tmp[10], &tmp[11],
		   &tmp[12], &tmp[13], &tmp[14], &tmp[15],
		   &tmp[16], &tmp[17], &tmp[18], &tmp[19],
		   &tmp[20], &tmp[21], &tmp[22], &tmp[23],
		   &tmp[24], &tmp[25], &tmp[26], &tmp[27],
		   &tmp[28], &tmp[29], &tmp[30], &tmp[31])
	    != 32)
		return -EINVAL;

	mutex_lock(&ds->cfg_lock);
	memcpy(ds->session_seed, tmp, 32);
	mutex_unlock(&ds->cfg_lock);

	return count;
}

static ssize_t ds_challenge_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	mutex_lock(&ds->cfg_lock);
	memcpy(tmp, ds->challenge, 32);
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%32ph\n", tmp);
}

static ssize_t ds_challenge_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	if (sscanf(buf,
		   "%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx",
		   &tmp[0], &tmp[1], &tmp[2], &tmp[3],
		   &tmp[4], &tmp[5], &tmp[6], &tmp[7],
		   &tmp[8], &tmp[9], &tmp[10], &tmp[11],
		   &tmp[12], &tmp[13], &tmp[14], &tmp[15],
		   &tmp[16], &tmp[17], &tmp[18], &tmp[19],
		   &tmp[20], &tmp[21], &tmp[22], &tmp[23],
		   &tmp[24], &tmp[25], &tmp[26], &tmp[27],
		   &tmp[28], &tmp[29], &tmp[30], &tmp[31])
	    != 32)
		return -EINVAL;

	mutex_lock(&ds->cfg_lock);
	memcpy(ds->challenge, tmp, 32);
	mutex_unlock(&ds->cfg_lock);

	return count;
}

static ssize_t ds_s_secret_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	mutex_lock(&ds->cfg_lock);
	memcpy(tmp, ds->s_secret, 32);
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%32ph\n", tmp);
}

static ssize_t ds_s_secret_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 tmp[32];

	if (sscanf(buf,
		   "%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx,%2hhx",
		   &tmp[0], &tmp[1], &tmp[2], &tmp[3],
		   &tmp[4], &tmp[5], &tmp[6], &tmp[7],
		   &tmp[8], &tmp[9], &tmp[10], &tmp[11],
		   &tmp[12], &tmp[13], &tmp[14], &tmp[15],
		   &tmp[16], &tmp[17], &tmp[18], &tmp[19],
		   &tmp[20], &tmp[21], &tmp[22], &tmp[23],
		   &tmp[24], &tmp[25], &tmp[26], &tmp[27],
		   &tmp[28], &tmp[29], &tmp[30], &tmp[31])
	    != 32)
		return -EINVAL;

	mutex_lock(&ds->cfg_lock);
	memcpy(ds->s_secret, tmp, 32);
	mutex_unlock(&ds->cfg_lock);

	return count;
}

static ssize_t ds_auth_anon_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val;

	mutex_lock(&ds->cfg_lock);
	val = ds->auth_anon;
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ds_auth_anon_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&ds->cfg_lock);
	ds->auth_anon = !!val;
	mutex_unlock(&ds->cfg_lock);

	return count;
}

static ssize_t ds_auth_bdconst_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val;

	mutex_lock(&ds->cfg_lock);
	val = ds->auth_bdconst;
	mutex_unlock(&ds->cfg_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ds_auth_bdconst_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&ds->cfg_lock);
	ds->auth_bdconst = !!val;
	mutex_unlock(&ds->cfg_lock);

	return count;
}

static ssize_t ds_readstatus_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ds28e16_data *ds = dev_get_drvdata(dev);
	u8 status[16] = {};
	u32 tries;
	int i, count = 0;

	mutex_lock(&ds->cfg_lock);
	tries = ds->attr_trytimes;
	mutex_unlock(&ds->cfg_lock);

	for (i = 0; i < tries; i++) {
		if (ds28e16_get_page_status_retry(ds, status) == DS_TRUE)
			count++;
		usleep_range(1000, 1200);
	}

	return scnprintf(buf, PAGE_SIZE,
			 "Success=%d status=%16ph\n", count, status);
}

static DEVICE_ATTR_RO(ds_readstatus);
static DEVICE_ATTR_RO(ds_romid);
static DEVICE_ATTR(ds_pagenumber, 0660, ds_pagenumber_show, ds_pagenumber_store);
static DEVICE_ATTR(ds_pagedata, 0660, ds_pagedata_show, ds_pagedata_store);
static DEVICE_ATTR(ds_time, 0660, ds_time_show, ds_time_store);
static DEVICE_ATTR(ds_session_seed, 0600, ds_session_seed_show, ds_session_seed_store);
static DEVICE_ATTR(ds_challenge, 0600, ds_challenge_show, ds_challenge_store);
static DEVICE_ATTR(ds_s_secret, 0600, ds_s_secret_show, ds_s_secret_store);
static DEVICE_ATTR(ds_auth_anon, 0660, ds_auth_anon_show, ds_auth_anon_store);
static DEVICE_ATTR(ds_auth_bdconst, 0660, ds_auth_bdconst_show, ds_auth_bdconst_store);
static DEVICE_ATTR_RO(ds_auth_result);

static struct attribute *ds_attrs[] = {
	&dev_attr_ds_readstatus.attr,
	&dev_attr_ds_romid.attr,
	&dev_attr_ds_pagenumber.attr,
	&dev_attr_ds_pagedata.attr,
	&dev_attr_ds_time.attr,
	&dev_attr_ds_session_seed.attr,
	&dev_attr_ds_challenge.attr,
	&dev_attr_ds_s_secret.attr,
	&dev_attr_ds_auth_anon.attr,
	&dev_attr_ds_auth_bdconst.attr,
	&dev_attr_ds_auth_result.attr,
	NULL,
};

static const struct attribute_group ds_attr_group = {
	.attrs = ds_attrs,
};

#define AUTHENTIC_PERIOD_MS	5000
#define AUTHENTIC_COUNT_MAX	5

static void authentic_work(struct work_struct *work)
{
	struct ds28e16_data *ds = container_of(work, struct ds28e16_data,
					       authentic_work.work);
	int i, result = 0;

	for (i = 0; i < 3; i++) {
		result = ds28e16_authenticate(ds);
		if (result == DS_TRUE)
			break;
		usleep_range(100, 200);
	}

	if (result == DS_TRUE) {
		WRITE_ONCE(ds->batt_verified, true);
		ds->auth_retry_count = 0;
		power_supply_changed(ds->verify_psy);
		pr_info("battery verified\n");
	} else {
		ds->auth_retry_count++;
		if (ds->auth_retry_count < AUTHENTIC_COUNT_MAX) {
			schedule_delayed_work(&ds->authentic_work,
					      msecs_to_jiffies(AUTHENTIC_PERIOD_MS));
			pr_info("verification retry %d/%d\n",
				ds->auth_retry_count, AUTHENTIC_COUNT_MAX);
		} else {
			WRITE_ONCE(ds->batt_verified, false);
			ds->auth_retry_count = 0;
			power_supply_changed(ds->verify_psy);
			pr_err("battery verification failed after %d attempts: %d\n",
			       AUTHENTIC_COUNT_MAX, result);
		}
	}
}

/**
 * ds28e16_init_defaults - populate the per-instance auth parameters with
 * the platform-default values.
 *
 * These values must match what is provisioned into the chip's Secret Page
 * at manufacturing time. They can be overridden via sysfs if needed.
 *
 * Called before any sysfs or workqueue context can access the fields,
 * so no locking is needed here.
 */
static void ds28e16_init_defaults(struct ds28e16_data *ds)
{
	static const u8 default_session_seed[32] = {
		0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
		0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
		0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
		0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
	};
	static const u8 default_s_secret[32] = {
		0x0C, 0x99, 0x2B, 0xD3, 0x95, 0xDB, 0xA0, 0xB4,
		0xEF, 0x07, 0xB3, 0xD8, 0x75, 0xF3, 0xC7, 0xAE,
		0xDA, 0xC4, 0x41, 0x2F, 0x48, 0x93, 0xB5, 0xD9,
		0xE1, 0xE5, 0x4B, 0x20, 0x9B, 0xF3, 0x77, 0x39,
	};

	memcpy(ds->session_seed, default_session_seed, 32);
	memcpy(ds->s_secret, default_s_secret, 32);
	memset(ds->challenge, 0x00, 32);

	ds->auth_anon = true;
	ds->auth_bdconst = true;
	ds->pagenumber = 1;
	ds->attr_trytimes = 1;
	ds->mi_auth_result = 0;
}

static int ds28e16_parse_dt(struct device *dev, struct ds28e16_data *ds)
{
	struct device_node *np = dev->of_node;
	const char *label;
	int ret, val;

	ret = of_property_read_string(np, "xiaomi,onewire-bus", &label);
	if (ret) {
		pr_err("missing 'xiaomi,onewire-bus' property: %d\n", ret);
		return ret;
	}
	strscpy(ds->ow_bus_label, label, sizeof(ds->ow_bus_label));
	pr_debug("onewire bus: '%s'\n", ds->ow_bus_label);

	ds->version = 0;
	if (!of_property_read_u32(np, "maxim,version", &val))
		ds->version = val;

	return 0;
}

static int ds28e16_probe(struct platform_device *pdev)
{
	struct ds28e16_data *ds;
	struct onewire_bus *ow_bus;
	int ret;

	pr_info("entry\n");

	if (strcmp(pdev->name, "soc:maxim_ds28e16") != 0)
		return -ENODEV;

	if (!pdev->dev.of_node || !of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	ds = devm_kzalloc(&pdev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	ret = ds28e16_parse_dt(&pdev->dev, ds);
	if (ret) {
		pr_err("parse_dt failed: %d\n", ret);
		return ret;
	}

	ow_bus = onewire_bus_get(ds->ow_bus_label);
	if (!ow_bus) {
		pr_info("onewire bus '%s' not ready, deferring\n",
			ds->ow_bus_label);
		return -EPROBE_DEFER;
	}

	ds->dev = &pdev->dev;
	ds->pdev = pdev;
	ds->ow_bus = ow_bus;
	platform_set_drvdata(pdev, ds);

	mutex_init(&ds->cmd_lock);
	mutex_init(&ds->cfg_lock);
	ds28e16_init_defaults(ds);

	INIT_DELAYED_WORK(&ds->authentic_work, authentic_work);

	ret = verify_psy_register(ds);
	if (ret) {
		pr_err("verify_psy_register failed: %d\n", ret);
		goto err_out;
	}

	ret = sysfs_create_group(&ds->dev->kobj, &ds_attr_group);
	if (ret) {
		pr_err("sysfs_create_group failed: %d\n", ret);
		goto err_out;
	}

	schedule_delayed_work(&ds->authentic_work, msecs_to_jiffies(500));

	pr_info("success\n");
	return 0;

err_out:
	mutex_destroy(&ds->cfg_lock);
	mutex_destroy(&ds->cmd_lock);
	platform_set_drvdata(pdev, NULL);
	onewire_bus_put(ow_bus);
	pr_err("probe failed: %d\n", ret);
	return ret;
}

static int ds28e16_remove(struct platform_device *pdev)
{
	struct ds28e16_data *ds = platform_get_drvdata(pdev);

	if (!ds)
		return 0;

	cancel_delayed_work_sync(&ds->authentic_work);
	sysfs_remove_group(&ds->dev->kobj, &ds_attr_group);
	mutex_destroy(&ds->cfg_lock);
	mutex_destroy(&ds->cmd_lock);
	onewire_bus_put(ds->ow_bus);
	ds->ow_bus = NULL;

	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id ds28e16_dt_match[] = {
	{ .compatible = "maxim,ds28e16", },
	{ },
};
MODULE_DEVICE_TABLE(of, ds28e16_dt_match);

static struct platform_driver ds28e16_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "maxim_ds28e16",
		.of_match_table	= of_match_ptr(ds28e16_dt_match),
	},
	.probe	= ds28e16_probe,
	.remove	= ds28e16_remove,
};

static int __init ds28e16_init(void)
{
	return platform_driver_register(&ds28e16_driver);
}

static void __exit ds28e16_exit(void)
{
	platform_driver_unregister(&ds28e16_driver);
}

subsys_initcall_sync(ds28e16_init);
module_exit(ds28e16_exit);

MODULE_AUTHOR("Xiaomi Inc.");
MODULE_DESCRIPTION("DS28E16 DeepCover Secure Authenticator driver");
MODULE_LICENSE("GPL");
