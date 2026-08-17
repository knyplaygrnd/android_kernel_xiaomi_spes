/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM psi

#if !defined(_TRACE_PSI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PSI_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <linux/psi_types.h>

#define show_psi_type(type)			\
	__print_symbolic(type,			\
	{ PSI_IO_SOME, "io_some" },		\
	{ PSI_IO_FULL, "io_full" },		\
	{ PSI_MEM_SOME,	"mem_some" },	\
	{ PSI_MEM_FULL, "mem_full" },	\
	{ PSI_CPU_SOME, "cpu_some" },	\
	{ PSI_NONIDLE, "non_idle" })

        TRACE_EVENT(psi_window_vmstat,

	TP_PROTO(u64 mem_some, u64 mem_full, const char *zone_name, u64 high,
		u64 free, u64 cma, u64 file),

	TP_ARGS(mem_some, mem_full, zone_name, high, free, cma, file),

	TP_STRUCT__entry(
		__field(u64, mem_some)
		__field(u64, mem_full)
		__string(name, zone_name)
		__field(u64, high)
		__field(u64, free)
		__field(u64, cma)
		__field(u64, file)
	),

	TP_fast_assign(
		__entry->mem_some = mem_some;
		__entry->mem_full = mem_full;
		__assign_str(name, zone_name);
		__entry->high = high;
		__entry->free = free;
		__entry->cma = cma;
		__entry->file = file;
	),

	TP_printk("%16s: MEMSOME: %9lluns MEMFULL: %9lluns High: %9llukB Free: %9llukB CMA: %8llukB File: %9llukB",
		__get_str(name), __entry->mem_some, __entry->mem_full,
		__entry->high, __entry->free, __entry->cma, __entry->file
	)
);

TRACE_EVENT(psi_event,

	TP_PROTO(enum psi_states state, u64 threshold),

	TP_ARGS(state, threshold),

	TP_STRUCT__entry(
		__field(enum psi_states, state)
		__field(u64,	threshold)
		__field(u64,	growth)
		__field(u64,	last_event_diff)
		__field(u64,	w_size)
		__field(u64,	elapsed)
	),

	TP_fast_assign(
		__entry->state			= t->state;
		__entry->growth			= growth;
		__entry->threshold		= t->threshold;
		__entry->elapsed		= now - t->win.start_time;
		__entry->w_size			= t->win.size;
		__entry->last_event_diff	= now - t->last_event_time;
	),

	TP_printk("%s growth=%llu threshold=%llu elapsed=%llu win=%llu last_event_diff=%llu",
			show_psi_type(__entry->state),
			__entry->growth,
			__entry->threshold,
			__entry->elapsed,
			__entry->w_size,
			__entry->last_event_diff)
);

TRACE_EVENT(psi_update_trigger_wake_up,

	TP_PROTO(struct psi_trigger *t, u64 growth),

	TP_ARGS(t, growth),

	TP_STRUCT__entry(
		__field(enum psi_states, state)
		__field(u64,	threshold)
		__field(u64,	growth)
		__field(u64,	last_event_time)
		__field(u64,	w_size)
	),

	TP_fast_assign(
		__entry->state			= t->state;
		__entry->growth			= growth;
		__entry->threshold		= t->threshold;
		__entry->w_size			= t->win.size;
		__entry->last_event_time	= t->last_event_time;
	),

	TP_printk("%s growth=%llu threshold=%llu win=%llu last_event_time=%llu",
			show_psi_type(__entry->state),
			__entry->growth,
			__entry->threshold,
			__entry->w_size,
			__entry->last_event_time)
);

#endif /* _TRACE_PSI_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
