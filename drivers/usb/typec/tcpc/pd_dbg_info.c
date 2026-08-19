// SPDX-License-Identifier: GPL-2.0-only
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

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched/clock.h>
#include <linux/wait.h>
#include "inc/pd_dbg_info.h"

#ifdef CONFIG_PD_DBG_INFO

#define PD_INFO_BUF_SIZE	(2048 * 256)
#define OUT_BUF_MAX		128

static struct {
	int used;
	char buf[PD_INFO_BUF_SIZE + OUT_BUF_MAX];
} pd_dbg_buffer[2];

static struct task_struct *print_out_task;
static DEFINE_MUTEX(buff_lock);
static unsigned int using_buf;
static DECLARE_WAIT_QUEUE_HEAD(print_out_wq);
static DECLARE_WAIT_QUEUE_HEAD(busy_wq);
static atomic_t pending_print_out = ATOMIC_INIT(0);
static atomic_t busy = ATOMIC_INIT(0);

void pd_dbg_info_lock(void)
{
	atomic_inc(&busy);
}

void pd_dbg_info_unlock(void)
{
	if (atomic_dec_and_test(&busy))
		wake_up(&busy_wq);
}

static bool pd_dbg_print_out(void)
{
	unsigned int index;
	int used;
	unsigned int i;

	mutex_lock(&buff_lock);
	index = using_buf;
	using_buf ^= 1;
	used = pd_dbg_buffer[index].used;
	mutex_unlock(&buff_lock);

	if (used == 0)
		return false;

	used = min(used, PD_INFO_BUF_SIZE - 1);
	pd_dbg_buffer[index].buf[used] = '\0';

	pr_info("///PD dbg info %u\n", used);

	for (i = 0; i < (unsigned int)used && !kthread_should_stop();
	     i += OUT_BUF_MAX) {
		unsigned int chunk = min_t(unsigned int, used - i, OUT_BUF_MAX);

		wait_event(busy_wq,
			   !atomic_read(&busy) || kthread_should_stop());

		pr_info("%.*s", (int)chunk, pd_dbg_buffer[index].buf + i);
	}

	pd_dbg_buffer[index].used = 0;
	return true;
}

static int print_out_thread_fn(void *data)
{
	while (!kthread_should_stop()) {
		wait_event(print_out_wq,
			   atomic_read(&pending_print_out) ||
			   kthread_should_stop());
		if (kthread_should_stop())
			break;

		do {
			atomic_dec_if_positive(&pending_print_out);
		} while (pd_dbg_print_out() && !kthread_should_stop());
	}

	return 0;
}

int pd_dbg_info(const char *fmt, ...)
{
	va_list args;
	unsigned int index;
	char *buf;
	int used, r;
	int remaining;
	u64 ts;
	unsigned long rem_msec;

	ts = local_clock();
	rem_msec = do_div(ts, 1000000000) / 1000000;

	va_start(args, fmt);
	mutex_lock(&buff_lock);

	index = using_buf;
	used = pd_dbg_buffer[index].used;
	remaining = PD_INFO_BUF_SIZE - used - 1;

	if (remaining <= 0) {
		mutex_unlock(&buff_lock);
		va_end(args);
		return -ENOSPC;
	}

	buf = pd_dbg_buffer[index].buf;

	r = snprintf(buf + used, remaining + 1,
		     "<%5lu.%03lu>", (unsigned long)ts, rem_msec);
	if (r > 0) {
		r = min(r, remaining);
		used += r;
		remaining -= r;
	}

	r = vsnprintf(buf + used, remaining + 1, fmt, args);
	if (r > 0) {
		r = min(r, remaining);
		used += r;
	}

	if (pd_dbg_buffer[index].used == 0) {
		atomic_inc(&pending_print_out);
		wake_up(&print_out_wq);
	}

	pd_dbg_buffer[index].used = min(used, PD_INFO_BUF_SIZE - 1);

	mutex_unlock(&buff_lock);
	va_end(args);
	return r;
}

int pd_dbg_info_init(void)
{
	int err;

	atomic_set(&pending_print_out, 0);
	atomic_set(&busy, 0);

	print_out_task = kthread_run(print_out_thread_fn, NULL, "pd_dbg_info");
	if (IS_ERR(print_out_task)) {
		err = PTR_ERR(print_out_task);
		print_out_task = NULL;
		pr_err("pd_dbg_info: failed to create print thread: %d\n", err);
		return err;
	}

	return 0;
}

void pd_dbg_info_exit(void)
{
	if (print_out_task) {
		kthread_stop(print_out_task);
		print_out_task = NULL;
	}
}

MODULE_LICENSE("GPL");

#endif /* CONFIG_PD_DBG_INFO */