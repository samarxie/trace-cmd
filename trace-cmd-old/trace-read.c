/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "trace-local.h"
#include "trace-hash-local.h"
#include "list.h"

static struct filter {
	struct filter		*next;
	const char		*filter;
	int			neg;
} *filter_strings;
static struct filter **filter_next = &filter_strings;

struct handle_list {
	struct list_head	list;
	struct tracecmd_input	*handle;
	const char		*file;
	int			cpus;
	int			done;
	struct record		*record;
	struct event_filter	*event_filters;
	struct event_filter	*event_filter_out;
};
static struct list_head handle_list;

struct input_files {
	struct list_head	list;
	const char		*file;
};
static struct list_head input_files;

static unsigned int page_size;
static int input_fd;
static const char *default_input_file = "trace.dat";
static const char *input_file;
static int multi_inputs;
static int max_file_size;

static int *filter_cpus;
static int nr_filter_cpus;

static int show_wakeup;
static int wakeup_id;
static int wakeup_new_id;
static int sched_id;

static struct format_field *wakeup_task;
static struct format_field *wakeup_success;
static struct format_field *wakeup_new_task;
static struct format_field *wakeup_new_success;
static struct format_field *sched_task;

static unsigned long long total_wakeup_lat;
static unsigned long wakeup_lat_count;

struct wakeup_info {
	struct wakeup_info	*next;
	unsigned long long	start;
	int			pid;
};

#define WAKEUP_HASH_SIZE 1024
static struct wakeup_info *wakeup_hash[WAKEUP_HASH_SIZE];

/* Debug variables for testing tracecmd_read_at */
#define TEST_READ_AT 0
#if TEST_READ_AT
#define DO_TEST
static off64_t test_read_at_offset;
static int test_read_at_copy = 100;
static int test_read_at_index;
static void show_test(struct tracecmd_input *handle)
{
	struct pevent *pevent;
	struct record *record;
	struct trace_seq s;
	int cpu;

	if (!test_read_at_offset) {
		printf("\nNO RECORD COPIED\n");
		return;
	}

	pevent = tracecmd_get_pevent(handle);

	record = tracecmd_read_at(handle, test_read_at_offset, &cpu);
	printf("\nHERE'S THE COPY RECORD\n");
	trace_seq_init(&s);
	pevent_print_event(pevent, &s, cpu, record->data, record->size, record->ts);
	trace_seq_do_printf(&s);
	trace_seq_destroy(&s);
	printf("\n");

	free_record(record);
}

static void test_save(struct record *record, int cpu)
{
	if (test_read_at_index++ == test_read_at_copy) {
		test_read_at_offset = record->offset;
		printf("\nUSING THIS RECORD\n");
	}
}
#endif /* TEST_READ_AT */

/* Debug variables for testing tracecmd_set_cpu_at_timestamp */
#define TEST_AT_TIMESTAMP 0
#if TEST_AT_TIMESTAMP
#define DO_TEST
static unsigned long long test_at_timestamp_ts;
static int test_at_timestamp_copy = 100;
static int test_at_timestamp_cpu = -1;
static int test_at_timestamp_index;
static void show_test(struct tracecmd_input *handle)
{
	struct pevent *pevent;
	struct record *record;
	struct trace_seq s;
	int cpu = test_at_timestamp_cpu;

	if (!test_at_timestamp_ts) {
		printf("\nNO RECORD COPIED\n");
		return;
	}

	pevent = tracecmd_get_pevent(handle);

	if (tracecmd_set_cpu_to_timestamp(handle, cpu, test_at_timestamp_ts))
		return;

	record = tracecmd_read_data(handle, cpu);
	printf("\nHERE'S THE COPY RECORD with page %p offset=%p\n",
	       (void *)(record->offset & ~(page_size - 1)),
	       (void *)record->offset);
	trace_seq_init(&s);
	pevent_print_event(pevent, &s, cpu, record->data, record->size, record->ts);
	trace_seq_do_printf(&s);
	trace_seq_destroy(&s);
	printf("\n");

	free_record(record);
}

static void test_save(struct record *record, int cpu)
{
	if (test_at_timestamp_index++ == test_at_timestamp_copy) {
		test_at_timestamp_ts = record->ts;
		test_at_timestamp_cpu = cpu;
		printf("\nUSING THIS RECORD page=%p offset=%p\n",
		       (void *)(record->offset & ~(page_size - 1)),
		       (void *)record->offset);
	}
}
#endif /* TEST_AT_TIMESTAMP */

#define TEST_FIRST_LAST 0
#if TEST_FIRST_LAST
#define DO_TEST
static void show_test(struct tracecmd_input *handle)
{
	struct pevent *pevent;
	struct record *record;
	struct trace_seq s;
	int cpu = 0;

	pevent = tracecmd_get_pevent(handle);

	record = tracecmd_read_cpu_first(handle, cpu);
	if (!record) {
		printf("No first record?\n");
		return;
	}

	printf("\nHERE'S THE FIRST RECORD with offset %p\n",
	       (void *)record->offset);
	trace_seq_init(&s);
	pevent_print_event(pevent, &s, cpu, record->data, record->size, record->ts);
	trace_seq_do_printf(&s);
	trace_seq_destroy(&s);
	printf("\n");

	free_record(record);

	record = tracecmd_read_cpu_last(handle, cpu);
	if (!record) {
		printf("No last record?\n");
		return;
	}

	printf("\nHERE'S THE LAST RECORD with offset %p\n",
	       (void *)record->offset);
	trace_seq_init(&s);
	pevent_print_event(pevent, &s, cpu, record->data, record->size, record->ts);
	trace_seq_do_printf(&s);
	trace_seq_destroy(&s);
	printf("\n");

	free_record(record);
}
static void test_save(struct record *record, int cpu)
{
}
#endif /* TEST_FIRST_LAST */

#ifndef DO_TEST
static void show_test(struct tracecmd_input *handle)
{
}
static void test_save(struct record *record, int cpu)
{
}
#endif

static void add_input(const char *file)
{
	struct input_files *item;

	item = malloc_or_die(sizeof(*item));
	item->file = file;
	list_add_tail(&item->list, &input_files);
}

static void add_handle(struct tracecmd_input *handle, const char *file)
{
	struct handle_list *item;

	item = malloc_or_die(sizeof(*item));
	memset(item, 0, sizeof(*item));
	item->handle = handle;
	item->file = file + strlen(file);
	/* we want just the base name */
	while (*item->file != '/' && item->file >= file)
		item->file--;
	item->file++;
	if (strlen(item->file) > max_file_size)
		max_file_size = strlen(item->file);

	list_add_tail(&item->list, &handle_list);
}

static void free_inputs(void)
{
	struct input_files *item;

	while (!list_empty(&input_files)) {
		item = container_of(input_files.next, struct input_files, list);
		list_del(&item->list);
		free(item);
	}
}

static void free_handles(void)
{
	struct handle_list *item;

	while (!list_empty(&handle_list)) {
		item = container_of(handle_list.next, struct handle_list, list);
		list_del(&item->list);
		free(item);
	}
}

static void add_filter(const char *filter, int neg)
{
	struct filter *ftr;

	ftr = malloc_or_die(sizeof(*ftr));
	ftr->filter = filter;
	ftr->next = NULL;
	ftr->neg = neg;

	/* must maintain order of command line */
	*filter_next = ftr;
	filter_next = &ftr->next;
}

static void process_filters(struct handle_list *handles)
{
	struct event_filter *event_filter;
	struct pevent *pevent;
	struct filter *filter;
	char *errstr;
	int ret;

	pevent = tracecmd_get_pevent(handles->handle);
	handles->event_filters = pevent_filter_alloc(pevent);
	handles->event_filter_out = pevent_filter_alloc(pevent);

	while (filter_strings) {
		filter = filter_strings;
		filter_strings = filter->next;
		if (filter->neg)
			event_filter = handles->event_filter_out;
		else
			event_filter = handles->event_filters;

		ret = pevent_filter_add_filter_str(event_filter,
						   filter->filter,
						   &errstr);
		if (ret < 0)
			die("Error filtering: %s\n%s",
			    filter->filter, errstr);
		free(errstr);
		free(filter);
	}
}

static int filter_record(struct tracecmd_input *handle,
			 struct record *record)
{
	return 0;
}

static void init_wakeup(struct tracecmd_input *handle)
{
	struct event_format *event;
	struct pevent *pevent;

	if (!show_wakeup)
		return;

	pevent = tracecmd_get_pevent(handle);

	event = pevent_find_event_by_name(pevent, "sched", "sched_wakeup");
	if (!event)
		goto fail;
	wakeup_id = event->id;
	wakeup_task = pevent_find_field(event, "pid");
	if (!wakeup_task)
		goto fail;
	wakeup_success = pevent_find_field(event, "success");

	event = pevent_find_event_by_name(pevent, "sched", "sched_switch");
	if (!event)
		goto fail;
	sched_id = event->id;
	sched_task = pevent_find_field(event, "next_pid");
	if (!sched_task)
		goto fail;


	wakeup_new_id = -1;

	event = pevent_find_event_by_name(pevent, "sched", "sched_wakeup_new");
	if (!event)
		goto skip;
	wakeup_new_id = event->id;
	wakeup_new_task = pevent_find_field(event, "pid");
	if (!wakeup_new_task)
		goto fail;
	wakeup_new_success = pevent_find_field(event, "success");

 skip:
	return;

 fail:
	show_wakeup = 0;
}

static unsigned int calc_wakeup_key(unsigned long val)
{
	return trace_hash(val) % WAKEUP_HASH_SIZE;
}

static struct wakeup_info *
__find_wakeup(unsigned int key, unsigned int val)
{
	struct wakeup_info *info = wakeup_hash[key];

	while (info) {
		if (info->pid == val)
			return info;
		info = info->next;
	}

	return NULL;
}

static void add_wakeup(unsigned int val, unsigned long long start)
{
	unsigned int key = calc_wakeup_key(val);
	struct wakeup_info *info;

	info = __find_wakeup(key, val);
	if (info) {
		/* Hmm, double wakeup? */
		info->start = start;
		return;
	}

	info = malloc_or_die(sizeof(*info));
	info->pid = val;
	info->start = start;
	info->next = wakeup_hash[key];
	wakeup_hash[key] = info;
}

static unsigned long long max_lat = 0;
static unsigned long long max_time;
static unsigned long long min_lat = -1;
static unsigned long long min_time;

static void add_sched(unsigned int val, unsigned long long end)
{
	unsigned int key = calc_wakeup_key(val);
	struct wakeup_info *info;
	struct wakeup_info **next;
	unsigned long long cal;

	info = __find_wakeup(key, val);
	if (!info)
		return;

	cal = end - info->start;

	if (cal > max_lat) {
		max_lat = cal;
		max_time = end;
	}
	if (cal < min_lat) {
		min_lat = cal;
		min_time = end;
	}

	printf(" Latency: %llu.%03llu usecs", cal / 1000, cal % 1000);

	total_wakeup_lat += cal;
	wakeup_lat_count++;

	next = &wakeup_hash[key];
	while (*next) {
		if (*next == info) {
			*next = info->next;
			break;
		}
		next = &(*next)->next;
	}
	free(info);
}

static void process_wakeup(struct pevent *pevent, struct record *record)
{
	unsigned long long val;
	int id;

	if (!show_wakeup)
		return;

	id = pevent_data_type(pevent, record);
	if (id == wakeup_id) {
		if (pevent_read_number_field(wakeup_success, record->data, &val) == 0) {
			if (!val)
				return;
		}
		if (pevent_read_number_field(wakeup_task, record->data, &val))
			return;
		add_wakeup(val, record->ts);
	} else if (id == wakeup_new_id) {
		if (pevent_read_number_field(wakeup_new_success, record->data, &val) == 0) {
			if (!val)
				return;
		}
		if (pevent_read_number_field(wakeup_new_task, record->data, &val))
			return;
		add_wakeup(val, record->ts);
	} else if (id == sched_id) {
		if (pevent_read_number_field(sched_task, record->data, &val))
			return;
		add_sched(val, record->ts);
	}
}

static void finish_wakeup(void)
{
	struct wakeup_info *info;
	int i;

	if (!show_wakeup || !wakeup_lat_count)
		return;

	total_wakeup_lat /= wakeup_lat_count;

	printf("\nAverage wakeup latency: %llu.%03llu usecs\n",
	       total_wakeup_lat / 1000,
	       total_wakeup_lat % 1000);
	printf("Maximum Latency: %llu.%03llu usecs at ", max_lat / 1000, max_lat % 1000);
	printf("timestamp: %llu.%06llu\n",
	       max_time / 1000000000, ((max_time + 500) % 1000000000) / 1000);
	printf("Minimum Latency: %llu.%03llu usecs at ", min_lat / 1000, min_lat % 1000);
	printf("timestamp: %llu.%06llu\n\n", min_time / 1000000000,
	       ((min_time + 500) % 1000000000) / 1000);

	for (i = 0; i < WAKEUP_HASH_SIZE; i++) {
		while (wakeup_hash[i]) {
			info = wakeup_hash[i];
			wakeup_hash[i] = info->next;
			free(info);
		}
	}
}

static void show_data(struct tracecmd_input *handle,
		      struct record *record, int cpu)
{
	struct pevent *pevent;
	struct trace_seq s;

	if (filter_record(handle, record))
		return;

	pevent = tracecmd_get_pevent(handle);

	test_save(record, cpu);

	trace_seq_init(&s);
	if (record->missed_events > 0)
		trace_seq_printf(&s, "CPU:%d [%lld EVENTS DROPPED]\n",
				 record->cpu, record->missed_events);
	else if (record->missed_events < 0)
		trace_seq_printf(&s, "CPU:%d [EVENTS DROPPED]\n",
				 record->cpu);
	pevent_print_event(pevent, &s, record);
	if (s.len && *(s.buffer + s.len - 1) == '\n')
		s.len--;
	trace_seq_do_printf(&s);
	trace_seq_destroy(&s);

	process_wakeup(pevent, record);

	printf("\n");
}

static void read_rest(void)
{
	char buf[BUFSIZ + 1];
	int r;

	do {
		r = read(input_fd, buf, BUFSIZ);
		if (r > 0) {
			buf[r] = 0;
			printf("%s", buf);
		}
	} while (r > 0);
}

static struct record *
get_next_record(struct handle_list *handles, int *next_cpu)
{
	unsigned long long ts;
	struct record *record;
	int found = 0;
	int next;
	int cpu;
	int ret;

	if (handles->record)
		return handles->record;

	if (handles->done)
		return NULL;

	do {
		next = -1;
		ts = 0;
		if (filter_cpus) {
			unsigned long long last_stamp = 0;
			struct record *precord;
			int next_cpu = -1;
			int i;

			for (i = 0; (cpu = filter_cpus[i]) >= 0; i++) {
				precord = tracecmd_peek_data(handles->handle, cpu);
				if (precord &&
				    (!last_stamp || precord->ts < last_stamp)) {
					next_cpu = cpu;
					last_stamp = precord->ts;
				}
			}
			if (last_stamp)
				record = tracecmd_read_data(handles->handle, next_cpu);
			else
				record = NULL;
		} else
			record = tracecmd_read_next_data(handles->handle, &cpu);

		if (record) {
			ret = pevent_filter_match(handles->event_filters, record);
			switch (ret) {
			case FILTER_NONE:
			case FILTER_MATCH:
				ret = pevent_filter_match(handles->event_filter_out, record);
				if (ret != FILTER_MATCH) {
					found = 1;
					break;
				}
				free_record(record);
			}
		}
	} while (record && !found);

	handles->record = record;
	if (!record)
		handles->done = 1;
	*next_cpu = next;

	return record;
}

static void free_handle_record(struct handle_list *handles)
{
	if (!handles->record)
		return;

	free_record(handles->record);
	handles->record = NULL;
}

static void print_handle_file(struct handle_list *handles)
{
	/* Only print file names if more than one file is read */
	if (!multi_inputs)
		return;
	printf("%*s: ", max_file_size, handles->file);
}

static void read_data_info(struct list_head *handle_list)
{
	struct handle_list *handles;
	struct handle_list *last_handle;
	struct record *record;
	struct record *last_record;
	int last_cpu;
	int cpus;
	int next;
	int ret;

	list_for_each_entry(handles, handle_list, list) {

		ret = tracecmd_init_data(handles->handle);
		if (ret < 0)
			die("failed to init data");

		cpus = tracecmd_cpus(handles->handle);
		handles->cpus = cpus;
		print_handle_file(handles);
		printf("cpus=%d\n", cpus);

		/* Latency trace is just all ASCII */
		if (ret > 0) {
			if (multi_inputs)
				die("latency traces do not work with multiple inputs");
			read_rest();
			return;
		}

		init_wakeup(handles->handle);
		process_filters(handles);
	}

	do {
		last_handle = NULL;
		last_record = NULL;

		list_for_each_entry(handles, handle_list, list) {
			record = get_next_record(handles, &next);
			if (!last_record ||
			    (record && record->ts < last_record->ts)) {
				last_record = record;
				last_handle = handles;
				last_cpu = next;
			}
		}
		if (last_record) {
			print_handle_file(last_handle);
			show_data(last_handle->handle, last_record, last_cpu);
			free_handle_record(last_handle);
		}
	} while (last_record);

	list_for_each_entry(handles, handle_list, list) {
		pevent_filter_free(handles->event_filters);
		pevent_filter_free(handles->event_filter_out);

		show_test(handles->handle);
	}
}

struct tracecmd_input *read_trace_header(const char *file)
{
	input_fd = open(file, O_RDONLY);
	if (input_fd < 0)
		die("opening '%s'\n", file);

	return tracecmd_alloc_fd(input_fd);
}

static void sig_end(int sig)
{
	fprintf(stderr, "trace-cmd: Received SIGINT\n");
	exit(0);
}

static const char *skip_space_and_test_digit(const char *p, const char *cpu_str)
{
	while (isspace(*p))
		p++;
	if (!isdigit(*p))
		die("invalid character '%c' in cpu string '%s'",
		    *p, cpu_str);
	return p;
}

static void __add_cpu(int cpu)
{
	filter_cpus = tracecmd_add_id(filter_cpus, cpu, nr_filter_cpus++);
}

static void parse_cpulist(const char *cpu_str)
{
	unsigned a, b;
	const char *s = cpu_str;

	do {
		s = skip_space_and_test_digit(s, cpu_str);
		b = a = strtoul(s, (char **)&s, 10);
		if (*s == '-') {
			s = skip_space_and_test_digit(s + 1, cpu_str);
			b = strtoul(s, (char **)&s, 10);
		}
		if (!(a <= b))
			die("range of cpu numbers must be lower to greater");
		while (a <= b) {
			__add_cpu(a);
			a++;
		}
		if (*s == ',' || *s == ':')
			s++;
	} while (*s != '\0');
}

static void read_file_fd(int fd, char *dst, int len)
{
	size_t size = 0;
	int r;

	do {
		r = read(fd, dst+size, len);
		if (r > 0) {
			size += r;
			len -= r;
		}
	} while (r > 0);
}

static void add_functions(struct pevent *pevent, const char *file)
{
	struct stat st;
	char *buf;
	int ret;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		die("Can't read file %s", file);

	ret = fstat(fd, &st);
	if (ret < 0)
		die("Can't stat file %s", file);

	buf = malloc_or_die(st.st_size);
	read_file_fd(fd, buf, st.st_size);
	close(fd);
	parse_proc_kallsyms(pevent, buf, st.st_size);
	free(buf);
}

static void process_plugin_option(char *option)
{
	char *name = option;
	char *val = NULL;
	char *p;

	if ((p = strstr(name, "="))) {
		*p = '\0';
		val = p+1;
	}
	trace_util_add_option(name, val);
}

enum {
	OPT_nodate	= 251,
	OPT_check_event_parsing	= 252,
	OPT_kallsyms	= 253,
	OPT_events	= 254,
	OPT_cpu		= 255,
};

void trace_report (int argc, char **argv)
{
	struct tracecmd_input *handle;
	struct pevent *pevent;
	const char *functions = NULL;
	struct input_files *inputs;
	struct handle_list *handles;
	int show_funcs = 0;
	int show_endian = 0;
	int show_page_size = 0;
	int show_printk = 0;
	int latency_format = 0;
	int show_events = 0;
	int print_events = 0;
	int test_filters = 0;
	int no_date = 0;
	int raw = 0;
	int neg = 0;
	int ret = 0;
	int check_event_parsing = 0;
	int c;

	list_head_init(&handle_list);
	list_head_init(&input_files);

	if (argc < 2)
		usage(argv);

	if (strcmp(argv[1], "report") != 0)
		usage(argv);

	signal(SIGINT, sig_end);

	for (;;) {
		int option_index = 0;
		static struct option long_options[] = {
			{"cpu", required_argument, NULL, OPT_cpu},
			{"events", no_argument, NULL, OPT_events},
			{"filter-test", no_argument, NULL, 'T'},
			{"kallsyms", required_argument, NULL, OPT_kallsyms},
			{"check-events", no_argument, NULL,
				OPT_check_event_parsing},
			{"nodate", no_argument, NULL, OPT_nodate},
			{"help", no_argument, NULL, '?'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc-1, argv+1, "+hi:feprPNLlEwF:VvTqO:",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			usage(argv);
			break;
		case 'i':
			if (input_file) {
				if (!multi_inputs)
					add_input(input_file);
				multi_inputs++;
				add_input(optarg);
			} else
				input_file = optarg;
			break;
		case 'F':
			add_filter(optarg, neg);
			break;
		case 'T':
			test_filters = 1;
			break;
		case 'f':
			show_funcs = 1;
			break;
		case 'P':
			show_printk = 1;
			break;
		case 'L':
			tracecmd_disable_sys_plugins = 1;
			break;
		case 'N':
			tracecmd_disable_plugins = 1;
			break;
		case 'e':
			show_endian = 1;
			break;
		case 'p':
			show_page_size = 1;
			break;
		case 'E':
			show_events = 1;
			break;
		case 'r':
			raw = 1;
			break;
		case 'w':
			show_wakeup = 1;
			break;
		case 'l':
			latency_format = 1;
			break;
		case 'O':
			process_plugin_option(optarg);
			break;
		case 'v':
			if (neg)
				die("Only 1 -v can be used");
			neg = 1;
			break;
		case 'V':
			show_status = 1;
			break;
		case 'q':
			silence_warnings = 1;
			break;
		case OPT_cpu:
			parse_cpulist(optarg);
			break;
		case OPT_events:
			print_events = 1;
			break;
		case OPT_kallsyms:
			functions = optarg;
			break;
		case OPT_check_event_parsing:
			check_event_parsing = 1;
			break;
		case OPT_nodate:
			no_date = 1;
			break;
		default:
			usage(argv);
		}
	}

	if ((argc - optind) >= 2) {
		if (input_file)
			usage(argv);
		input_file = argv[optind + 1];
	}

	if (!input_file)
		input_file = default_input_file;

	if (!multi_inputs)
		add_input(input_file);
	else if (show_wakeup)
		die("Wakeup tracing can only be done on a single input file");

	list_for_each_entry(inputs, &input_files, list) {
		handle = read_trace_header(inputs->file);
		if (!handle)
			die("error reading header for %s", inputs->file);
		add_handle(handle, inputs->file);

		if (no_date)
			tracecmd_set_flag(handle, TRACECMD_FL_IGNORE_DATE);

		page_size = tracecmd_page_size(handle);

		if (show_page_size) {
			printf("file page size is %d, and host page size is %d\n",
			       page_size,
			       getpagesize());
			return;
		}

		pevent = tracecmd_get_pevent(handle);

		if (raw)
			pevent->print_raw = 1;

		if (test_filters)
			pevent->test_filters = 1;

		if (functions)
			add_functions(pevent, functions);

		if (show_endian) {
			printf("file is %s endian and host is %s endian\n",
			       pevent_is_file_bigendian(pevent) ? "big" : "little",
			       pevent_is_host_bigendian(pevent) ? "big" : "little");
			return;
		}

		if (print_events) {
			tracecmd_print_events(handle);
			return;
		}

		ret = tracecmd_read_headers(handle);
		if (check_event_parsing) {
			if (ret || pevent->parsing_failures)
				exit(EINVAL);
			else
				exit(0);
		} else {
			if (ret)
				return;
		}

		if (show_funcs) {
			pevent_print_funcs(pevent);
			return;
		}
		if (show_printk) {
			pevent_print_printk(pevent);
			return;
		}

		if (show_events) {
			struct event_format **events;
			struct event_format *event;
			int i;

			events = pevent_list_events(pevent, EVENT_SORT_SYSTEM);
			for (i = 0; events[i]; i++) {
				event = events[i];
				if (event->system)
					printf("%s:", event->system);
				printf("%s\n", event->name);
			}
			return;
		}
	}

	if (latency_format)
		pevent_set_latency_format(pevent, latency_format);

	read_data_info(&handle_list);

	list_for_each_entry(handles, &handle_list, list) {
		tracecmd_close(handles->handle);
	}
	free_handles();
	free_inputs();

	finish_wakeup();

	return;
}
