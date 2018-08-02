/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "trace-cmd.h"

struct plugin_option trace_ftrace_options[] = {
	{
		.name = "tailprint",
		.plugin_alias = "fgraph",
		.description =
		"Print function name at function exit in function graph",
	},
	{
		.name = NULL,
	}
};

static struct plugin_option *fgraph_tail = &trace_ftrace_options[0];

static void find_long_size(struct tracecmd_ftrace *finfo)
{
	finfo->long_size = tracecmd_long_size(finfo->handle);
}

#define long_size_check(handle)				\
	do {						\
		if (!finfo->long_size)				\
			find_long_size(finfo);		\
	} while (0)

static int find_ret_event(struct tracecmd_ftrace *finfo, struct pevent *pevent)
{
	struct event_format *event;

	/* Store the func ret id and event for later use */
	event = pevent_find_event_by_name(pevent, "ftrace", "funcgraph_exit");
	if (!event)
		return -1;

	finfo->fgraph_ret_id = event->id;
	finfo->fgraph_ret_event = event;
	return 0;
}

#define ret_event_check(finfo, pevent)					\
	do {								\
		if (!finfo->fgraph_ret_event && find_ret_event(finfo, pevent) < 0) \
			return -1;					\
	} while (0)

static int function_handler(struct trace_seq *s, struct record *record,
			    struct event_format *event, void *context)
{
	struct pevent *pevent = event->pevent;
	unsigned long long function;
	const char *func;

	if (pevent_get_field_val(s, event, "ip", record, &function, 1))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, function);
	if (func)
		trace_seq_printf(s, "%s <-- ", func);
	else
		trace_seq_printf(s, "0x%llx", function);

	if (pevent_get_field_val(s, event, "parent_ip", record, &function, 1))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, function);
	if (func)
		trace_seq_printf(s, "%s", func);
	else
		trace_seq_printf(s, "0x%llx", function);

	return 0;
}

#define TRACE_GRAPH_INDENT		2

static struct record *
get_return_for_leaf(struct trace_seq *s, int cpu, int cur_pid,
		    unsigned long long cur_func, struct record *next,
		    struct tracecmd_ftrace *finfo)
{
	unsigned long long val;
	unsigned long long type;
	unsigned long long pid;

	/* Searching a common field, can use any event */
	if (pevent_get_common_field_val(s, finfo->fgraph_ret_event, "common_type", next, &type, 1))
		return NULL;

	if (type != finfo->fgraph_ret_id)
		return NULL;

	if (pevent_get_common_field_val(s, finfo->fgraph_ret_event, "common_pid", next, &pid, 1))
		return NULL;

	if (cur_pid != pid)
		return NULL;

	/* We aleady know this is a funcgraph_ret_event */
	if (pevent_get_field_val(s, finfo->fgraph_ret_event, "func", next, &val, 1))
		return NULL;

	if (cur_func != val)
		return NULL;

	/* this is a leaf, now advance the iterator */
	return tracecmd_read_data(tracecmd_curr_thread_handle, cpu);
}

/* Signal a overhead of time execution to the output */
static void print_graph_overhead(struct trace_seq *s,
				 unsigned long long duration)
{
	/* Non nested entry or return */
	if (duration == ~0ULL)
		return (void)trace_seq_printf(s, "  ");

	/* Duration exceeded 100 msecs */
	if (duration > 100000ULL)
		return (void)trace_seq_printf(s, "! ");

	/* Duration exceeded 10 msecs */
	if (duration > 10000ULL)
		return (void)trace_seq_printf(s, "+ ");

	trace_seq_printf(s, "  ");
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static void print_graph_duration(struct trace_seq *s, unsigned long long duration)
{
	unsigned long usecs = duration / 1000;
	unsigned long nsecs_rem = duration % 1000;
	/* log10(ULONG_MAX) + '\0' */
	char msecs_str[21];
	char nsecs_str[5];
	int len;
	int i;

	sprintf(msecs_str, "%lu", usecs);

	/* Print msecs */
	len = s->len;
	trace_seq_printf(s, "%lu", usecs);

	/* Print nsecs (we don't want to exceed 7 numbers) */
	if ((s->len - len) < 7) {
		snprintf(nsecs_str, MIN(sizeof(nsecs_str), 8 - len), "%03lu", nsecs_rem);
		trace_seq_printf(s, ".%s", nsecs_str);
	}

	len = s->len - len;

	trace_seq_puts(s, " us ");

	/* Print remaining spaces to fit the row's width */
	for (i = len; i < 7; i++)
		trace_seq_putc(s, ' ');

	trace_seq_puts(s, "|  ");
}

static int
print_graph_entry_leaf(struct trace_seq *s,
		       struct event_format *event,
		       struct record *record, struct record *ret_rec,
		       struct tracecmd_ftrace *finfo)
{
	struct pevent *pevent = event->pevent;
	unsigned long long rettime, calltime;
	unsigned long long duration, depth;
	unsigned long long val;
	const char *func;
	int i;

	if (pevent_get_field_val(s, finfo->fgraph_ret_event, "rettime", ret_rec, &rettime, 1))
		return trace_seq_putc(s, '!');

	if (pevent_get_field_val(s, finfo->fgraph_ret_event, "calltime", ret_rec, &calltime, 1))
		return trace_seq_putc(s, '!');

	duration = rettime - calltime;

	/* Overhead */
	print_graph_overhead(s, duration);

	/* Duration */
	print_graph_duration(s, duration);

	if (pevent_get_field_val(s, event, "depth", record, &depth, 1))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	if (pevent_get_field_val(s, event, "func", record, &val, 1))
		return trace_seq_putc(s, '!');
	func = pevent_find_function(pevent, val);

	if (func)
		return trace_seq_printf(s, "%s();", func);
	else
		return trace_seq_printf(s, "%llx();", val);
}

static int print_graph_nested(struct trace_seq *s,
			      struct event_format *event,
			      struct record *record)
{
	struct pevent *pevent = event->pevent;
	unsigned long long depth;
	unsigned long long val;
	const char *func;
	int i;

	/* No overhead */
	print_graph_overhead(s, -1);

	/* No time */
	trace_seq_puts(s, "           |  ");

	if (pevent_get_field_val(s, event, "depth", record, &depth, 1))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	if (pevent_get_field_val(s, event, "func", record, &val, 1))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, val);

	if (func)
		return trace_seq_printf(s, "%s() {", func);
	else
		return trace_seq_printf(s, "%llx() {", val);
}

static int
fgraph_ent_handler(struct trace_seq *s, struct record *record,
		   struct event_format *event, void *context)
{
	struct tracecmd_ftrace *finfo = context;
	struct record *rec;
	unsigned long long val, pid;
	int cpu = record->cpu;

	ret_event_check(finfo, event->pevent);

	if (pevent_get_common_field_val(s, event, "common_pid", record, &pid, 1))
		return trace_seq_putc(s, '!');

	if (pevent_get_field_val(s, event, "func", record, &val, 1))
		return trace_seq_putc(s, '!');

	rec = tracecmd_peek_data(tracecmd_curr_thread_handle, cpu);
	if (rec)
		rec = get_return_for_leaf(s, cpu, pid, val, rec, finfo);

	if (rec) {
		/*
		 * If this is a leaf function, then get_return_for_leaf
		 * returns the return of the function
		 */
		print_graph_entry_leaf(s, event, record, rec, finfo);
		free_record(rec);
	} else
		print_graph_nested(s, event, record);

	return 0;
}

static int
fgraph_ret_handler(struct trace_seq *s, struct record *record,
		   struct event_format *event, void *context)
{
	struct tracecmd_ftrace *finfo = context;
	unsigned long long rettime, calltime;
	unsigned long long duration, depth;
	unsigned long long val;
	const char *func;
	int i;

	ret_event_check(finfo, event->pevent);

	if (pevent_get_field_val(s, event, "rettime", record, &rettime, 1))
		return trace_seq_putc(s, '!');

	if (pevent_get_field_val(s, event, "calltime", record, &calltime, 1))
		return trace_seq_putc(s, '!');

	duration = rettime - calltime;

	/* Overhead */
	print_graph_overhead(s, duration);

	/* Duration */
	print_graph_duration(s, duration);

	if (pevent_get_field_val(s, event, "depth", record, &depth, 1))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	trace_seq_putc(s, '}');

	if (fgraph_tail->set) {
		if (pevent_get_field_val(s, event, "func", record, &val, 0))
			return 0;
		func = pevent_find_function(event->pevent, val);
		if (!func)
			return 0;
		trace_seq_printf(s, " /* %s */", func);
	}

	return 0;
}

static int
trace_stack_handler(struct trace_seq *s, struct record *record,
		    struct event_format *event, void *context)
{
	struct tracecmd_ftrace *finfo = context;
	struct format_field *field;
	unsigned long long addr;
	const char *func;
	void *data = record->data;

	field = pevent_find_any_field(event, "caller");
	if (!field) {
		trace_seq_printf(s, "<CANT FIND FIELD %s>", "caller");
		return 0;
	}

	trace_seq_puts(s, "<stack trace>\n");

	long_size_check(finfo);

	for (data += field->offset; data < record->data + record->size;
	     data += finfo->long_size) {
		addr = pevent_read_number(event->pevent, data, finfo->long_size);

		if ((finfo->long_size == 8 && addr == (unsigned long long)-1) ||
		    ((int)addr == -1))
			break;

		func = pevent_find_function(event->pevent, addr);
		if (func)
			trace_seq_printf(s, "=> %s (%llx)\n", func, addr);
		else
			trace_seq_printf(s, "=> %llx\n", addr);
	}

	return 0;
}

int tracecmd_ftrace_overrides(struct tracecmd_input *handle,
	struct tracecmd_ftrace *finfo)
{
	struct pevent *pevent;
	struct event_format *event;

	finfo->handle = handle;

	pevent = tracecmd_get_pevent(handle);

	pevent_register_event_handler(pevent, -1, "ftrace", "function",
				      function_handler, NULL);

	pevent_register_event_handler(pevent, -1, "ftrace", "funcgraph_entry",
				      fgraph_ent_handler, finfo);

	pevent_register_event_handler(pevent, -1, "ftrace", "funcgraph_exit",
				      fgraph_ret_handler, finfo);

	pevent_register_event_handler(pevent, -1, "ftrace", "kernel_stack",
				      trace_stack_handler, finfo);

	/* Store the func ret id and event for later use */
	event = pevent_find_event_by_name(pevent, "ftrace", "funcgraph_exit");
	if (!event)
		return 0;

	finfo->long_size = tracecmd_long_size(handle);

	finfo->fgraph_ret_id = event->id;
	finfo->fgraph_ret_event = event;

	return 0;
}
