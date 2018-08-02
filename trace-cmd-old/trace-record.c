/*
 * Copyright (C) 2008, 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
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
#define _GNU_SOURCE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#ifndef NO_PTRACE
#include <sys/ptrace.h>
#else
#ifdef WARN_NO_PTRACE
#warning ptrace not supported. -c feature will not work
#endif
#endif
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sched.h>
#include <glob.h>
#include <errno.h>

#include "trace-local.h"

#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

#define TRACE_CTRL	"tracing_on"
#define TRACE		"trace"
#define AVAILABLE	"available_tracers"
#define CURRENT		"current_tracer"
#define ITER_CTRL	"trace_options"
#define MAX_LATENCY	"tracing_max_latency"
#define STAMP		"stamp"
#define FUNC_STACK_TRACE "func_stack_trace"

#define UDP_MAX_PACKET (65536 - 20)

static int tracing_on_init_val;

static int rt_prio;

static int use_tcp;

static unsigned int page_size;

static int buffer_size;

static const char *output_file = "trace.dat";

static int latency;
static int sleep_time = 1000;
static int cpu_count;
static int *pids;

static char *host;
static int *client_ports;
static int sfd;

static int do_ptrace;

static int filter_task;
static int filter_pid = -1;

static int finished;

/* Try a few times to get an accurate date */
static int date2ts_tries = 5;

struct func_list {
	struct func_list *next;
	const char *func;
};

static struct func_list *filter_funcs;
static struct func_list *notrace_funcs;
static struct func_list *graph_funcs;

static int func_stack;

struct filter_pids {
	struct filter_pids *next;
	int pid;
};

static struct filter_pids *filter_pids;
static int nr_filter_pids;
static int len_filter_pids;

struct opt_list {
	struct opt_list *next;
	const char	*option;
};

static struct opt_list *options;

struct event_list {
	struct event_list *next;
	const char *event;
	char *filter;
	char *filter_file;
	char *enable_file;
	int neg;
};

static struct event_list *sched_switch_event;
static struct event_list *sched_wakeup_event;
static struct event_list *sched_wakeup_new_event;
static struct event_list *sched_event;

static struct event_list *event_selection;
struct tracecmd_event_list *listed_events;

struct events {
	struct events *sibling;
	struct events *children;
	struct events *next;
	char *name;
};

static struct tracecmd_recorder *recorder;

static int ignore_event_not_found = 0;

static char *get_temp_file(int cpu)
{
	char *file = NULL;
	int size;

	size = snprintf(file, 0, "%s.cpu%d", output_file, cpu);
	file = malloc_or_die(size + 1);
	sprintf(file, "%s.cpu%d", output_file, cpu);

	return file;
}

static void put_temp_file(char *file)
{
	free(file);
}

static void delete_temp_file(int cpu)
{
	char file[MAX_PATH];

	snprintf(file, MAX_PATH, "%s.cpu%d", output_file, cpu);
	unlink(file);
}

static void kill_threads(void)
{
	int i;

	if (!cpu_count || !pids)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGKILL);
			delete_temp_file(i);
			pids[i] = 0;
		}
	}
}

void die(const char *fmt, ...)
{
	va_list ap;
	int ret = errno;

	if (errno)
		perror("trace-cmd");
	else
		ret = -1;

	kill_threads();
	va_start(ap, fmt);
	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
	exit(ret);
}

static void delete_thread_data(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids) {
			if (pids[i]) {
				delete_temp_file(i);
				if (pids[i] < 0)
					pids[i] = 0;
			}
		} else
			/* Extract does not allocate pids */
			delete_temp_file(i);
	}
}

static void stop_threads(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGINT);
			waitpid(pids[i], NULL, 0);
			pids[i] = -1;
		}
	}
}

static int create_recorder(int cpu, int extract);

static void flush_threads(void)
{
	long ret;
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		ret = create_recorder(i, 1);
		if (ret < 0)
			die("error reading ring buffer");
	}
}

static int set_ftrace(int set)
{
	struct stat buf;
	char *path = "/proc/sys/kernel/ftrace_enabled";
	int fd;
	char *val = set ? "1" : "0";

	/* if ftace_enable does not exist, simply ignore it */
	fd = stat(path, &buf);
	if (fd < 0)
		return -ENODEV;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		die ("Can't %s ftrace", set ? "enable" : "disable");

	write(fd, val, 1);
	close(fd);

	return 0;
}

static void clear_trace(void)
{
	FILE *fp;
	char *path;

	/* reset the trace */
	path = tracecmd_get_tracing_file("trace");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	tracecmd_put_tracing_file(path);
	fwrite("0", 1, 1, fp);
	fclose(fp);
}

static void reset_max_latency(void)
{
	FILE *fp;
	char *path;

	/* reset the trace */
	path = tracecmd_get_tracing_file("tracing_max_latency");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	tracecmd_put_tracing_file(path);
	fwrite("0", 1, 1, fp);
	fclose(fp);
}

static void add_filter_pid(int pid)
{
	struct filter_pids *p;
	char buf[100];

	p = malloc_or_die(sizeof(*p));
	p->next = filter_pids;
	p->pid = pid;
	filter_pids = p;
	nr_filter_pids++;

	len_filter_pids += sprintf(buf, "%d", pid);
}

static void update_ftrace_pid(const char *pid, int reset)
{
	static char *path;
	int ret;
	static int fd = -1;

	if (!pid) {
		if (fd >= 0)
			close(fd);
		if (path)
			tracecmd_put_tracing_file(path);
		fd = -1;
		path = NULL;
		return;
	}

	/* Force reopen on reset */
	if (reset && fd >= 0) {
		close(fd);
		fd = -1;
	}

	if (fd < 0) {
		if (!path)
			path = tracecmd_get_tracing_file("set_ftrace_pid");
		if (!path)
			return;
		fd = open(path, O_WRONLY | O_CLOEXEC | (reset ? O_TRUNC : 0));
		if (fd < 0)
			return;
	}

	ret = write(fd, pid, strlen(pid));

	/*
	 * Older kernels required "-1" to disable pid
	 */
	if (ret < 0 && !strlen(pid))
		ret = write(fd, "-1", 2);

	if (ret < 0)
		die("error writing to %s", path);

	/* add whitespace in case another pid is written */
	write(fd, " ", 1);
}

static void update_event_filters(const char *pid_filter);
static void update_pid_event_filters(const char *pid);
static void enable_tracing(void);
static void
update_sched_event(struct event_list **event, const char *file,
		   const char *pid_filter, const char *field_filter);

static void update_task_filter(void)
{
	int pid = getpid();
	char spid[100];

	if (!filter_task && filter_pid < 0) {
		update_ftrace_pid("", 1);
		enable_tracing();
		return;
	}

	if (filter_pid >= 0)
		pid = filter_pid;

	snprintf(spid, 100, "%d", pid);

	update_ftrace_pid(spid, 1);

	update_pid_event_filters(spid);

	enable_tracing();
}

#ifndef NO_PTRACE
static char *make_pid_filter(const char *field)
{
	struct filter_pids *p;
	char *filter;
	char *orit;
	char *str;
	int len;

	filter = malloc_or_die(len_filter_pids +
		       (strlen(field) + strlen("(==)||")) * nr_filter_pids);
	/* Last '||' that is not used will cover the \0 */
	str = filter;

	for (p = filter_pids; p; p = p->next) {
		if (p == filter_pids)
			orit = "";
		else
			orit = "||";
		len = sprintf(str, "%s(%s==%d)", orit, field, p->pid);
		str += len;
	}

	return filter;
}

static void add_new_filter_pid(int pid)
{
	char *pid_filter;
	char *filter;
	char buf[100];

	add_filter_pid(pid);
	sprintf(buf, "%d", pid);
	update_ftrace_pid(buf, 0);

	pid_filter = make_pid_filter("common_pid");
	update_event_filters(pid_filter);

	if (!sched_event && !sched_switch_event
	    && !sched_wakeup_event && !sched_wakeup_new_event)
		return;

	/*
	 * Also make sure that the sched_switch to this pid
	 * and wakeups of this pid are also traced.
	 * Only need to do this if the events are active.
	 */
	filter = make_pid_filter("next_pid");
	update_sched_event(&sched_switch_event, "sched/sched_switch", pid_filter, filter);
	free(filter);

	filter = make_pid_filter("pid");
	update_sched_event(&sched_wakeup_event, "sched/sched_wakeup",
			   pid_filter, filter);
	update_sched_event(&sched_wakeup_new_event, "sched/sched_wakeup_new",
			   pid_filter, filter);
	free(pid_filter);
	free(filter);
}

static void ptrace_attach(int pid)
{
	int ret;

	ret = ptrace(PTRACE_ATTACH, pid, NULL, 0);
	if (ret < 0) {
		warning("Unable to trace process %d children", pid);
		do_ptrace = 0;
		return;
	}
	add_filter_pid(pid);
}

static void enable_ptrace(void)
{
	if (!do_ptrace || !filter_task)
		return;

	ptrace(PTRACE_TRACEME, 0, NULL, 0);
}

static void ptrace_wait(int main_pid)
{
	unsigned long send_sig;
	unsigned long child;
	siginfo_t sig;
	int cstatus;
	int status;
	int event;
	int pid;
	int ret;

	do {
		ret = waitpid(-1, &status, WSTOPPED | __WALL);
		if (ret < 0)
			continue;

		pid = ret;

		if (WIFSTOPPED(status)) {
			event = (status >> 16) & 0xff;
			ptrace(PTRACE_GETSIGINFO, pid, NULL, &sig);
			send_sig = sig.si_signo;
			/* Don't send ptrace sigs to child */
			if (send_sig == SIGTRAP || send_sig == SIGSTOP)
				send_sig = 0;
			switch (event) {
			case PTRACE_EVENT_FORK:
			case PTRACE_EVENT_VFORK:
			case PTRACE_EVENT_CLONE:
				/* forked a child */
				ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child);
				ptrace(PTRACE_SETOPTIONS, child, NULL,
				       PTRACE_O_TRACEFORK |
				       PTRACE_O_TRACEVFORK |
				       PTRACE_O_TRACECLONE |
				       PTRACE_O_TRACEEXIT);
				add_new_filter_pid(child);
				ptrace(PTRACE_CONT, child, NULL, 0);
				break;

			case PTRACE_EVENT_EXIT:
				ptrace(PTRACE_GETEVENTMSG, pid, NULL, &cstatus);
				ptrace(PTRACE_DETACH, pid, NULL, NULL);
				break;
			}
			ptrace(PTRACE_SETOPTIONS, pid, NULL,
			       PTRACE_O_TRACEFORK |
			       PTRACE_O_TRACEVFORK |
			       PTRACE_O_TRACECLONE |
			       PTRACE_O_TRACEEXIT);
			ptrace(PTRACE_CONT, pid, NULL, send_sig);
		}
	} while (!finished && ret > 0 &&
		 (!WIFEXITED(status) || pid != main_pid));
}
#else
static inline void ptrace_wait(int main_pid) { }
static inline void enable_ptrace(void) { }
static inline void ptrace_attach(int pid) { }

#endif /* NO_PTRACE */

void trace_or_sleep(void)
{
	if (do_ptrace && filter_pid >= 0)
		ptrace_wait(filter_pid);
	else
		sleep(10);
}

void run_cmd(int argc, char **argv)
{
	int status;
	int pid;

	if ((pid = fork()) < 0)
		die("failed to fork");
	if (!pid) {
		/* child */
		update_task_filter();
		enable_ptrace();
		if (execvp(argv[0], argv)) {
			fprintf(stderr, "\n********************\n");
			fprintf(stderr, " Unable to exec %s\n", argv[0]);
			fprintf(stderr, "********************\n");
			die("Failed to exec %s", argv[0]);
		}
	}
	if (do_ptrace) {
		add_filter_pid(pid);
		ptrace_wait(pid);
	} else
		waitpid(pid, &status, 0);
}

static void set_plugin(const char *name)
{
	FILE *fp;
	char *path;
	char zero = '0';

	path = tracecmd_get_tracing_file("current_tracer");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	tracecmd_put_tracing_file(path);

	fwrite(name, 1, strlen(name), fp);
	fclose(fp);

	if (strncmp(name, "function", 8) != 0)
		return;

	/* Make sure func_stack_trace option is disabled */
	path = tracecmd_get_tracing_file("options/func_stack_trace");
	fp = fopen(path, "w");
	tracecmd_put_tracing_file(path);
	if (!fp)
		return;
	fwrite(&zero, 1, 1, fp);
	fclose(fp);
}

static void save_option(const char *option)
{
	struct opt_list *opt;

	opt = malloc_or_die(sizeof(*opt));
	opt->next = options;
	options = opt;
	opt->option = option;
}

static void set_option(const char *option)
{
	FILE *fp;
	char *path;

	path = tracecmd_get_tracing_file("trace_options");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	tracecmd_put_tracing_file(path);

	fwrite(option, 1, strlen(option), fp);
	fclose(fp);
}

static void set_options(void)
{
	struct opt_list *opt;

	while (options) {
		opt = options;
		options = opt->next;
		set_option(opt->option);
		free(opt);
	}
}

static int use_old_event_method(void)
{
	static int old_event_method;
	static int processed;
	struct stat st;
	char *path;
	int ret;

	if (processed)
		return old_event_method;

	/* Check if the kernel has the events/enable file */
	path = tracecmd_get_tracing_file("events/enable");
	ret = stat(path, &st);
	tracecmd_put_tracing_file(path);
	if (ret < 0)
		old_event_method = 1;

	processed = 1;

	return old_event_method;
}

static void old_update_events(const char *name, char update)
{
	char *path;
	FILE *fp;
	int ret;

	if (strcmp(name, "all") == 0)
		name = "*:*";

	/* need to use old way */
	path = tracecmd_get_tracing_file("set_event");
	fp = fopen(path, "w");
	if (!fp)
		die("opening '%s'", path);
	tracecmd_put_tracing_file(path);

	/* Disable the event with "!" */
	if (update == '0')
		fwrite("!", 1, 1, fp);

	ret = fwrite(name, 1, strlen(name), fp);
	if (ret < 0)
		die("bad event '%s'", name);

	ret = fwrite("\n", 1, 1, fp);
	if (ret < 0)
		die("bad event '%s'", name);

	fclose(fp);

	return;
}

static void reset_events()
{
	glob_t globbuf;
	char *path;
	char c;
	int fd;
	int i;
	int ret;

	if (use_old_event_method()) {
		old_update_events("all", '0');
		return;
	}

	c = '0';
	path = tracecmd_get_tracing_file("events/enable");
	fd = open(path, O_WRONLY);
	if (fd < 0)
		die("opening to '%s'", path);
	ret = write(fd, &c, 1);
	close(fd);
	tracecmd_put_tracing_file(path);

	path = tracecmd_get_tracing_file("events/*/filter");
	globbuf.gl_offs = 0;
	ret = glob(path, 0, NULL, &globbuf);
	tracecmd_put_tracing_file(path);
	if (ret < 0)
		return;

	for (i = 0; i < globbuf.gl_pathc; i++) {
		path = globbuf.gl_pathv[i];
		fd = open(path, O_WRONLY);
		if (fd < 0)
			die("opening to '%s'", path);
		ret = write(fd, &c, 1);
		close(fd);
	}
	globfree(&globbuf);
}

static void write_filter(const char *file, const char *filter)
{
	char buf[BUFSIZ];
	int fd;
	int ret;

	fd = open(file, O_WRONLY);
	if (fd < 0)
		die("opening to '%s'", file);
	ret = write(fd, filter, strlen(filter));
	close(fd);
	if (ret < 0) {
		/* filter failed */
		fd = open(file, O_RDONLY);
		if (fd < 0)
			die("writing to '%s'", file);
		/* the filter has the error */
		while ((ret = read(fd, buf, BUFSIZ)) > 0)
			fprintf(stderr, "%.*s", ret, buf);
		die("Failed filter of %s\n", file);
		close(fd);
	}
}

static void
update_event(struct event_list *event, const char *filter,
	     int filter_only, char update)
{
	const char *name = event->event;
	FILE *fp;
	char *path;
	int ret;

	if (use_old_event_method()) {
		if (filter_only)
			return;
		old_update_events(name, update);
		return;
	}

	if (filter && event->filter_file)
		write_filter(event->filter_file, filter);

	if (filter_only || !event->enable_file)
		return;

	path = event->enable_file;

	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	ret = fwrite(&update, 1, 1, fp);
	fclose(fp);
	if (ret < 0)
		die("writing to '%s'", path);
}

/*
 * The debugfs file tracing_enabled needs to be deprecated.
 * But just in case anyone fiddled with it. If it exists,
 * make sure it is one.
 * No error checking needed here.
 */
static void check_tracing_enabled(void)
{
	static int fd = -1;
	char *path;

	if (fd < 0) {
		path = tracecmd_get_tracing_file("tracing_enabled");
		fd = open(path, O_WRONLY | O_CLOEXEC);
		tracecmd_put_tracing_file(path);

		if (fd < 0)
			return;
	}
	write(fd, "1", 1);
}

static int tracing_on_fd = -1;

static int open_tracing_on(void)
{
	int fd = tracing_on_fd;
	char *path;

	if (fd >= 0)
		return fd;

	path = tracecmd_get_tracing_file("tracing_on");
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("opening '%s'", path);
	tracecmd_put_tracing_file(path);
	tracing_on_fd = fd;

	return fd;
}

static void write_tracing_on(int on)
{
	int ret;
	int fd;

	fd = open_tracing_on();
	if (fd < 0)
		return;

	if (on)
		ret = write(fd, "1", 1);
	else
		ret = write(fd, "0", 1);

	if (ret < 0)
		die("writing 'tracing_on'");
}

static int read_tracing_on(void)
{
	int fd;
	char buf[10];
	int ret;

	fd = open_tracing_on();
	if (fd < 0)
		return 0;

	ret = read(fd, buf, 10);
	if (ret <= 0)
		die("Reading 'tracing_on'");
	buf[9] = 0;
	ret = atoi(buf);

	return ret;
}

static void enable_tracing(void)
{
	check_tracing_enabled();

	write_tracing_on(1);

	if (latency)
		reset_max_latency();
}

static void disable_tracing(void)
{
	write_tracing_on(0);
}

static void disable_all(void)
{
	disable_tracing();

	set_plugin("nop");
	reset_events();

	/* Force close and reset of ftrace pid file */
	update_ftrace_pid("", 1);
	update_ftrace_pid(NULL, 0);

	clear_trace();
}

static void
update_sched_event(struct event_list **event, const char *file,
		   const char *pid_filter, const char *field_filter)
{
	char *event_filter;
	char *filter;
	char *path;
	char *p;

	if (!*event) {
		/* No sched events are being processed, ignore */
		if (!sched_event)
			return;
		*event = malloc_or_die(sizeof(**event));
		memset(*event, 0, sizeof(**event));
		(*event)->event = file;
		p = malloc_or_die(strlen(file) + strlen("events//filter") + 1);
		sprintf(p, "events/%s/filter", file);
		path = tracecmd_get_tracing_file(p);
		free(p);
		(*event)->filter_file = strdup(path);
		if (sched_event->filter)
			(*event)->filter = strdup(sched_event->filter);
		tracecmd_put_tracing_file(path);
	}

	path = (*event)->filter_file;
	if (!path)
		return;

	filter = (*event)->filter;

	if (filter) {
		event_filter = malloc_or_die(strlen(pid_filter) +
					     strlen(field_filter) +
					     strlen(filter) +
				       strlen("(()||())&&()") + 1);
		sprintf(event_filter, "((%s)||(%s))&&(%s)",
			pid_filter, field_filter, filter);
	} else {
		event_filter = malloc_or_die(strlen(pid_filter) +
					     strlen(field_filter) +
					     strlen("(()||())") + 1);
		sprintf(event_filter, "((%s)||(%s))",
			pid_filter, field_filter);
	}
	write_filter(path, event_filter);
	free(event_filter);
}

static void update_event_filters(const char *pid_filter)
{
	struct event_list *event;
	char *event_filter;
	int free_it;
	int len;

	len = strlen(pid_filter);
	for (event = event_selection; event; event = event->next) {
		if (!event->neg) {
			free_it = 0;
			if (event->filter) {
				event_filter = malloc_or_die(len +
				     strlen(event->filter) + strlen("()&&()" + 1));
				free_it = 1;
				sprintf(event_filter, "(%s)&&(%s)",
					pid_filter, event->filter);
			} else
				event_filter = (char *)pid_filter;
			update_event(event, event_filter, 1, '1');
			if (free_it)
				free(event_filter);
		}
	}
}

static void update_pid_event_filters(const char *pid)
{
	char *pid_filter;
	char *filter;

	pid_filter = malloc_or_die(strlen(pid) + strlen("common_pid==") + 1);
	sprintf(pid_filter, "common_pid==%s", pid);
	update_event_filters(pid_filter);

	/*
	 * Also make sure that the sched_switch to this pid
	 * and wakeups of this pid are also traced.
	 * Only need to do this if the events are active.
	 */
	filter = malloc_or_die(strlen(pid) + strlen("next_pid==") + 1);
	sprintf(filter, "next_pid==%s", pid);
	update_sched_event(&sched_switch_event, "sched/sched_switch", pid_filter, filter);
	free(filter);

	filter = malloc_or_die(strlen(pid) + strlen("pid==") + 1);
	sprintf(filter, "pid==%s", pid);
	update_sched_event(&sched_wakeup_event, "sched/sched_wakeup",
			   pid_filter, filter);
	update_sched_event(&sched_wakeup_new_event, "sched/sched_wakeup_new",
			   pid_filter, filter);
	free(pid_filter);
	free(filter);
}

static void enable_events(void)
{
	struct event_list *event;

	for (event = event_selection; event; event = event->next) {
		if (!event->neg)
			update_event(event, event->filter, 0, '1');
	}

	/* Now disable any events */
	for (event = event_selection; event; event = event->next) {
		if (event->neg)
			update_event(event, NULL, 0, '0');
	}
}

static void test_event(struct event_list *event, const char *path,
		       const char *name, struct event_list **save, int len)
{
	path += len - strlen(name);

	if (strcmp(path, name) != 0)
		return;

	*save = event;
}

static int expand_event_files(const char *file, struct event_list *old_event)
{
	struct event_list *save_events = event_selection;
	struct event_list *event;
	glob_t globbuf;
	struct stat st;
	char *path;
	char *p;
	int ret;
	int i;

	p = malloc_or_die(strlen(file) + strlen("events//filter") + 1);
	sprintf(p, "events/%s/filter", file);

	path = tracecmd_get_tracing_file(p);
	printf("%s\n", path);

	globbuf.gl_offs = 0;
	ret = glob(path, 0, NULL, &globbuf);
	tracecmd_put_tracing_file(path);
	free(p);

	if (ret < 0)
		die("No filters found");

	for (i = 0; i < globbuf.gl_pathc; i++) {
		int len;

		path = globbuf.gl_pathv[i];

		event = malloc_or_die(sizeof(*event));
		*event = *old_event;
		event->next = event_selection;
		event_selection = event;
		if (event->filter || filter_task || filter_pid) {
			event->filter_file = strdup(path);
			if (!event->filter_file)
				die("malloc filter file");
		}
		for (p = path + strlen(path) - 1; p > path; p--)
			if (*p == '/')
				break;
		*p = '\0';
		p = malloc_or_die(strlen(path) + strlen("/enable") + 1);
		sprintf(p, "%s/enable", path);
		ret = stat(p, &st);
		if (ret >= 0)
			event->enable_file = p;
		else
			free(p);

		len = strlen(path);

		test_event(event, path, "sched/sched_switch", &sched_switch_event, len);
		test_event(event, path, "sched/sched_wakeup_new", &sched_wakeup_new_event, len);
		test_event(event, path, "sched/sched_wakeup", &sched_wakeup_event, len);
		test_event(event, path, "sched", &sched_event, len);
	}
	globfree(&globbuf);

	return save_events == event_selection;
}

static void expand_event(struct event_list *event)
{
	const char *name = event->event;
	char *str;
	char *ptr;
	int len;
	int ret;
	int ret2;

	/*
	 * We allow the user to use "all" to enable all events.
	 * Expand event_selection to all systems.
	 */
	if (strcmp(name, "all") == 0) {
		expand_event_files("*", event);
		return;
	}

	ptr = strchr(name, ':');

	if (ptr) {
		len = ptr - name;
		str = malloc_or_die(strlen(name) + 1); /* may add '*' */
		strcpy(str, name);
		str[len] = '/';
		ptr++;
		if (!strlen(ptr)) {
			str[len + 1] = '*';
			str[len + 2] = '\0';
		}

		ret = expand_event_files(str, event);
		if (!ignore_event_not_found && ret)
			die("No events enabled with %s", name);
		free(str);
		return;
	}

	/* No ':' so enable all matching systems and events */
	ret = expand_event_files(name, event);

	len = strlen(name) + strlen("*/") + 1;
	str = malloc_or_die(len);
	snprintf(str, len, "*/%s", name);
	ret2 = expand_event_files(str, event);
	free(str);

	if (!ignore_event_not_found && ret && ret2)
		die("No events enabled with %s", name);

	return;
}

static void expand_event_list(void)
{
	struct event_list *compressed_list = event_selection;
	struct event_list *event;

	if (use_old_event_method())
		return;

	event_selection = NULL;

	while (compressed_list) {
		event = compressed_list;
		compressed_list = event->next;
		expand_event(event);
		free(event);
	}
}

static int count_cpus(void)
{
	FILE *fp;
	char buf[1024];
	int cpus = 0;
	char *pbuf;
	size_t *pn;
	size_t n;
	int r;

	cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (cpus > 0)
		return cpus;

	warning("sysconf could not determine number of CPUS");

	/* Do the hack to figure out # of CPUS */
	n = 1024;
	pn = &n;
	pbuf = buf;

	fp = fopen("/proc/cpuinfo", "r");
	if (!fp)
		die("Can not read cpuinfo");

	while ((r = getline(&pbuf, pn, fp)) >= 0) {
		char *p;

		if (strncmp(buf, "processor", 9) != 0)
			continue;
		for (p = buf+9; isspace(*p); p++)
			;
		if (*p == ':')
			cpus++;
	}
	fclose(fp);

	return cpus;
}

static void finish(int sig)
{
	/* all done */
	if (recorder)
		tracecmd_stop_recording(recorder);
	finished = 1;
}

static void flush(int sig)
{
	if (recorder)
		tracecmd_stop_recording(recorder);
}

static void connect_port(int cpu)
{
	struct addrinfo hints;
	struct addrinfo *results, *rp;
	int s;
	char buf[BUFSIZ];

	snprintf(buf, BUFSIZ, "%d", client_ports[cpu]);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = use_tcp ? SOCK_STREAM : SOCK_DGRAM;

	s = getaddrinfo(host, buf, &hints, &results);
	if (s != 0)
		die("connecting to %s server %s:%s",
		    use_tcp ? "TCP" : "UDP", host, buf);

	for (rp = results; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,
			     rp->ai_protocol);
		if (sfd == -1)
			continue;
		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
		close(sfd);
	}

	if (rp == NULL)
		die("Can not connect to %s server %s:%s",
		    use_tcp ? "TCP" : "UDP", host, buf);

	freeaddrinfo(results);

	client_ports[cpu] = sfd;
}

static void set_prio(int prio)
{
	struct sched_param sp;

	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = prio;
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		warning("failed to set priority");
}

/*
 * If extract is set, then this is going to set up the recorder,
 * connections and exit as the tracing is serialized by a single thread.
 */
static int create_recorder(int cpu, int extract)
{
	long ret;
	char *file;
	int pid;

	if (!extract) {
		signal(SIGUSR1, flush);

		pid = fork();
		if (pid < 0)
			die("fork");

		if (pid)
			return pid;

		if (rt_prio)
			set_prio(rt_prio);

		/* do not kill tasks on error */
		cpu_count = 0;
	}

	if (client_ports) {
		connect_port(cpu);
		recorder = tracecmd_create_recorder_fd(client_ports[cpu], cpu);
	} else {
		file = get_temp_file(cpu);
		recorder = tracecmd_create_recorder(file, cpu);
		put_temp_file(file);
	}

	if (!recorder)
		die ("can't create recorder");

	if (extract) {
		ret = tracecmd_flush_recording(recorder);
		tracecmd_free_recorder(recorder);
		return ret;
	}

	while (!finished) {
		if (tracecmd_start_recording(recorder, sleep_time) < 0)
			break;
	}
	tracecmd_free_recorder(recorder);

	exit(0);
}

static void setup_network(void)
{
	struct tracecmd_output *handle;
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	ssize_t n;
	char buf[BUFSIZ];
	char *server;
	char *port;
	char *p;
	int cpu;
	int i;

	if (!strchr(host, ':')) {
		server = strdup("localhost");
		if (!server)
			die("alloctating server");
		port = host;
		host = server;
	} else {
		host = strdup(host);
		if (!host)
			die("alloctating server");
		server = strtok_r(host, ":", &p);
		port = strtok_r(NULL, ":", &p);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	s = getaddrinfo(server, port, &hints, &result);
	if (s != 0)
		die("getaddrinfo: %s", gai_strerror(s));

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,
			     rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
		close(sfd);
	}

	if (!rp)
		die("Can not connect to %s:%s", server, port);

	freeaddrinfo(result);

	n = read(sfd, buf, 8);

	/* Make sure the server is the tracecmd server */
	if (memcmp(buf, "tracecmd", 8) != 0)
		die("server not tracecmd server");

	/* write the number of CPUs we have (in ASCII) */

	sprintf(buf, "%d", cpu_count);

	/* include \0 */
	write(sfd, buf, strlen(buf)+1);

	/* write the pagesize (in ASCII) */
	sprintf(buf, "%d", page_size);

	/* include \0 */
	write(sfd, buf, strlen(buf)+1);

	/*
	 * If we are using IPV4 and our page size is greater than
	 * or equal to 64K, we need to punt and use TCP. :-(
	 */

	/* TODO, test for ipv4 */
	if (page_size >= UDP_MAX_PACKET) {
		warning("page size too big for UDP using TCP in live read");
		use_tcp = 1;
	}

	if (use_tcp) {
		/* Send one option */
		write(sfd, "1", 2);
		/* Size 4 */
		write(sfd, "4", 2);
		/* use TCP */
		write(sfd, "TCP", 4);
	} else
		/* No options */
		write(sfd, "0", 2);

	client_ports = malloc_or_die(sizeof(int) * cpu_count);

	/*
	 * Now we will receive back a comma deliminated list
	 * of client ports to connect to.
	 */
	for (cpu = 0; cpu < cpu_count; cpu++) {
		for (i = 0; i < BUFSIZ; i++) {
			n = read(sfd, buf+i, 1);
			if (n != 1)
				die("Error, reading server ports");
			if (!buf[i] || buf[i] == ',')
				break;
		}
		if (i == BUFSIZ)
			die("read bad port number");
		buf[i] = 0;
		client_ports[cpu] = atoi(buf);
	}

	/* Now create the handle through this socket */
	handle = tracecmd_create_init_fd_glob(sfd, listed_events);

	/* OK, we are all set, let'r rip! */
}

static void finish_network(void)
{
	close(sfd);
	free(host);
}

static void start_threads(void)
{
	int i;

	if (host)
		setup_network();

	/* make a thread for every CPU we have */
	pids = malloc_or_die(sizeof(*pids) * cpu_count);

	memset(pids, 0, sizeof(*pids) * cpu_count);

	for (i = 0; i < cpu_count; i++) {
		pids[i] = create_recorder(i, 0);
	}
}

static void record_data(char *date2ts)
{
	struct tracecmd_output *handle;
	char **temp_files;
	int i;

	if (host) {
		finish_network();
		return;
	}

	if (latency)
		handle = tracecmd_create_file_latency(output_file, cpu_count);
	else {
		if (!cpu_count)
			return;

		temp_files = malloc_or_die(sizeof(*temp_files) * cpu_count);

		for (i = 0; i < cpu_count; i++)
			temp_files[i] = get_temp_file(i);

		handle = tracecmd_create_init_file_glob(output_file, listed_events);
		if (!handle)
			die("Error creating output file");

		if (date2ts)
			tracecmd_add_option(handle, TRACECMD_OPTION_DATE,
					    strlen(date2ts)+1, date2ts);

		tracecmd_append_cpu_data(handle, cpu_count, temp_files);

		for (i = 0; i < cpu_count; i++)
			put_temp_file(temp_files[i]);
		free(temp_files);
	}
	if (!handle)
		die("could not write to file");
	tracecmd_output_close(handle);
}

static void write_func_file(const char *file, struct func_list **list)
{
	struct func_list *item;
	char *path;
	int fd;

	path = tracecmd_get_tracing_file(file);

	fd = open(path, O_WRONLY | O_TRUNC);
	if (fd < 0)
		goto free;

	while (*list) {
		item = *list;
		*list = item->next;
		write(fd, item->func, strlen(item->func));
		write(fd, " ", 1);
		free(item);
	}
	close(fd);

 free:
	tracecmd_put_tracing_file(path);
}

static int functions_filtered(void)
{
	char buf[1] = { '#' };
	char *path;
	int fd;

	path = tracecmd_get_tracing_file("set_ftrace_filter");
	fd = open(path, O_RDONLY);
	tracecmd_put_tracing_file(path);
	if (fd < 0)
		return 0;

	/*
	 * If functions are not filtered, than the first character
	 * will be '#'. Make sure it is not an '#' and also not space.
	 */
	read(fd, buf, 1);
	close(fd);

	if (buf[0] == '#' || isspace(buf[0]))
		return 0;
	return 1;
}

static void set_funcs(void)
{
	write_func_file("set_ftrace_filter", &filter_funcs);
	write_func_file("set_ftrace_notrace", &notrace_funcs);
	write_func_file("set_graph_function", &graph_funcs);

	/* make sure we are filtering functions */
	if (func_stack) {
		if (!functions_filtered()) {
			disable_all();
			die("Function stack trace set, but functions not filtered");
		}
		save_option(FUNC_STACK_TRACE);
	}
}

static void add_func(struct func_list **list, const char *func)
{
	struct func_list *item;

	item = malloc_or_die(sizeof(*item));
	item->func = func;
	item->next = *list;
	*list = item;
}

static unsigned long long
find_ts_in_page(struct pevent *pevent, void *page, int size)
{
	struct event_format *event;
	struct format_field *field;
	struct record *last_record = NULL;
	struct record *record;
	unsigned long long ts = 0;
	int id;

	if (size <= 0)
		return 0;

	while (!ts) {
		record = tracecmd_read_page_record(pevent, page, size,
						   last_record);
		if (!record)
			break;
		free_record(last_record);
		id = pevent_data_type(pevent, record);
		event = pevent_data_event_from_type(pevent, id);
		if (event) {
			/* Make sure this is our event */
			field = pevent_find_field(event, "buf");
			/* the trace_marker adds a '\n' */
			if (field && strcmp(STAMP"\n", record->data + field->offset) == 0)
				ts = record->ts;
		}
		last_record = record;
	}
	free_record(last_record);

	return ts;
}

static unsigned long long find_time_stamp(struct pevent *pevent)
{
	struct dirent *dent;
	unsigned long long ts = 0;
	void *page;
	char *path;
	char *file;
	DIR *dir;
	int len;
	int fd;
	int r;

	path = tracecmd_get_tracing_file("per_cpu");
	if (!path)
		return 0;

	dir = opendir(path);
	if (!dir)
		goto out;

	len = strlen(path);
	file = malloc_or_die(len + strlen("trace_pipe_raw") + 32);
	page = malloc_or_die(page_size);

	while ((dent = readdir(dir))) {
		const char *name = dent->d_name;

		if (strncmp(name, "cpu", 3) != 0)
			continue;

		sprintf(file, "%s/%s/trace_pipe_raw", path, name);
		fd = open(file, O_RDONLY);
		if (fd < 0)
			continue;
		do {
			r = read(fd, page, page_size);
			ts = find_ts_in_page(pevent, page, r);
			if (ts)
				break;
		} while (r > 0);
		if (ts)
			break;
	}
	free(file);
	free(page);
	closedir(dir);

 out:
	tracecmd_put_tracing_file(path);
	return ts;
}

static char *read_file(char *file, int *psize)
{
	char buffer[BUFSIZ];
	char *path;
	char *buf;
	int size = 0;
	int fd;
	int r;

	path = tracecmd_get_tracing_file(file);
	fd = open(path, O_RDONLY);
	tracecmd_put_tracing_file(path);
	if (fd < 0) {
		warning("%s not found, --date ignored", file);
		return NULL;
	}
	do {
		r = read(fd, buffer, BUFSIZ);
		if (r <= 0)
			continue;
		if (size) {
			buf = realloc(buf, size+r+1);
			if (!buf)
				die("malloc");
		} else
			buf = malloc_or_die(r+1);
		memcpy(buf+size, buffer, r);
		size += r;
	} while (r);

	buf[size] = '\0';
	if (psize)
		*psize = size;
	return buf;
}

/*
 * Try to write the date into the ftrace buffer and then
 * read it back, mapping the timestamp to the date.
 */
static char *get_date_to_ts(void)
{
	unsigned long long min = -1ULL;
	unsigned long long diff;
	unsigned long long stamp;
	unsigned long long min_stamp;
	unsigned long long min_ts;
	unsigned long long ts;
	struct pevent *pevent;
	struct timeval start;
	struct timeval end;
	char *date2ts = NULL;
	char *path;
	char *buf;
	int size;
	int tfd;
	int ret;
	int i;

	/* Set up a pevent to read the raw format */
	pevent = pevent_alloc();
	if (!pevent) {
		warning("failed to alloc pevent, --date ignored");
		return NULL;
	}

	buf = read_file("events/header_page", &size);
	if (!buf)
		goto out_pevent;
	ret = pevent_parse_header_page(pevent, buf, size, sizeof(unsigned long));
	free(buf);
	if (ret < 0) {
		warning("Can't parse header page, --date ignored");
		goto out_pevent;
	}

	/* Find the format for ftrace:print. */
	buf = read_file("events/ftrace/print/format", &size);
	if (!buf)
		goto out_pevent;
	ret = pevent_parse_event(pevent, buf, size, "ftrace");
	free(buf);
	if (ret < 0) {
		warning("Can't parse print event, --date ignored");
		goto out_pevent;
	}

	path = tracecmd_get_tracing_file("trace_marker");
	tfd = open(path, O_WRONLY);
	tracecmd_put_tracing_file(path);
	if (tfd < 0) {
		warning("Can not open 'trace_marker', --date ignored");
		goto out_pevent;
	}

	for (i = 0; i < date2ts_tries; i++) {
		disable_tracing();
		clear_trace();
		enable_tracing();

		gettimeofday(&start, NULL);
		write(tfd, STAMP, 5);
		gettimeofday(&end, NULL);

		disable_tracing();
		ts = find_time_stamp(pevent);
		if (!ts)
			continue;

		diff = (unsigned long long)end.tv_sec * 1000000;
		diff += (unsigned long long)end.tv_usec;
		stamp = diff;
		diff -= (unsigned long long)start.tv_sec * 1000000;
		diff -= (unsigned long long)start.tv_usec;

		if (diff < min) {
			min_ts = ts;
			min_stamp = stamp - diff / 2;
			min = diff;
		}
	}

	close(tfd);

	/* 16 hex chars + 0x + \0 */
	date2ts = malloc(19);
	if (!date2ts)
		goto out_pevent;

	/*
	 * The difference between the timestamp and the gtod is
	 * stored as an ASCII string in hex.
	 */
	snprintf(date2ts, 19, "0x%llx", min_stamp - min_ts / 1000);

 out_pevent:
	pevent_free(pevent);

	return date2ts;
}

void set_buffer_size(void)
{
	char buf[BUFSIZ];
	char *path;
	int ret;
	int fd;

	if (!buffer_size)
		return;

	if (buffer_size < 0)
		die("buffer size must be positive");

	snprintf(buf, BUFSIZ, "%d", buffer_size);

	path = tracecmd_get_tracing_file("buffer_size_kb");
	fd = open(path, O_WRONLY);
	if (fd < 0)
		die("can't open %s", path);

	ret = write(fd, buf, strlen(buf));
	if (ret < 0)
		warning("Can't write to %s", path);
	tracecmd_put_tracing_file(path);
	close(fd);
}

static void check_plugin(const char *plugin)
{
	char *buf;
	char *tok;

	/*
	 * nop is special. We may want to just trace
	 * trace_printks, that are in the kernel.
	 */
	if (strcmp(plugin, "nop") == 0)
		return;

	buf = read_file("available_tracers", NULL);
	if (!buf)
		die("No plugins available");

	while ((tok = strtok(buf, " "))) {
		buf = NULL;
		if (strcmp(tok, plugin) == 0)
			goto out;
	}
	die ("Plugin '%s' does not exist", plugin);
 out:
	fprintf(stderr, "  plugin '%s'\n", plugin);
	free(buf);
}

static void record_all_events(void)
{
	struct tracecmd_event_list *list;

	while (listed_events) {
		list = listed_events;
		listed_events = list->next;
		free(list);
	}
	list = malloc_or_die(sizeof(*list));
	list->next = NULL;
	list->glob = "*/*";
	listed_events = list;
}

enum {
	OPT_funcstack	= 254,
	OPT_date	= 255,
};

void trace_record (int argc, char **argv)
{
	const char *plugin = NULL;
	const char *output = NULL;
	const char *option;
	struct event_list *event;
	struct event_list *last_event;
	struct tracecmd_event_list *list;
	struct trace_seq *s;
	char *date2ts = NULL;
	int record_all = 0;
	int disable = 0;
	int events = 0;
	int record = 0;
	int extract = 0;
	int run_command = 0;
	int neg_event = 0;
	int keep = 0;
	int date = 0;
	int fset;
	int cpu;

	int c;

	cpu_count = count_cpus();

	if ((record = (strcmp(argv[1], "record") == 0)))
		; /* do nothing */
	else if (strcmp(argv[1], "start") == 0)
		; /* do nothing */
	else if ((extract = strcmp(argv[1], "extract") == 0))
		; /* do nothing */
	else if (strcmp(argv[1], "stop") == 0) {
		disable_tracing();
		exit(0);
	} else if (strcmp(argv[1], "reset") == 0) {
		while ((c = getopt(argc-1, argv+1, "b:")) >= 0) {
			switch (c) {
			case 'b':
				buffer_size = atoi(optarg);
				/* Min buffer size is 1 */
				if (strcmp(optarg, "0") == 0)
					buffer_size = 1;
				break;
			}
		}
		disable_all();
		set_buffer_size();
		exit(0);
	} else
		usage(argv);

	for (;;) {
		int option_index = 0;
		static struct option long_options[] = {
			{"date", no_argument, NULL, OPT_date},
			{"func-stack", no_argument, NULL, OPT_funcstack},
			{"help", no_argument, NULL, '?'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc-1, argv+1, "+hae:f:Fp:cdo:O:s:r:vg:l:n:P:N:tb:kiT",
				 long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			usage(argv);
			break;
		case 'a':
			if (!extract) {
				record_all = 1;
				record_all_events();
			}
			break;
		case 'e':
			if (extract)
				usage(argv);
			events = 1;
			event = malloc_or_die(sizeof(*event));
			memset(event, 0, sizeof(*event));
			event->event = optarg;
			event->next = event_selection;
			event->neg = neg_event;
			event_selection = event;
			event->filter = NULL;
			last_event = event;

			if (!record_all) {
				list = malloc_or_die(sizeof(*list));
				list->next = listed_events;
				list->glob = optarg;
				listed_events = list;
			}

			break;
		case 'f':
			if (!last_event)
				die("filter must come after event");
			if (last_event->filter) {
				last_event->filter =
					realloc(last_event->filter,
						strlen(last_event->filter) +
						strlen("&&()") +
						strlen(optarg) + 1);
				strcat(last_event->filter, "&&(");
				strcat(last_event->filter, optarg);
				strcat(last_event->filter, ")");
			} else {
				last_event->filter =
					malloc_or_die(strlen(optarg) +
						      strlen("()") + 1);
				sprintf(last_event->filter, "(%s)", optarg);
			}
			break;

		case 'F':
			if (filter_pid >= 0)
				die("-P and -F can not both be specified");
			filter_task = 1;
			break;
		case 'P':
			if (filter_task)
				die("-P and -F can not both be specified");
			if (filter_pid >= 0)
				die("only one -P pid can be filtered at a time");
			filter_pid = atoi(optarg);
			break;
		case 'c':
#ifdef NO_PTRACE
			die("-c invalid: ptrace not supported");
#endif
			do_ptrace = 1;
			break;
		case 'v':
			if (extract)
				usage(argv);
			neg_event = 1;
			break;
		case 'l':
			add_func(&filter_funcs, optarg);
			break;
		case 'n':
			add_func(&notrace_funcs, optarg);
			break;
		case 'g':
			add_func(&graph_funcs, optarg);
			break;
		case 'p':
			if (plugin)
				die("only one plugin allowed");
			for (plugin = optarg; isspace(*plugin); plugin++)
				;
			for (optarg += strlen(optarg) - 1;
			     optarg > plugin && isspace(*optarg); optarg--)
				;
			optarg++;
			optarg[0] = '\0';
			break;
		case 'd':
			if (extract)
				usage(argv);
			disable = 1;
			break;
		case 'o':
			if (host)
				die("-o incompatible with -N");
			if (!record && !extract)
				die("start does not take output\n"
				    "Did you mean 'record'?");
			if (output)
				die("only one output file allowed");
			output = optarg;
			break;
		case 'O':
			option = optarg;
			save_option(option);
			break;
		case 'T':
			save_option("stacktrace");
			break;
		case 's':
			if (extract)
				usage(argv);
			sleep_time = atoi(optarg);
			break;
		case 'r':
			rt_prio = atoi(optarg);
			break;
		case 'N':
			if (!record)
				die("-N only available with record");
			if (output)
				die("-N incompatible with -o");
			host = optarg;
			break;
		case 't':
			use_tcp = 1;
			break;
		case 'b':
			buffer_size = atoi(optarg);
			break;
		case 'k':
			keep = 1;
			break;
		case 'i':
			ignore_event_not_found = 1;
			break;
		case OPT_date:
			date = 1;
			break;
		case OPT_funcstack:
			func_stack = 1;
			break;
		default:
			usage(argv);
		}
	}

	if (plugin && strncmp(plugin, "function", 8) == 0 &&
	    func_stack && !filter_funcs)
		die("Must supply function filtering with --func-stack\n");

	if (do_ptrace && !filter_task && (filter_pid < 0))
		die(" -c can only be used with -F or -P");

	if ((argc - optind) >= 2) {
		if (!record)
			die("Command start does not take any commands\n"
			    "Did you mean 'record'?");
		if (extract)
			die("Command extract does not take any commands\n"
			    "Did you mean 'record'?");
		run_command = 1;
	}

	if (!events && !plugin && !extract)
		die("no event or plugin was specified... aborting");

	if (output)
		output_file = output;

	tracing_on_init_val = read_tracing_on();

	/* Extracting data records all events in the system. */
	if (extract && !record_all)
		record_all_events();

	if (event_selection)
		expand_event_list();

	page_size = getpagesize();

	if (!extract) {
		fset = set_ftrace(!disable);
		disable_all();

		/* Record records the date first */
		if (record && date)
			date2ts = get_date_to_ts();

		set_funcs();

		if (events)
			enable_events();
		set_buffer_size();
	}

	if (plugin) {

		check_plugin(plugin);
		/*
		 * Latency tracers just save the trace and kill
		 * the threads.
		 */
		if (strcmp(plugin, "irqsoff") == 0 ||
		    strcmp(plugin, "preemptoff") == 0 ||
		    strcmp(plugin, "preemptirqsoff") == 0 ||
		    strcmp(plugin, "wakeup") == 0 ||
		    strcmp(plugin, "wakeup_rt") == 0) {
			latency = 1;
			if (host)
				die("Network tracing not available with latency tracer plugins");
		}
		if (fset < 0 && (strcmp(plugin, "function") == 0 ||
				 strcmp(plugin, "function_graph") == 0))
			die("function tracing not configured on this kernel");
		if (!extract)
			set_plugin(plugin);
	}

	set_options();

	s = malloc_or_die(sizeof(*s) * cpu_count);

	if (record) {
		signal(SIGINT, finish);
		if (!latency)
			start_threads();
	}

	if (extract) {
		flush_threads();

	} else {
		if (!record) {
			update_task_filter();
			exit(0);
		}

		if (run_command)
			run_cmd((argc - optind) - 1, &argv[optind + 1]);
		else {
			update_task_filter();
			/* We don't ptrace ourself */
			if (do_ptrace && filter_pid >= 0)
				ptrace_attach(filter_pid);
			/* sleep till we are woken with Ctrl^C */
			printf("Hit Ctrl^C to stop recording\n");
			while (!finished)
				trace_or_sleep();
		}

		disable_tracing();
		stop_threads();
	}

	for (cpu = 0; cpu < cpu_count; cpu++) {
		trace_seq_init(&s[cpu]);
		trace_seq_printf(&s[cpu], "CPU: %d\n", cpu);
		tracecmd_stat_cpu(&s[cpu], cpu);
	}

	if (!keep)
		disable_all();

	printf("Kernel buffer statistics:\n"
	       "  Note: \"entries\" are the entries left in the kernel ring buffer and are not\n"
	       "        recorded in the trace data. They should all be zero.\n\n");
	for (cpu = 0; cpu < cpu_count; cpu++) {
		trace_seq_do_printf(&s[cpu]);
		trace_seq_destroy(&s[cpu]);
		printf("\n");
	}


	/* extract records the date after extraction */
	if (extract && date)
		date2ts = get_date_to_ts();

	record_data(date2ts);
	delete_thread_data();

	if (keep)
		exit(0);

	/* If tracing_on was enabled before we started, set it on now */
	if (tracing_on_init_val)
		write_tracing_on(tracing_on_init_val);

	exit(0);
}
