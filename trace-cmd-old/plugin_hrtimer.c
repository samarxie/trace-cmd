/*
 * Copyright (C) 2009 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 * Copyright (C) 2009 Johannes Berg <johannes@sipsolutions.net>
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

#include "parse-events.h"

static int timer_expire_handler(struct trace_seq *s, struct record *record,
				struct event_format *event, void *context)
{
	trace_seq_printf(s, "hrtimer=");

	if (pevent_print_num_field(s, "0x%llx", event, "timer", record, 0) == -1)
		pevent_print_num_field(s, "0x%llx", event, "hrtimer", record, 1);

	trace_seq_printf(s, " now=");

	pevent_print_num_field(s, "%llu", event, "now", record, 1);

	return 0;
}

static int timer_start_handler(struct trace_seq *s, struct record *record,
			       struct event_format *event, void *context)
{
	struct pevent *pevent = event->pevent;
	struct format_field *fn = pevent_find_field(event, "function");
	void *data = record->data;

	trace_seq_printf(s, "hrtimer=");

	if (pevent_print_num_field(s, "0x%llx", event, "timer", record, 0) == -1)
		pevent_print_num_field(s, "0x%llx", event, "hrtimer", record, 1);

	if (!fn) {
		trace_seq_printf(s, " function=MISSING");
	} else {
		unsigned long long function;
		const char *func;

		if (pevent_read_number_field(fn, data, &function))
			trace_seq_printf(s, " function=INVALID");

		func = pevent_find_function(pevent, function);

		trace_seq_printf(s, " function=%s", func);
	}

	trace_seq_printf(s, " expires=");
	pevent_print_num_field(s, "%llu", event, "expires", record, 1);

	trace_seq_printf(s, " softexpires=");
	pevent_print_num_field(s, "%llu", event, "softexpires", record, 1);

	return 0;
}

int PEVENT_PLUGIN_LOADER(struct pevent *pevent)
{
	pevent_register_event_handler(pevent, -1, "timer", "hrtimer_expire_entry",
				      timer_expire_handler, NULL);

	pevent_register_event_handler(pevent, -1, "timer", "hrtimer_start",
				      timer_start_handler, NULL);

	return 0;
}
