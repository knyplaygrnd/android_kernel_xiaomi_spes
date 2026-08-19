// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "inc/rt-regmap.h"

#define RT_REGMAP_VERSION	"1.2.3_G"

#define ERR_MSG_SIZE		128
#define MAX_BYTE_SIZE		32

struct rt_regmap_ops {
	int (*regmap_block_write)(struct rt_regmap_device *rd, u32 reg,
				  int bytes, const void *src);
	int (*regmap_block_read)(struct rt_regmap_device *rd, u32 reg,
				 int bytes, void *dst);
};

enum {
	RT_DBG_REG_ADDR,
	RT_DBG_DATA,
	RT_DBG_REGS,
	RT_DBG_SYNC,
	RT_DBG_ERROR,
	RT_DBG_NAME,
	RT_DBG_BLOCK,
	RT_DBG_SIZE,
	RT_DBG_DEVICE_ADDR,
	RT_DBG_SUPPORT_MODE,
	RT_DBG_IO_LOG,
	RT_DBG_CACHE_MODE,
	RT_DBG_REG_SIZE,
	RT_DBG_MAX,
};

struct reg_index_offset {
	int index;
	int offset;
};

#ifdef CONFIG_DEBUG_FS
struct rt_debug_data {
	struct reg_index_offset rio;
	unsigned int reg_addr;
	unsigned int reg_size;
};

struct rt_debug_st {
	void *info;
	int id;
};
#endif /* CONFIG_DEBUG_FS */

/* rt_regmap_device
 *
 * Richtek regmap device. One for each rt_regmap.
 *
 */
struct rt_regmap_device {
	struct rt_regmap_properties props;
	struct rt_regmap_fops *rops;
	struct rt_regmap_ops regmap_ops;
	struct device dev;
	void *client;
	struct mutex lock;
	struct delayed_work rt_work;
	int dev_addr;
	unsigned char *alloc_data;
	unsigned char **cache_data;
	unsigned long *cached;
	unsigned long *cache_dirty;
	char *err_msg;
	unsigned char error_occurred:1;

	int (*rt_block_write[4])(struct rt_regmap_device *rd,
				 const struct rt_register *rm, int size,
				 const unsigned char *wdata, int count,
				 int cache_idx, int cache_offset);
#ifdef CONFIG_DEBUG_FS
	struct dentry *rt_den;
	struct dentry *rt_debug_file[RT_DBG_MAX];
	struct rt_debug_st rtdbg_st[RT_DBG_MAX];
	struct dentry **rt_reg_file;
	struct rt_debug_st **reg_st;
	struct rt_debug_data dbg_data;
#endif /* CONFIG_DEBUG_FS */
};

static struct reg_index_offset find_register_index(const struct rt_regmap_device *rd, u32 reg)
{
	int i = 0, j = 0, unit = RT_1BYTE_MODE;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t *rm = rd->props.rm;

	for (i = 0; i < rd->props.register_num; i++) {
		if (reg == rm[i]->addr) {
			rio.index = i;
			rio.offset = 0;
			break;
		}

		if (reg > rm[i]->addr && (reg - rm[i]->addr) < rm[i]->size) {
			rio.index = i;
			for (j = 0; rd->props.group[j].mode != RT_DUMMY_MODE; j++) {
				if (reg >= rd->props.group[j].start &&
				    reg <= rd->props.group[j].end) {
					unit = rd->props.group[j].mode;
					break;
				}
			}
			rio.offset = (reg - rm[i]->addr) * unit;
		}
	}
	return rio;
}

static int rt_chip_block_write(struct rt_regmap_device *rd, u32 reg,
			       int bytes, const void *src);

/* rt_regmap_cache_sync - sync all cache data to chip */
void rt_regmap_cache_sync(struct rt_regmap_device *rd)
{
	int ret = 0, i = 0, j = 0;
	const rt_register_map_t rm = NULL;

	mutex_lock(&rd->lock);
	for (i = 0; i < rd->props.register_num; i++) {
		if (!rd->cache_dirty[i])
			continue;
		rm = rd->props.rm[i];
		for (j = 0; j < rm->size; j++) {
			if (!test_bit(j, &rd->cache_dirty[i]))
				continue;
			ret = rt_chip_block_write(rd, rm->addr + j, 1,
						  rd->cache_data[i] + j);
			if (ret < 0) {
				dev_notice(&rd->dev,
					   "%s: block write fail(%d) @ 0x%02x\n",
					   __func__, ret, rm->addr + j);
				goto err_cache_sync;
			}
			clear_bit(j, &rd->cache_dirty[i]);
		}
		rd->cache_dirty[i] = 0;
	}
	dev_info(&rd->dev, "%s: successfully\n", __func__);
err_cache_sync:
	mutex_unlock(&rd->lock);
}
EXPORT_SYMBOL(rt_regmap_cache_sync);

/* rt_regmap_cache_write_back - write current cache data to chip
 * @rd: rt_regmap_device pointer.
 * @reg: register address
 */
void rt_regmap_cache_write_back(struct rt_regmap_device *rd, u32 reg)
{
	int ret = 0, j = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t rm = NULL;

	rio = find_register_index(rd, reg);
	if (rio.index < 0 || rio.offset != 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of map\n",
			   __func__, reg);
		return;
	}

	mutex_lock(&rd->lock);
	if (!rd->cache_dirty[rio.index])
		goto out;

	rm = rd->props.rm[rio.index];
	for (j = 0; j < rm->size; j++) {
		if (!test_bit(j, &rd->cache_dirty[rio.index]))
			continue;

		ret = rt_chip_block_write(rd, rm->addr + j, 1,
					  rd->cache_data[rio.index] + j);
		if (ret < 0) {
			dev_notice(&rd->dev,
				   "%s: block write fail(%d) @ 0x%02x\n",
				   __func__, ret, rm->addr + j);
			goto err_cache_write_back;
		}
		clear_bit(j, &rd->cache_dirty[rio.index]);
	}
out:
	dev_info(&rd->dev, "%s: successfully\n", __func__);
err_cache_write_back:
	mutex_unlock(&rd->lock);
}
EXPORT_SYMBOL(rt_regmap_cache_write_back);

/* rt_is_reg_volatile - check register map is volatile or not
 * @rd: rt_regmap_device pointer.
 * @reg: register address.
 */
int rt_is_reg_volatile(struct rt_regmap_device *rd, u32 reg)
{
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t rm = NULL;

	rio = find_register_index(rd, reg);
	if (rio.index < 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of range\n",
			   __func__, reg);
		return -EINVAL;
	}
	rm = rd->props.rm[rio.index];

	return (rm->reg_type & RT_REG_TYPE_MASK) == RT_VOLATILE ? 1 : 0;
}
EXPORT_SYMBOL(rt_is_reg_volatile);

/* rt_get_regsize - get register map size for specific register
 * @rd: rt_regmap_device pointer.
 * @reg: register address
 */
int rt_get_regsize(struct rt_regmap_device *rd, u32 reg)
{
	struct reg_index_offset rio = {-1, -1};

	rio = find_register_index(rd, reg);
	if (rio.index < 0 || rio.offset != 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of map\n",
			   __func__, reg);
		return -EINVAL;
	}
	return rd->props.rm[rio.index]->size;
}
EXPORT_SYMBOL(rt_get_regsize);

static void rt_work_func(struct work_struct *work)
{
	struct rt_regmap_device *rd =
		container_of(work, struct rt_regmap_device, rt_work.work);

	rt_regmap_cache_sync(rd);
}

static int rt_chip_block_write(struct rt_regmap_device *rd, u32 reg,
			       int bytes, const void *src)
{
	if ((rd->props.rt_regmap_mode & RT_IO_BLK_MODE_MASK) == RT_IO_BLK_ALL ||
	    (rd->props.rt_regmap_mode & RT_IO_BLK_MODE_MASK) == RT_IO_BLK_CHIP)
		return -EPERM;

	return rd->rops->write_device(rd->client, reg, bytes, src);
}

static int rt_chip_block_read(struct rt_regmap_device *rd, u32 reg,
			      int bytes, void *dst)
{
	return rd->rops->read_device(rd->client, reg, bytes, dst);
}

static int rt_block_write(struct rt_regmap_device *rd,
			  const struct rt_register *rm, int size,
			  const unsigned char *wdata, int count,
			  int cache_idx, int cache_offset)
{
	int ret = 0, j = 0, change = 0;

	for (j = cache_offset; j < cache_offset + size; j++, count++) {
		ret = test_and_set_bit(j, &rd->cached[cache_idx]);
		if (ret && (rm->reg_type & RT_WR_ONCE)) {
			if (rd->cache_data[cache_idx][j] ==
					(wdata[count] & rm->wbit_mask[j]))
				continue;
		}
		rd->cache_data[cache_idx][j] = wdata[count] & rm->wbit_mask[j];
		change++;
	}

	if (!change)
		goto out;

	ret = rt_chip_block_write(rd, rm->addr + cache_offset, size,
				  rd->cache_data[cache_idx] + cache_offset);
	if (ret < 0)
		dev_notice(&rd->dev, "%s: block write fail(%d) @ 0x%02x\n",
			   __func__, ret, rm->addr + cache_offset);
out:
	return ret < 0 ? ret : 0;
}

static int rt_block_write_blk_all(struct rt_regmap_device *rd,
				  const struct rt_register *rm, int size,
				  const unsigned char *wdata, int count,
				  int cache_idx, int cache_offset)
{
	dev_dbg(&rd->dev, "%s: write blocked (BLK_ALL) @ 0x%02x\n",
		__func__, rm->addr + cache_offset);
	return -EPERM;
}

static int rt_block_write_blk_cache(struct rt_regmap_device *rd,
				    const struct rt_register *rm, int size,
				    const unsigned char *wdata, int count,
				    int cache_idx, int cache_offset)
{
	int ret = 0;

	ret = rt_chip_block_write(rd, rm->addr + cache_offset, size,
				  &wdata[count]);
	if (ret < 0)
		dev_notice(&rd->dev, "%s: block write fail(%d) @ 0x%02x\n",
			   __func__, ret, rm->addr + cache_offset);
	return ret < 0 ? ret : 0;
}

static int rt_block_write_blk_chip(struct rt_regmap_device *rd,
				   const struct rt_register *rm, int size,
				   const unsigned char *wdata, int count,
				   int cache_idx, int cache_offset)
{
	int ret = 0, j = 0;

	for (j = cache_offset; j < cache_offset + size; j++, count++) {
		ret = test_and_set_bit(j, &rd->cached[cache_idx]);
		if (ret && (rm->reg_type & RT_WR_ONCE)) {
			if (rd->cache_data[cache_idx][j] ==
					(wdata[count] & rm->wbit_mask[j]))
				continue;
		}
		rd->cache_data[cache_idx][j] = wdata[count] & rm->wbit_mask[j];
		set_bit(j, &rd->cache_dirty[cache_idx]);
	}
	return 0;
}

static int (*rt_block_map[])(struct rt_regmap_device *rd,
		const struct rt_register *rm, int size,
		const unsigned char *wdata, int count,
		int cache_idx, int cache_offset) = {
			&rt_block_write,
			&rt_block_write_blk_all,
			&rt_block_write_blk_cache,
			&rt_block_write_blk_chip,
		};

static int _rt_cache_block_write(struct rt_regmap_device *rd, u32 reg,
				 int bytes, const void *src, bool asyn)
{
	int ret = 0, i = 0, j = 0, count = 0, size = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t rm = NULL;
	const unsigned char *wdata = src;
	unsigned char wri_data[128];
	unsigned char blk_index = 0;
	int log_pos = 0, log_ret = 0;

	rio = find_register_index(rd, reg);
	if (rio.index < 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of range\n",
			   __func__, reg);
		return -EINVAL;
	}

	for (i = rio.index, j = rio.offset, count = 0;
	     i < rd->props.register_num && count < bytes;
	     i++, j = 0, count += size) {
		rm = rd->props.rm[i];
		size = (bytes - count) <= (rm->size - j) ?
			(bytes - count) : (rm->size - j);

		if ((rm->reg_type & RT_REG_TYPE_MASK) == RT_VOLATILE) {
			ret = rt_chip_block_write(rd, rm->addr + j, size,
						  &wdata[count]);
		} else if (asyn) {
			ret = rd->props.rt_regmap_mode & RT_IO_BLK_MODE_MASK;
			if (ret == RT_IO_BLK_ALL || ret == RT_IO_BLK_CACHE) {
				dev_notice(&rd->dev, "%s: ret = %d\n",
					   __func__, ret);
				ret = -EPERM;
				goto err_cache_block_write;
			}

			ret = rt_block_write_blk_chip(rd, rm, size, wdata,
						      count, i, j);
		} else {
			blk_index = (rd->props.rt_regmap_mode &
					RT_IO_BLK_MODE_MASK) >> 3;
			ret = rd->rt_block_write[blk_index](rd, rm, size,
					wdata, count, i, j);
		}

		if (ret < 0) {
			dev_notice(&rd->dev,
				   "%s: block write fail(%d) @ 0x%02x\n",
				__func__, ret, rm->addr + j);
			goto err_cache_block_write;
		}
	}

	if (rd->props.io_log_en) {
		for (i = 0; i < count; i++) {
			log_ret = snprintf(wri_data + log_pos,
					   sizeof(wri_data) - log_pos,
					   "%02x,", wdata[i]);
			if (log_ret < 0)
				break;
			if (log_ret >= sizeof(wri_data) - log_pos) {
				snprintf(wri_data + log_pos,
					 sizeof(wri_data) - log_pos, "...");
				break;
			}
			log_pos += log_ret;
		}
		dev_info(&rd->dev,
			 "%s: RT_REGMAP [WRITE] reg0x%02x [Data] %s\n",
			 __func__, reg, wri_data);
	}
	return 0;
err_cache_block_write:
	return ret;
}

static int rt_cache_block_write(struct rt_regmap_device *rd, u32 reg,
				int bytes, const void *src)
{
	return _rt_cache_block_write(rd, reg, bytes, src, false);
}

static int rt_asyn_cache_block_write(struct rt_regmap_device *rd, u32 reg,
				     int bytes, const void *src)
{
	int ret = 0;

	ret = _rt_cache_block_write(rd, reg, bytes, src, true);
	if (ret >= 0)
		mod_delayed_work(system_wq, &rd->rt_work, msecs_to_jiffies(1));
	return ret;
}

static int rt_cache_block_read(struct rt_regmap_device *rd, u32 reg,
			       int bytes, void *dest)
{
	int ret = 0, i = 0, j = 0, count = 0, total_bytes = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t rm = NULL;
	unsigned long mask = 0;
	unsigned char local_val[MAX_BYTE_SIZE];

	rio = find_register_index(rd, reg);
	if (rio.index < 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of range\n",
			   __func__, reg);
		return -EINVAL;
	}

	rm = rd->props.rm[rio.index];
	total_bytes += (rm->size - rio.offset);

	for (i = rio.index + 1; i < rd->props.register_num; i++)
		total_bytes += rd->props.rm[i]->size;

	if (bytes > total_bytes) {
		dev_notice(&rd->dev, "%s: bytes %d is out of range\n",
			   __func__, bytes);
		return -EINVAL;
	}

	for (i = rio.index, j = rio.offset, count = 0;
	     i < rd->props.register_num && count < bytes;
	     i++, j = 0) {
		rm = rd->props.rm[i];
		mask = GENMASK(rm->size - 1, j);
		if ((rd->cached[i] & mask) == mask) {
			count += rm->size - j;
			continue;
		}

		ret = rd->rops->read_device(rd->client, rm->addr, rm->size,
				local_val);
		if (ret < 0) {
			dev_notice(&rd->dev,
				   "%s: read device fail(%d) @ 0x%02x\n",
				   __func__, ret, rm->addr);
			return ret;
		}

		for (; j < rm->size && count < bytes; j++, count++) {
			if (test_bit(j, &rd->cached[i]))
				continue;

			rd->cache_data[i][j] = local_val[j];
			if ((rm->reg_type & RT_REG_TYPE_MASK) != RT_VOLATILE)
				set_bit(j, &rd->cached[i]);
		}
	}

	if (rd->props.io_log_en)
		dev_info(&rd->dev, "%s: RT_REGMAP [READ] reg0x%02x\n",
			 __func__, reg);

	memcpy(dest, &rd->cache_data[rio.index][rio.offset], bytes);

	return 0;
}

static u32 cpu_to_chip(struct rt_regmap_device *rd, u32 cpu_data, int size)
{
	u32 chip_data = 0;

	if (rd->props.rt_format == RT_BIG_ENDIAN) {
#ifdef CONFIG_CPU_BIG_ENDIAN
		chip_data = cpu_data << (4 - size) * 8;
#else
		chip_data = cpu_to_be32(cpu_data);
		chip_data >>= (4 - size) * 8;
#endif /* CONFIG_CPU_BIG_ENDIAN */
	} else {
		chip_data = cpu_to_le32(cpu_data);
	}

	return chip_data;
}

static u32 chip_to_cpu(struct rt_regmap_device *rd, u32 chip_data, int size)
{
	u32 cpu_data = 0;

	if (rd->props.rt_format == RT_BIG_ENDIAN) {
#ifdef CONFIG_CPU_BIG_ENDIAN
		cpu_data = chip_data >> ((4 - size) * 8);
#else
		cpu_data = be32_to_cpu(chip_data);
		cpu_data >>= (4 - size) * 8;
#endif /* CONFIG_CPU_BIG_ENDIAN */
	} else {
		cpu_data = le32_to_cpu(chip_data);
	}

	return cpu_data;
}

/* _rt_regmap_reg_write - write data to specific register map
 * only support 1, 2, 4 bytes register map
 * @rd: rt_regmap_device pointer.
 * @rrd: rt_reg_data pointer.
 */
static int _rt_regmap_reg_write(struct rt_regmap_device *rd,
				struct rt_reg_data *rrd, bool asyn)
{
	int ret = -ENOTSUPP, size = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t *rm = rd->props.rm;
	u32 tmp_data = 0;

	rio = find_register_index(rd, rrd->reg);
	if (rio.index < 0 || rio.offset != 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of map\n",
			   __func__, rrd->reg);
		return -EINVAL;
	}

	size = rm[rio.index]->size;
	if (size < 1 || size > 4) {
		dev_notice(&rd->dev, "%s: only support 1~4 bytes(%d)\n",
			   __func__, size);
		return -EINVAL;
	}

	tmp_data = cpu_to_chip(rd, rrd->rt_data.data_u32, size);
	mutex_lock(&rd->lock);
	ret = (asyn ? rt_asyn_cache_block_write :
		rd->regmap_ops.regmap_block_write)(rd, rrd->reg, size,
						   &tmp_data);
	mutex_unlock(&rd->lock);
	if (ret < 0)
		dev_notice(&rd->dev, "%s: block write fail(%d) @ 0x%02x\n",
			   __func__, ret, rrd->reg);

	return (ret < 0) ? ret : 0;
}

int rt_regmap_reg_write(struct rt_regmap_device *rd, struct rt_reg_data *rrd,
			u32 reg, const u32 data)
{
	rrd->reg = reg;
	rrd->rt_data.data_u32 = data;
	return _rt_regmap_reg_write(rd, rrd, false);
}
EXPORT_SYMBOL(rt_regmap_reg_write);

int rt_asyn_regmap_reg_write(struct rt_regmap_device *rd,
			     struct rt_reg_data *rrd, u32 reg, const u32 data)
{
	rrd->reg = reg;
	rrd->rt_data.data_u32 = data;
	return _rt_regmap_reg_write(rd, rrd, true);
}
EXPORT_SYMBOL(rt_asyn_regmap_reg_write);

/* _rt_regmap_reg_read - register read for specific register map
 * only support 1, 2, 4 bytes register map.
 * @rd: rt_regmap_device pointer.
 * @rrd: rt_reg_data pointer.
 */
static int _rt_regmap_reg_read(struct rt_regmap_device *rd,
			       struct rt_reg_data *rrd)
{
	int ret = -ENOTSUPP, size = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t *rm = rd->props.rm;
	u32 data = 0;

	rio = find_register_index(rd, rrd->reg);
	if (rio.index < 0 || rio.offset != 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of map\n",
			   __func__, rrd->reg);
		return -EINVAL;
	}

	size = rm[rio.index]->size;
	if (size < 1 || size > 4) {
		dev_notice(&rd->dev, "%s: only support 1~4 bytes(%d)\n",
			   __func__, size);
		return -EINVAL;
	}

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_read(rd, rrd->reg, size, &data);
	mutex_unlock(&rd->lock);
	if (ret < 0) {
		dev_notice(&rd->dev, "%s: block read fail(%d) @ 0x%02x\n",
			   __func__, ret, rrd->reg);
		goto out;
	}
	rrd->rt_data.data_u32 = chip_to_cpu(rd, data, size);
out:
	return (ret < 0) ? ret : 0;
}

int rt_regmap_reg_read(struct rt_regmap_device *rd,
		       struct rt_reg_data *rrd, u32 reg)
{
	rrd->reg = reg;
	return _rt_regmap_reg_read(rd, rrd);
}
EXPORT_SYMBOL(rt_regmap_reg_read);

/* _rt_regmap_update_bits - assign bits specific register map */
static int _rt_regmap_update_bits(struct rt_regmap_device *rd,
				  struct rt_reg_data *rrd)
{
	int ret = -ENOTSUPP, size = 0;
	struct reg_index_offset rio = {-1, -1};
	const rt_register_map_t *rm = rd->props.rm;
	u32 new = 0, old = 0;
	bool change = false;

	rio = find_register_index(rd, rrd->reg);
	if (rio.index < 0 || rio.offset != 0) {
		dev_notice(&rd->dev, "%s: reg 0x%02x is out of map\n",
			   __func__, rrd->reg);
		return -EINVAL;
	}

	size = rm[rio.index]->size;
	if (size < 1 || size > 4) {
		dev_notice(&rd->dev, "%s: only support 1~4 bytes(%d)\n",
			   __func__, size);
		return -EINVAL;
	}

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_read(rd, rrd->reg, size, &old);
	if (ret < 0) {
		dev_notice(&rd->dev, "%s: block read fail(%d) @ 0x%02x\n",
			   __func__, ret, rrd->reg);
		goto out;
	}

	old = chip_to_cpu(rd, old, size);
	new = (old & ~(rrd->mask)) | (rrd->rt_data.data_u32 & rrd->mask);
	change = old != new;
	if ((rm[rio.index]->reg_type & RT_WR_ONCE) && !change)
		goto out;

	new = cpu_to_chip(rd, new, size);
	ret = rd->regmap_ops.regmap_block_write(rd, rrd->reg, size, &new);
	if (ret < 0)
		dev_notice(&rd->dev, "%s: block write fail(%d) @ 0x%02x\n",
			   __func__, ret, rrd->reg);
out:
	mutex_unlock(&rd->lock);
	return (ret < 0) ? ret : 0;
}

int rt_regmap_update_bits(struct rt_regmap_device *rd, struct rt_reg_data *rrd,
			  u32 reg, u32 mask, u32 data)
{
	rrd->reg = reg;
	rrd->mask = mask;
	rrd->rt_data.data_u32 = data;
	return _rt_regmap_update_bits(rd, rrd);
}
EXPORT_SYMBOL(rt_regmap_update_bits);

static int rt_regmap_check_bytes(struct rt_regmap_device *rd, int bytes)
{
	if (bytes <= 0) {
		dev_notice(&rd->dev, "%s: invalid bytes %d\n",
			   __func__, bytes);
		return -EINVAL;
	}
	if (bytes > MAX_BYTE_SIZE) {
		dev_notice(&rd->dev, "%s: bytes %d exceeds max %d\n",
			   __func__, bytes, MAX_BYTE_SIZE);
		return -EINVAL;
	}
	return 0;
}

/* rt_regmap_block_write - block write data to register
 * @rd: rt_regmap_device pointer
 * @reg: register address
 * @bytes: length for write
 * @src: source of write data
 */
int rt_regmap_block_write(struct rt_regmap_device *rd, u32 reg,
			  int bytes, const void *src)
{
	int ret = 0;

	ret = rt_regmap_check_bytes(rd, bytes);
	if (ret < 0)
		return ret;

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_write(rd, reg, bytes, src);
	mutex_unlock(&rd->lock);
	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(rt_regmap_block_write);

/* rt_asyn_regmap_block_write - asyn block write */
int rt_asyn_regmap_block_write(struct rt_regmap_device *rd, u32 reg,
			       int bytes, const void *src)
{
	int ret = 0;

	ret = rt_regmap_check_bytes(rd, bytes);
	if (ret < 0)
		return ret;

	mutex_lock(&rd->lock);
	ret = rt_asyn_cache_block_write(rd, reg, bytes, src);
	mutex_unlock(&rd->lock);
	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(rt_asyn_regmap_block_write);

/* rt_regmap_block_read - block read data from register
 * @rd: rt_regmap_device pointer
 * @reg: register address
 * @bytes: length for read
 * @dst: destination for read data
 */
int rt_regmap_block_read(struct rt_regmap_device *rd, u32 reg,
			 int bytes, void *dst)
{
	int ret = 0;

	ret = rt_regmap_check_bytes(rd, bytes);
	if (ret < 0)
		return ret;

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_read(rd, reg, bytes, dst);
	mutex_unlock(&rd->lock);
	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(rt_regmap_block_read);

void rt_cache_getlasterror(struct rt_regmap_device *rd, char *buf, size_t size)
{
	int ret = 0;

	mutex_lock(&rd->lock);
	ret = snprintf(buf, size, "%s", rd->err_msg);
	mutex_unlock(&rd->lock);
	if (ret < 0 || ret >= size)
		dev_notice(&rd->dev, "%s: snprintf fail(%d)\n", __func__, ret);
}
EXPORT_SYMBOL(rt_cache_getlasterror);

void rt_cache_clrlasterror(struct rt_regmap_device *rd)
{
	mutex_lock(&rd->lock);
	rd->error_occurred = 0;
	rd->err_msg[0] = '\0';
	mutex_unlock(&rd->lock);
}
EXPORT_SYMBOL(rt_cache_clrlasterror);

/* initialize cache data from rt_register */
static int rt_regmap_cache_init(struct rt_regmap_device *rd)
{
	int ret = 0, i = 0, j = 0, count = 0, bytes_num = 0;
	const rt_register_map_t *rm = rd->props.rm;

	rd->cache_data = devm_kzalloc(&rd->dev,
				      rd->props.register_num * sizeof(*rd->cache_data),
				      GFP_KERNEL);
	rd->cached = devm_kzalloc(&rd->dev,
				  rd->props.register_num * sizeof(*rd->cached),
				  GFP_KERNEL);
	rd->cache_dirty = devm_kzalloc(&rd->dev,
				       rd->props.register_num * sizeof(*rd->cache_dirty),
				       GFP_KERNEL);

	if (!rd->cache_data || !rd->cached || !rd->cache_dirty)
		return -ENOMEM;

	if (!rd->props.group) {
		rd->props.group = devm_kzalloc(&rd->dev,
					       sizeof(*rd->props.group) * 2, GFP_KERNEL);
		if (!rd->props.group)
			return -ENOMEM;
		rd->props.group[0].start = 0;
		rd->props.group[0].end = U32_MAX;
		rd->props.group[0].mode = RT_1BYTE_MODE;
		rd->props.group[1].mode = RT_DUMMY_MODE;
	}

	for (i = 0; i < rd->props.register_num; i++)
		bytes_num += rm[i]->size;

	rd->alloc_data = devm_kzalloc(&rd->dev, bytes_num, GFP_KERNEL);
	if (!rd->alloc_data)
		return -ENOMEM;

	for (i = 0; i < rd->props.register_num; i++) {
		rd->cache_data[i] = rd->alloc_data + count;
		count += rm[i]->size;
	}

	/* set 0xff writeable mask for NORMAL and RESERVE type */
	for (i = 0; i < rd->props.register_num; i++) {
		if ((rm[i]->reg_type & RT_REG_TYPE_MASK) == RT_NORMAL ||
		    (rm[i]->reg_type & RT_REG_TYPE_MASK) == RT_RESERVE) {
			for (j = 0; j < rm[i]->size; j++)
				rm[i]->wbit_mask[j] = 0xff;
		}
	}

	dev_info(&rd->dev, "%s: successfully\n", __func__);
	return ret;
}

/* rt_regmap_cache_reload - reload cache from chip */
int rt_regmap_cache_reload(struct rt_regmap_device *rd)
{
	int i = 0;

	mutex_lock(&rd->lock);
	for (i = 0; i < rd->props.register_num; i++)
		rd->cache_dirty[i] = rd->cached[i] = 0;
	mutex_unlock(&rd->lock);
	return 0;
}
EXPORT_SYMBOL(rt_regmap_cache_reload);

/* rt_regmap_add_debugfs - add user own debugfs node
 * @rd: rt_regmap_device pointer.
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fops: a pointer to a struct file_operations that should be used for
 *        this file.
 */
int rt_regmap_add_debugfs(struct rt_regmap_device *rd, const char *name,
			  umode_t mode, void *data, const struct file_operations *fops)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *den = NULL;

	den = debugfs_create_file(name, mode, rd->rt_den, data, fops);
	if (!den)
		return -EINVAL;
#endif /* CONFIG_DEBUG_FS */
	return 0;
}
EXPORT_SYMBOL(rt_regmap_add_debugfs);

static void rt_regmap_set_cache_mode(struct rt_regmap_device *rd,
				     unsigned char mode)
{
	unsigned char mode_mask = mode & RT_CACHE_MODE_MASK;
	unsigned char prev_io_blk;
	int i;

	dev_info(&rd->dev, "%s: mode_mask = %d\n", __func__, mode_mask);
	mutex_lock(&rd->lock);

	prev_io_blk = rd->props.rt_regmap_mode & RT_IO_BLK_MODE_MASK;
	if (prev_io_blk == RT_IO_BLK_CACHE) {
		for (i = 0; i < rd->props.register_num; i++)
			rd->cache_dirty[i] = rd->cached[i] = 0;
	}

	if (mode_mask == RT_CACHE_WR_THROUGH ||
	    mode_mask == RT_CACHE_WR_BACK) {
		for (i = 0; i < rd->props.register_num; i++)
			rd->cache_dirty[i] = rd->cached[i] = 0;
	}

	if (mode_mask == RT_CACHE_WR_THROUGH) {
		rd->regmap_ops.regmap_block_write = rt_cache_block_write;
		rd->regmap_ops.regmap_block_read = rt_cache_block_read;
	} else if (mode_mask == RT_CACHE_WR_BACK) {
		rd->regmap_ops.regmap_block_write = rt_asyn_cache_block_write;
		rd->regmap_ops.regmap_block_read = rt_cache_block_read;
	} else if (mode_mask == RT_CACHE_DISABLE) {
		rd->regmap_ops.regmap_block_write = rt_chip_block_write;
		rd->regmap_ops.regmap_block_read = rt_chip_block_read;
	} else {
		dev_notice(&rd->dev, "%s: invalid cache mode\n", __func__);
		goto err_mode;
	}

	rd->props.rt_regmap_mode &= ~RT_CACHE_MODE_MASK;
	rd->props.rt_regmap_mode |= mode_mask;
err_mode:
	mutex_unlock(&rd->lock);
}

#ifdef CONFIG_DEBUG_FS
struct dentry *rt_regmap_dir;

#define erro_printf(rd, fmt, ...)					\
do {									\
	struct rt_regmap_device *_rd = (rd);				\
	int _ret = 0;							\
	size_t _len = 0;						\
									\
	dev_notice(&_rd->dev, fmt, ##__VA_ARGS__);			\
	mutex_lock(&_rd->lock);						\
	_len = strlen(_rd->err_msg);					\
	_ret = snprintf(_rd->err_msg + _len, ERR_MSG_SIZE - _len,	\
			fmt, ##__VA_ARGS__);				\
	_rd->error_occurred = 1;					\
	mutex_unlock(&_rd->lock);					\
	if (_ret < 0 || _ret >= ERR_MSG_SIZE - _len)			\
		dev_notice(&_rd->dev, "%s: snprintf fail(%d)\n",	\
				__func__, _ret);			\
} while (0)

static int get_parameters(char *buf, unsigned long *param, int num_of_par)
{
	int cnt = 0;
	char *token = strsep(&buf, " ");

	for (cnt = 0; cnt < num_of_par; cnt++) {
		if (token) {
			if (kstrtoul(token, 0, &param[cnt]) != 0)
				return -EINVAL;

			token = strsep(&buf, " ");
		} else {
			return -EINVAL;
		}
	}

	return 0;
}

static int get_data(const char *buf, size_t count,
		    unsigned char *data_buffer, unsigned int data_length)
{
	int i = 0, ptr = 0;
	u8 value = 0;
	char token[5] = {0};

	token[0] = '0';
	token[1] = 'x';
	token[4] = 0;
	if (buf[0] != '0' || buf[1] != 'x')
		return -EINVAL;

	ptr = 2;
	for (i = 0; (i < data_length) && (ptr + 2 <= count); i++) {
		token[2] = buf[ptr++];
		token[3] = buf[ptr++];
		ptr++;
		if (kstrtou8(token, 16, &value) != 0)
			return -EINVAL;
		data_buffer[i] = value;
	}
	return 0;
}

static void rt_show_regs(struct rt_regmap_device *rd,
			 struct seq_file *seq_file)
{
	int ret = 0, i = 0, j = 0;
	const rt_register_map_t *rm = rd->props.rm;
	unsigned char local_val[MAX_BYTE_SIZE];

	for (i = 0; i < rd->props.register_num; i++) {
		mutex_lock(&rd->lock);
		ret = rd->regmap_ops.regmap_block_read(rd, rm[i]->addr,
				rm[i]->size, local_val);
		mutex_unlock(&rd->lock);
		if (ret < 0) {
			erro_printf(rd, "%s: block read fail(%d) @ 0x%02x\n",
				    __func__, ret, rm[i]->addr);
			break;
		}

		if ((rm[i]->reg_type & RT_REG_TYPE_MASK) != RT_RESERVE) {
			seq_printf(seq_file, "reg0x%02x:0x", rm[i]->addr);
			for (j = 0; j < rm[i]->size; j++)
				seq_printf(seq_file, "%02x,", local_val[j]);
			seq_puts(seq_file, "\n");
		} else {
			seq_printf(seq_file, "reg0x%02x:reserve\n",
				   rm[i]->addr);
		}
	}
}

static int general_read(struct seq_file *seq_file, void *_data)
{
	int ret = 0, i = 0, size = 0;
	struct rt_debug_st *st = seq_file->private;
	struct rt_regmap_device *rd = st->info;
	unsigned char local_val[MAX_BYTE_SIZE];
	unsigned char data = 0;

	switch (st->id) {
	case RT_DBG_REG_ADDR:
		seq_printf(seq_file, "0x%02x\n", rd->dbg_data.reg_addr);
		break;
	case RT_DBG_DATA:
		if (rd->dbg_data.reg_size == 0)
			rd->dbg_data.reg_size = 1;
		size = rd->dbg_data.reg_size;

		mutex_lock(&rd->lock);
		if (rd->dbg_data.rio.index < 0)
			ret = rt_chip_block_read(rd, rd->dbg_data.reg_addr,
						 size, local_val);
		else
			ret = rd->regmap_ops.regmap_block_read(rd,
					rd->dbg_data.reg_addr, size, local_val);
		mutex_unlock(&rd->lock);
		if (ret < 0) {
			erro_printf(rd,
				    "%s: block read fail(%d) @ 0x%02x\n",
				    __func__, ret, rd->dbg_data.reg_addr);
			break;
		}

		seq_puts(seq_file, "0x");
		for (i = 0; i < size; i++)
			seq_printf(seq_file, "%02x,", local_val[i]);
		seq_puts(seq_file, "\n");
		break;
	case RT_DBG_ERROR:
		seq_puts(seq_file, "======== Error Message ========\n");
		seq_puts(seq_file,
			 rd->error_occurred ? rd->err_msg : "No Error\n");
		break;
	case RT_DBG_REGS:
		rt_show_regs(rd, seq_file);
		break;
	case RT_DBG_NAME:
		seq_printf(seq_file, "%s\n", rd->props.aliases);
		break;
	case RT_DBG_SIZE:
		seq_printf(seq_file, "%u\n", rd->dbg_data.reg_size);
		break;
	case RT_DBG_BLOCK:
		data = rd->props.rt_regmap_mode & RT_IO_BLK_MODE_MASK;
		if (data == RT_IO_PASS_THROUGH)
			seq_puts(seq_file, "0 => IO_PASS_THROUGH\n");
		else if (data == RT_IO_BLK_ALL)
			seq_puts(seq_file, "1 => IO_BLK_ALL\n");
		else if (data == RT_IO_BLK_CACHE)
			seq_puts(seq_file, "2 => IO_BLK_CACHE\n");
		else if (data == RT_IO_BLK_CHIP)
			seq_puts(seq_file, "3 => IO_BLK_CHIP\n");
		break;
	case RT_DBG_DEVICE_ADDR:
		seq_printf(seq_file, "0x%02x\n", rd->dev_addr);
		break;
	case RT_DBG_SUPPORT_MODE:
		seq_puts(seq_file, " == BLOCK MODE ==\n");
		seq_puts(seq_file, "0 => IO_PASS_THROUGH\n");
		seq_puts(seq_file, "1 => IO_BLK_ALL\n");
		seq_puts(seq_file, "2 => IO_BLK_CACHE\n");
		seq_puts(seq_file, "3 => IO_BLK_CHIP\n");
		seq_puts(seq_file, " == CACHE MODE ==\n");
		seq_puts(seq_file, "0 => CACHE_WR_THROUGH\n");
		seq_puts(seq_file, "1 => CACHE_WR_BACK\n");
		seq_puts(seq_file, "2 => CACHE_DISABLE\n");
		break;
	case RT_DBG_IO_LOG:
		seq_printf(seq_file, "%d\n", rd->props.io_log_en);
		break;
	case RT_DBG_CACHE_MODE:
		data = rd->props.rt_regmap_mode & RT_CACHE_MODE_MASK;
		if (data == RT_CACHE_WR_THROUGH)
			seq_puts(seq_file, "0 => CACHE_WR_THROUGH\n");
		else if (data == RT_CACHE_WR_BACK)
			seq_puts(seq_file, "1 => CACHE_WR_BACK\n");
		else if (data == RT_CACHE_DISABLE)
			seq_puts(seq_file, "2 => CACHE_DISABLE\n");
		break;
	case RT_DBG_REG_SIZE:
		size = rt_get_regsize(rd, rd->dbg_data.reg_addr);
		seq_printf(seq_file, "%d\n", size);
		break;
	}
	return 0;
}

static int general_open(struct inode *inode, struct file *file)
{
	return single_open(file, general_read, inode->i_private);
}

static ssize_t general_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	int ret = 0;
	struct rt_debug_st *st =
		((struct seq_file *)file->private_data)->private;
	struct rt_regmap_device *rd = st->info;
	struct reg_index_offset rio = {-1, -1};
	unsigned char local_val[MAX_BYTE_SIZE];
	char lbuf[128];
	ssize_t res = 0;
	unsigned int size = 0;
	unsigned long param = 0;

	dev_info(&rd->dev, "%s: @ %p, count = %u, pos = %llu\n",
		 __func__, ubuf, (unsigned int)count, *ppos);
	*ppos = 0;
	res = simple_write_to_buffer(lbuf, sizeof(lbuf) - 1,
				     ppos, ubuf, count);
	if (res <= 0)
		return -EFAULT;
	count = res;
	lbuf[count] = '\0';

	switch (st->id) {
	case RT_DBG_REG_ADDR:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		rio = find_register_index(rd, param);
		mutex_lock(&rd->lock);
		rd->dbg_data.rio = rio;
		rd->dbg_data.reg_addr = param;
		mutex_unlock(&rd->lock);
		if (rio.index < 0)
			erro_printf(rd, "%s: reg 0x%02lx is out of range\n",
				    __func__, param);
		break;
	case RT_DBG_DATA:
		if (rd->dbg_data.reg_size == 0)
			rd->dbg_data.reg_size = 1;
		size = rd->dbg_data.reg_size;

		if ((size - 1) * 3 + 5 != count) {
			erro_printf(rd,
				    "%s: wrong input length, size = %u, count = %u\n",
				    __func__, size, (unsigned int)count);
			return -EINVAL;
		}

		memset(local_val, 0, sizeof(local_val));
		ret = get_data(lbuf, count, local_val, size);
		if (ret < 0) {
			erro_printf(rd, "%s: get data fail(%d)\n",
				    __func__, ret);
			return ret;
		}

		mutex_lock(&rd->lock);
		if (rd->dbg_data.rio.index < 0)
			ret = rt_chip_block_write(rd, rd->dbg_data.reg_addr,
						  size, local_val);
		else
			ret = rd->regmap_ops.regmap_block_write(rd,
					rd->dbg_data.reg_addr, size,
					local_val);
		mutex_unlock(&rd->lock);
		if (ret < 0) {
			erro_printf(rd,
				    "%s: block write fail(%d) @ 0x%02x\n",
				    __func__, ret, rd->dbg_data.reg_addr);
			return ret;
		}
		break;
	case RT_DBG_SYNC:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		if (param)
			rt_regmap_cache_sync(rd);
		break;
	case RT_DBG_ERROR:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		if (param)
			rt_cache_clrlasterror(rd);
		break;
	case RT_DBG_SIZE:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		if (param >= 1 && param <= MAX_BYTE_SIZE) {
			mutex_lock(&rd->lock);
			rd->dbg_data.reg_size = param;
			mutex_unlock(&rd->lock);
		} else {
			erro_printf(rd,
				    "%s: size(%lu) must be %d ~ %d\n",
				    __func__, param, 1, MAX_BYTE_SIZE);
			return -EINVAL;
		}
		break;
	case RT_DBG_BLOCK:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		if (param > 3)
			param = 3;
		param <<= 3;

		mutex_lock(&rd->lock);
		rd->props.rt_regmap_mode &= ~RT_IO_BLK_MODE_MASK;
		rd->props.rt_regmap_mode |= param;
		mutex_unlock(&rd->lock);
		if (param == RT_IO_PASS_THROUGH)
			rt_regmap_cache_sync(rd);
		break;
	case RT_DBG_IO_LOG:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		mutex_lock(&rd->lock);
		rd->props.io_log_en = !!param;
		mutex_unlock(&rd->lock);
		break;
	case RT_DBG_CACHE_MODE:
		ret = get_parameters(lbuf, &param, 1);
		if (ret < 0)
			return ret;
		if (param > 2)
			param = 2;
		param <<= 1;
		rt_regmap_set_cache_mode(rd, param);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static const struct file_operations general_ops = {
	.owner = THIS_MODULE,
	.open = general_open,
	.write = general_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int rt_create_general_debug(struct rt_regmap_device *rd,
				   struct dentry *dir)
{
	static const struct {
		int id;
		const char *name;
		umode_t mode;
	} files[] = {
		{ RT_DBG_REG_ADDR, "reg_addr", 0644 },
		{ RT_DBG_DATA, "data", 0644 },
		{ RT_DBG_SYNC, "sync", 0644 },
		{ RT_DBG_ERROR, "Error", 0644 },
		{ RT_DBG_BLOCK, "block", 0644 },
		{ RT_DBG_SIZE, "size", 0644 },
		{ RT_DBG_IO_LOG, "io_log", 0644 },
		{ RT_DBG_CACHE_MODE, "cache_mode", 0644 },
		{ RT_DBG_REGS, "regs", 0444 },
		{ RT_DBG_NAME, "name", 0444 },
		{ RT_DBG_DEVICE_ADDR, "device_addr", 0444 },
		{ RT_DBG_SUPPORT_MODE, "support_mode", 0444 },
		{ RT_DBG_REG_SIZE, "reg_size", 0444 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		int id = files[i].id;

		rd->rtdbg_st[id].info = rd;
		rd->rtdbg_st[id].id = id;
		rd->rt_debug_file[id] = debugfs_create_file(files[i].name,
							    files[i].mode, dir,
							    &rd->rtdbg_st[id],
							    &general_ops);
		if (!rd->rt_debug_file[id])
			return -EINVAL;
	}

	return 0;
}

static int eachreg_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t eachreg_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	int ret = 0, i = 0, j = 0;
	struct rt_debug_st *st = file->private_data;
	struct rt_regmap_device *rd = st->info;
	const rt_register_map_t rm = rd->props.rm[st->id];
	unsigned char local_val[MAX_BYTE_SIZE];
	char *lbuf = NULL;
	const size_t lbuf_size = MAX_BYTE_SIZE * 3 + 11;

	lbuf = kzalloc(lbuf_size, GFP_KERNEL);
	if (!lbuf)
		return -ENOMEM;

	if (rm->size > MAX_BYTE_SIZE) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_read(rd, rm->addr, rm->size,
					       local_val);
	mutex_unlock(&rd->lock);
	if (ret < 0) {
		dev_notice(&rd->dev, "%s: block read fail(%d) @ 0x%02x\n",
			   __func__, ret, rm->addr);
		goto out;
	}

	ret = snprintf(lbuf + j, lbuf_size - j, "reg0x%02x:0x", rm->addr);
	if (ret < 0 || ret >= lbuf_size - j) {
		ret = -EINVAL;
		goto out;
	}

	j += ret;
	for (i = 0; i < rm->size; i++) {
		ret = snprintf(lbuf + j, lbuf_size - j, "%02x,", local_val[i]);
		if (ret < 0 || ret >= lbuf_size - j) {
			ret = -EINVAL;
			goto out;
		}
		j += ret;
	}

	ret = snprintf(lbuf + j, lbuf_size - j, "\n");
	if (ret < 0 || ret >= lbuf_size - j) {
		ret = -EINVAL;
		goto out;
	}
	j += ret;
	ret = simple_read_from_buffer(ubuf, count, ppos, lbuf, strlen(lbuf));
out:
	kfree(lbuf);
	return ret;
}

static ssize_t eachreg_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	int ret = 0;
	struct rt_debug_st *st = file->private_data;
	struct rt_regmap_device *rd = st->info;
	const rt_register_map_t rm = rd->props.rm[st->id];
	unsigned char local_val[MAX_BYTE_SIZE];
	char lbuf[128];
	ssize_t res = 0;

	if ((rm->size - 1) * 3 + 5 != count) {
		dev_notice(&rd->dev,
			   "%s: wrong input length, size = %u, count = %u\n",
			   __func__, rm->size, (unsigned int)count);
		return -EINVAL;
	}

	dev_info(&rd->dev, "%s: @ %p, count = %u, pos = %llu\n",
		 __func__, ubuf, (unsigned int)count, *ppos);
	*ppos = 0;
	res = simple_write_to_buffer(lbuf, sizeof(lbuf) - 1,
				     ppos, ubuf, count);
	if (res <= 0)
		return -EFAULT;
	count = res;
	lbuf[count] = '\0';

	memset(local_val, 0, sizeof(local_val));
	ret = get_data(lbuf, count, local_val, rm->size);
	if (ret < 0) {
		dev_notice(&rd->dev, "%s: get data fail(%d)\n", __func__, ret);
		return ret;
	}

	mutex_lock(&rd->lock);
	ret = rd->regmap_ops.regmap_block_write(rd, rm->addr, rm->size,
						local_val);
	mutex_unlock(&rd->lock);
	if (ret < 0) {
		dev_notice(&rd->dev, "%s: block write fail(%d) @ 0x%02x\n",
			   __func__, ret, rm->addr);
		return ret;
	}

	return count;
}

static const struct file_operations eachreg_ops = {
	.open = eachreg_open,
	.read = eachreg_read,
	.write = eachreg_write,
};

/* create every register node at debugfs */
static int rt_create_every_debug(struct rt_regmap_device *rd,
				 struct dentry *dir)
{
	int ret = 0, i = 0;
	char buf[10];

	rd->rt_reg_file = devm_kzalloc(&rd->dev,
				       rd->props.register_num * sizeof(*rd->rt_reg_file),
				       GFP_KERNEL);
	if (!rd->rt_reg_file)
		return -ENOMEM;

	rd->reg_st = devm_kzalloc(&rd->dev,
				  rd->props.register_num * sizeof(*rd->reg_st),
				  GFP_KERNEL);
	if (!rd->reg_st)
		return -ENOMEM;

	for (i = 0; i < rd->props.register_num; i++) {
		ret = snprintf(buf, sizeof(buf), "reg0x%02x",
			       rd->props.rm[i]->addr);
		if (ret < 0 || ret >= sizeof(buf)) {
			dev_notice(&rd->dev, "%s: snprintf fail(%d)\n",
				   __func__, ret);
			continue;
		}

		rd->rt_reg_file[i] = devm_kzalloc(&rd->dev,
						  sizeof(*rd->rt_reg_file[i]), GFP_KERNEL);
		rd->reg_st[i] = devm_kzalloc(&rd->dev,
					     sizeof(*rd->reg_st[i]), GFP_KERNEL);
		if (!rd->rt_reg_file[i] || !rd->reg_st[i])
			return -ENOMEM;

		rd->reg_st[i]->info = rd;
		rd->reg_st[i]->id = i;
		rd->rt_reg_file[i] = debugfs_create_file(buf, 0644, dir,
							 rd->reg_st[i], &eachreg_ops);
		if (!rd->rt_reg_file[i])
			return -EINVAL;
	}

	return 0;
}
#endif /* CONFIG_DEBUG_FS */

/* check the rt_register format is correct */
static int rt_regmap_check(struct rt_regmap_device *rd)
{
	int i = 0;
	const rt_register_map_t *rm = rd->props.rm;

	/* check name property */
	if (!rd->props.name) {
		pr_notice("%s: no name\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < rd->props.register_num; i++) {
		/* check byte size, 1 byte ~ 32 bytes is valid */
		if (rm[i]->size < 1 || rm[i]->size > MAX_BYTE_SIZE) {
			pr_notice("%s: size(%d) must be %d ~ %d @ 0x%02x\n",
				  __func__, rm[i]->size, 1, MAX_BYTE_SIZE,
				  rm[i]->addr);
			return -EINVAL;
		}
	}

	for (i = 0; i < rd->props.register_num - 1; i++) {
		/* check register sequence */
		if (rm[i]->addr >= rm[i + 1]->addr)
			pr_info("%s: sequence error @ 0x%02x\n",
				__func__, rm[i]->addr);
	}

	/* no default reg_addr and register_map first addr is not 0x00 */
#ifdef CONFIG_DEBUG_FS
	if (!rd->dbg_data.reg_addr && rm[0]->addr) {
		rd->dbg_data.reg_addr = rm[0]->addr;
		rd->dbg_data.rio.index = 0;
		rd->dbg_data.rio.offset = 0;
	}
#endif /* CONFIG_DEBUG_FS */
	return 0;
}

static void rt_regmap_dev_release(struct device *dev)
{
}

struct rt_regmap_device *rt_regmap_device_register_ex(struct rt_regmap_properties *props,
						      struct rt_regmap_fops *rops,
						      struct device *parent,
						      void *client,
						      int dev_addr,
						      void *drvdata)
{
	int ret = 0, i = 0;
	struct rt_regmap_device *rd = NULL;

	if (!props) {
		pr_notice("%s: rt_regmap_properties is NULL\n", __func__);
		return NULL;
	}

	if (!rops) {
		pr_notice("%s: rt_regmap_fops is NULL\n", __func__);
		return NULL;
	}

	pr_info("%s: name = %s\n", __func__, props->name);
	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	memcpy(&rd->props, props, sizeof(rd->props));
	rd->rops = rops;
	rd->dev.parent = parent;
	rd->dev.release = rt_regmap_dev_release;
	dev_set_drvdata(&rd->dev, drvdata);
	rd->client = client;
	mutex_init(&rd->lock);
	INIT_DELAYED_WORK(&rd->rt_work, rt_work_func);
	rd->dev_addr = dev_addr;

	/* check rt_register_map format */
	ret = rt_regmap_check(rd);
	if (ret < 0) {
		pr_notice("%s: check fail(%d)\n", __func__, ret);
		goto out;
	}

	dev_set_name(&rd->dev, "rt_regmap_%s", rd->props.name);
	ret = device_register(&rd->dev);
	if (ret) {
		pr_notice("%s: device register fail(%d)\n", __func__, ret);
		goto out;
	}

	rd->err_msg = devm_kzalloc(&rd->dev, ERR_MSG_SIZE, GFP_KERNEL);
	if (!rd->err_msg)
		goto err_msgalloc;

	ret = rt_regmap_cache_init(rd);
	if (ret < 0) {
		pr_notice("%s: init fail(%d)\n", __func__, ret);
		goto err_cacheinit;
	}

	for (i = 0; i <= 3; i++)
		rd->rt_block_write[i] = rt_block_map[i];

	rt_regmap_set_cache_mode(rd, rd->props.rt_regmap_mode);

#ifdef CONFIG_DEBUG_FS
	rd->rt_den = debugfs_create_dir(props->name, rt_regmap_dir);
	if (rd->rt_den) {
		ret = rt_create_general_debug(rd, rd->rt_den);
		if (ret < 0) {
			pr_notice("%s: create general debug fail(%d)\n",
				  __func__, ret);
			goto err_create_general_debug;
		}
		if (rd->props.rt_regmap_mode & RT_DBG_MODE_MASK) {
			ret = rt_create_every_debug(rd, rd->rt_den);
			if (ret < 0) {
				pr_notice("%s: create every debug fail(%d)\n",
					  __func__, ret);
				goto err_create_every_debug;
			}
		}
	} else {
		pr_notice("%s: debugfs create dir fail\n", __func__);
		goto err_debug;
	}
#endif /* CONFIG_DEBUG_FS */

	return rd;

#ifdef CONFIG_DEBUG_FS
err_create_every_debug:
err_create_general_debug:
	debugfs_remove_recursive(rd->rt_den);
err_debug:
#endif /* CONFIG_DEBUG_FS */
err_cacheinit:
err_msgalloc:
	device_unregister(&rd->dev);
out:
	mutex_destroy(&rd->lock);
	kfree(rd);
	return NULL;
}
EXPORT_SYMBOL(rt_regmap_device_register_ex);

void rt_regmap_device_unregister(struct rt_regmap_device *rd)
{
	if (!rd)
		return;

	cancel_delayed_work_sync(&rd->rt_work);
	mutex_lock(&rd->lock);
	rd->rops = NULL;
	mutex_unlock(&rd->lock);
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(rd->rt_den);
#endif /* CONFIG_DEBUG_FS */
	device_unregister(&rd->dev);
	mutex_destroy(&rd->lock);
	kfree(rd);
}
EXPORT_SYMBOL(rt_regmap_device_unregister);

int rt_regmap_init(void)
{
	pr_info("Init Richtek RegMap %s\n", RT_REGMAP_VERSION);
#ifdef CONFIG_DEBUG_FS
	rt_regmap_dir = debugfs_create_dir("rt-regmap", NULL);
	if (!rt_regmap_dir) {
		pr_notice("%s: debugfs create dir fail\n", __func__);
		return -EINVAL;
	}
#endif /* CONFIG_DEBUG_FS */
	return 0;
}

void rt_regmap_exit(void)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(rt_regmap_dir);
#endif /* CONFIG_DEBUG_FS */
}

MODULE_DESCRIPTION("Richtek regmap Driver");
MODULE_AUTHOR("Jeff Chang <jeff_chang@richtek.com>");
MODULE_LICENSE("GPL");

/* Release Note
 * 1.2.3
 *	Replace binary semaphore with mutex for rd->lock
 *	Remove write_mode_lock semaphore
 *	Inline cache invalidation into rt_regmap_set_cache_mode
 *	Remove unnecessary lock acquisition in rt_regmap_cache_init()
 *	Completes the local stack buffer migration
 *
 * 1.2.2
 *	Fix use-after-free in rt_regmap_device_unregister()
 *	Fix potential deadlock in rt_regmap_set_cache_mode()
 *	Fix BLK_ALL silent data discard
 *	Fix stale cache after BLK_CACHE
 *	Fix partial dirty-bit clearing in cache_sync/write_back
 *	Fix debugfs permissions
 *	Remove dead code in rt_cache_clrlasterror
 *	Fix false error return when IO log buffer overflows
 *	Fix eachreg_read / rt_show_regs / general_read race on rd->regval
 *	Add input validation for bytes in public block read/write API
 *
 * 1.2.1
 *	Fix the deadlock in rt_asyn_cache_block_write()
 *
 * 1.2.0
 *	Revise memory allocation, code flow, and error handling
 *
 * 1.1.14
 *	Fix Coverity by Mandatory's
 */
