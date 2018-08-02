// SPDX-License-Identifier: LGPL-2.1

/*
 * Copyright (C) 2017 VMware Inc, Yordan Karadzhov <y.karadz@gmail.com>
 */

 /**
 *  @file    libkshark.c
 *  @brief   API for processing of FTRACE (trace-cmd) data.
 */

/** Use GNU C Library. */
#define _GNU_SOURCE 1

// C
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

// KernelShark
#include "libkshark.h"

static __thread struct trace_seq seq;

static struct kshark_context *kshark_context_handler = NULL;

static bool kshark_default_context(struct kshark_context **context)
{
	struct kshark_context *kshark_ctx;

	kshark_ctx = calloc(1, sizeof(*kshark_ctx));
	if (!kshark_ctx)
		return false;

	kshark_ctx->show_task_filter = tracecmd_filter_id_hash_alloc();
	kshark_ctx->hide_task_filter = tracecmd_filter_id_hash_alloc();

	kshark_ctx->show_event_filter = tracecmd_filter_id_hash_alloc();
	kshark_ctx->hide_event_filter = tracecmd_filter_id_hash_alloc();

	kshark_ctx->filter_mask = 0x0;

	/* Will free kshark_context_handler. */
	kshark_free(NULL);

	/* Will do nothing if *context is NULL. */
	kshark_free(*context);

	*context = kshark_context_handler = kshark_ctx;

	return true;
}

static bool init_thread_seq(void)
{
	if (!seq.buffer)
		trace_seq_init(&seq);

	return seq.buffer != NULL;
}

/**
 * @brief Initialize a kshark session. This function must be called before
 *	  calling any other kshark function. If the session has been
 *	  initialized, this function can be used to obtain the session's
 *	  context.
 * @param kshark_ctx: Optional input/output location for context pointer.
 *		      If it points to a context, that context will become
 *		      the new session. If it points to NULL, it will obtain
 *		      the current (or new) session. The result is only
 *		      valid on return of true.
 * @returns True on success, or false on failure.
 */
bool kshark_instance(struct kshark_context **kshark_ctx)
{
	if (*kshark_ctx != NULL) {
		/* Will free kshark_context_handler */
		kshark_free(NULL);

		/* Use the context provided by the user. */
		kshark_context_handler = *kshark_ctx;
	} else {
		if (kshark_context_handler) {
			/*
			 * No context is provided by the user, but the context
			 * handler is already set. Use the context handler.
			 */
			*kshark_ctx = kshark_context_handler;
		} else {
			/* No kshark_context exists. Create a default one. */
			if (!kshark_default_context(kshark_ctx))
				return false;
		}
	}

	if (!init_thread_seq())
		return false;

	return true;
}

static void kshark_free_task_list(struct kshark_context *kshark_ctx)
{
	struct kshark_task_list *task;
	int i;

	if (!kshark_ctx)
		return;

	for (i = 0; i < KS_TASK_HASH_SIZE; ++i) {
		while (kshark_ctx->tasks[i]) {
			task = kshark_ctx->tasks[i];
			kshark_ctx->tasks[i] = task->next;
			free(task);
		}
	}
}

/**
 * @brief Open and prepare for reading a trace data file specified by "file".
 *	  If the specified file does not exist, or contains no trace data,
 *	  the function returns false.
 * @param kshark_ctx: Input location for context pointer.
 * @param file: The file to load.
 * @returns True on success, or false on failure.
 */
bool kshark_open(struct kshark_context *kshark_ctx, const char *file)
{
	struct tracecmd_input *handle;

	kshark_free_task_list(kshark_ctx);

	handle = tracecmd_open(file);
	if (!handle)
		return false;

	if (pthread_mutex_init(&kshark_ctx->input_mutex, NULL) != 0) {
		tracecmd_close(handle);
		return false;
	}

	kshark_ctx->handle = handle;
	kshark_ctx->pevent = tracecmd_get_pevent(handle);

	kshark_ctx->advanced_event_filter =
		pevent_filter_alloc(kshark_ctx->pevent);

	/*
	 * Turn off function trace indent and turn on show parent
	 * if possible.
	 */
	trace_util_add_option("ftrace:parent", "1");
	trace_util_add_option("ftrace:indent", "0");

	return true;
}

/**
 * @brief Close the trace data file and free the trace data handle.
 * @param kshark_ctx: Input location for the session context pointer.
 */
void kshark_close(struct kshark_context *kshark_ctx)
{
	if (!kshark_ctx || !kshark_ctx->handle)
		return;

	/*
	 * All filters are file specific. Make sure that the Pids and Event Ids
	 * from this file are not going to be used with another file.
	 */
	tracecmd_filter_id_clear(kshark_ctx->show_task_filter);
	tracecmd_filter_id_clear(kshark_ctx->hide_task_filter);
	tracecmd_filter_id_clear(kshark_ctx->show_event_filter);
	tracecmd_filter_id_clear(kshark_ctx->hide_event_filter);

	if (kshark_ctx->advanced_event_filter) {
		pevent_filter_reset(kshark_ctx->advanced_event_filter);
		pevent_filter_free(kshark_ctx->advanced_event_filter);
		kshark_ctx->advanced_event_filter = NULL;
	}

	tracecmd_close(kshark_ctx->handle);
	kshark_ctx->handle = NULL;
	kshark_ctx->pevent = NULL;

	pthread_mutex_destroy(&kshark_ctx->input_mutex);
}

/**
 * @brief Deinitialize kshark session. Should be called after closing all
 *	  open trace data files and before your application terminates.
 * @param kshark_ctx: Optional input location for session context pointer.
 *		      If it points to a context of a sessuin, that sessuin
 *		      will be deinitialize. If it points to NULL, it will
 *		      deinitialize the current session.
 */
void kshark_free(struct kshark_context *kshark_ctx)
{
	if (kshark_ctx == NULL) {
		if (kshark_context_handler == NULL)
			return;

		kshark_ctx = kshark_context_handler;
		/* kshark_ctx_handler will be set to NULL below. */
	}

	tracecmd_filter_id_hash_free(kshark_ctx->show_task_filter);
	tracecmd_filter_id_hash_free(kshark_ctx->hide_task_filter);

	tracecmd_filter_id_hash_free(kshark_ctx->show_event_filter);
	tracecmd_filter_id_hash_free(kshark_ctx->hide_event_filter);

	kshark_free_task_list(kshark_ctx);

	if (seq.buffer)
		trace_seq_destroy(&seq);

	if (kshark_ctx == kshark_context_handler)
		kshark_context_handler = NULL;

	free(kshark_ctx);
}

static inline uint8_t knuth_hash8(uint32_t val)
{
	/*
	 * Hashing functions, based on Donald E. Knuth's Multiplicative
	 * hashing. See The Art of Computer Programming (TAOCP).
	 * Multiplication by the Prime number, closest to the golden
	 * ratio of 2^8.
	 */
	return UINT8_C(val) * UINT8_C(157);
}

static struct kshark_task_list *
kshark_find_task(struct kshark_context *kshark_ctx, uint8_t key, int pid)
{
	struct kshark_task_list *list;

	for (list = kshark_ctx->tasks[key]; list; list = list->next) {
		if (list->pid == pid)
			return list;
	}

	return NULL;
}

static struct kshark_task_list *
kshark_add_task(struct kshark_context *kshark_ctx, int pid)
{
	struct kshark_task_list *list;
	uint8_t key;

	key = knuth_hash8(pid);
	list = kshark_find_task(kshark_ctx, key, pid);
	if (list)
		return list;

	list = malloc(sizeof(*list));
	if (!list)
		return NULL;

	list->pid = pid;
	list->next = kshark_ctx->tasks[key];
	kshark_ctx->tasks[key] = list;

	return list;
}

/**
 * @brief Get an array containing the Process Ids of all tasks presented in
 *	  the loaded trace data file.
 * @param kshark_ctx: Input location for context pointer.
 * @param pids: Output location for the Pids of the tasks. The user is
 *		responsible for freeing the elements of the outputted array.
 * @returns The size of the outputted array of Pids in the case of success,
 *	    or a negative error code on failure.
 */
ssize_t kshark_get_task_pids(struct kshark_context *kshark_ctx, int **pids)
{
	size_t i, pid_count = 0, pid_size = KS_TASK_HASH_SIZE;
	struct kshark_task_list *list;
	int *temp_pids;

	*pids = calloc(pid_size, sizeof(int));
	if (!*pids)
		goto fail;

	for (i = 0; i < KS_TASK_HASH_SIZE; ++i) {
		list = kshark_ctx->tasks[i];
		while (list) {
			(*pids)[pid_count] = list->pid;
			list = list->next;
			if (++pid_count >= pid_size) {
				pid_size *= 2;
				temp_pids = realloc(*pids, pid_size * sizeof(int));
				if (!temp_pids) {
					goto fail;
				}
				*pids = temp_pids;
			}
		}
	}

	temp_pids = realloc(*pids, pid_count * sizeof(int));
	if (!temp_pids)
		goto fail;

	/* Paranoid: In the unlikely case of shrinking *pids, realloc moves it */
	*pids = temp_pids;

	return pid_count;

fail:
	fprintf(stderr, "Failed to allocate memory for Task Pids.\n");
	free(*pids);
	*pids = NULL;
	return -ENOMEM;
}

static bool filter_find(struct tracecmd_filter_id *filter, int pid,
			bool test)
{
	return !filter || !filter->count ||
		!!(unsigned long)tracecmd_filter_id_find(filter, pid) == test;
}

static bool kshark_show_task(struct kshark_context *kshark_ctx, int pid)
{
	return filter_find(kshark_ctx->show_task_filter, pid, true) &&
	       filter_find(kshark_ctx->hide_task_filter, pid, false);
}

static bool kshark_show_event(struct kshark_context *kshark_ctx, int pid)
{
	return filter_find(kshark_ctx->show_event_filter, pid, true) &&
	       filter_find(kshark_ctx->hide_event_filter, pid, false);
}

/**
 * @brief Add an Id value to the filster specified by "filter_id".
 * @param kshark_ctx: Input location for the session context pointer.
 * @param filter_id: Identifier of the filter.
 * @param id: Id value to be added to the filter.
 */
void kshark_filter_add_id(struct kshark_context *kshark_ctx,
			  int filter_id, int id)
{
	struct tracecmd_filter_id *filter;

	switch (filter_id) {
		case KS_SHOW_EVENT_FILTER:
			filter = kshark_ctx->show_event_filter;
			break;
		case KS_HIDE_EVENT_FILTER:
			filter = kshark_ctx->hide_event_filter;
			break;
		case KS_SHOW_TASK_FILTER:
			filter = kshark_ctx->show_task_filter;
			break;
		case KS_HIDE_TASK_FILTER:
			filter = kshark_ctx->hide_task_filter;
			break;
		default:
			return;
	}

	tracecmd_filter_id_add(filter, id);
}

/**
 * @brief Clear (reset) the filster specified by "filter_id".
 * @param kshark_ctx: Input location for the session context pointer.
 * @param filter_id: Identifier of the filter.
 */
void kshark_filter_clear(struct kshark_context *kshark_ctx, int filter_id)
{
	struct tracecmd_filter_id *filter;

	switch (filter_id) {
		case KS_SHOW_EVENT_FILTER:
			filter = kshark_ctx->show_event_filter;
			break;
		case KS_HIDE_EVENT_FILTER:
			filter = kshark_ctx->hide_event_filter;
			break;
		case KS_SHOW_TASK_FILTER:
			filter = kshark_ctx->show_task_filter;
			break;
		case KS_HIDE_TASK_FILTER:
			filter = kshark_ctx->hide_task_filter;
			break;
		default:
			return;
	}

	tracecmd_filter_id_clear(filter);
}

static bool filter_is_set(struct tracecmd_filter_id *filter)
{
	return filter && filter->count;
}

static bool kshark_filter_is_set(struct kshark_context *kshark_ctx)
{
	return filter_is_set(kshark_ctx->show_task_filter) ||
	       filter_is_set(kshark_ctx->hide_task_filter) ||
	       filter_is_set(kshark_ctx->show_event_filter) ||
	       filter_is_set(kshark_ctx->hide_event_filter);
}

static void unset_event_filter_flag(struct kshark_context *kshark_ctx,
				    struct kshark_entry *e)
{
	/*
	 * All entries, filtered-out by the event filters, will be treated
	 * differently, when visualized. Because of this, ignore the value
	 * of the GRAPH_VIEW flag provided by the user via
	 * kshark_ctx->filter_mask and unset the EVENT_VIEW flag.
	 */
	int event_mask = kshark_ctx->filter_mask;

	event_mask &= ~KS_GRAPH_VIEW_FILTER_MASK;
	event_mask |= KS_EVENT_VIEW_FILTER_MASK;
	e->visible &= ~event_mask;
}

/**
 * @brief This function loops over the array of entries specified by "data"
 *	  and "n_entries" and sets the "visible" fields of each entry
 *	  according to the criteria provided by the filters of the session's
 *	  context. The field "filter_mask" of the session's context is used to
 *	  control the level of visibility/invisibility of the entries which
 *	  are filtered-out.
 *	  WARNING: Do not use this function if the advanced filter is set.
 *	  Applying the advanced filter requires access to prevent_record,
 *	  hence the data has to be reloaded using kshark_load_data_entries().
 * @param kshark_ctx: Input location for the session context pointer.
 * @param data: Input location for the trace data to be filtered.
 * @param n_entries: The size of the inputted data.
 */
void kshark_filter_entries(struct kshark_context *kshark_ctx,
			   struct kshark_entry **data,
			   size_t n_entries)
{
	int i;

	if (kshark_ctx->advanced_event_filter->filters) {
		/* The advanced filter is set. */
		fprintf(stderr,
			"Failed to filter!\n");
		fprintf(stderr,
			"Reset the Advanced filter or reload the data.\n");
		return;
	}

	if (!kshark_filter_is_set(kshark_ctx))
		return;

	/* Apply only the Id filters. */
	for (i = 0; i < n_entries; ++i) {
		/* Start with and entry which is visible everywhere. */
		data[i]->visible = 0xFF;

		/* Apply event filtering. */
		if (!kshark_show_event(kshark_ctx, data[i]->event_id))
			unset_event_filter_flag(kshark_ctx, data[i]);

		/* Apply task filtering. */
		if (!kshark_show_task(kshark_ctx, data[i]->pid))
			data[i]->visible &= ~kshark_ctx->filter_mask;
	}
}

static void kshark_set_entry_values(struct kshark_context *kshark_ctx,
				    struct pevent_record *record,
				    struct kshark_entry *entry)
{
	/* Offset of the record */
	entry->offset = record->offset;

	/* CPU Id of the record */
	entry->cpu = record->cpu;

	/* Time stamp of the record */
	entry->ts = record->ts;

	/* Event Id of the record */
	entry->event_id = pevent_data_type(kshark_ctx->pevent, record);

	/*
	 * Is visible mask. This default value means that the entry
	 * is visible everywhere.
	 */
	entry->visible = 0xFF;

	/* Process Id of the record */
	entry->pid = pevent_data_pid(kshark_ctx->pevent, record);
}

/**
 * rec_list is used to pass the data to the load functions.
 * The rec_list will contain the list of entries from the source,
 * and will be a link list of per CPU entries.
 */
struct rec_list {
	union {
		/* Used by kshark_load_data_records */
		struct {
			/** next pointer, matches entry->next */
			struct rec_list		*next;
			/** pointer to the raw record data */
			struct pevent_record	*rec;
		};
		/** entry - Used for kshark_load_data_entries() */
		struct kshark_entry		entry;
	};
};

/**
 * rec_type defines what type of rec_list is being used.
 */
enum rec_type {
	REC_RECORD,
	REC_ENTRY,
};

static void free_rec_list(struct rec_list **rec_list, int n_cpus,
			  enum rec_type type)
{
	struct rec_list *temp_rec;
	int cpu;

	for (cpu = 0; cpu < n_cpus; ++cpu) {
		while (rec_list[cpu]) {
			temp_rec = rec_list[cpu];
			rec_list[cpu] = temp_rec->next;
			if (type == REC_RECORD)
				free_record(temp_rec->rec);
			free(temp_rec);
		}
	}
	free(rec_list);
}

static size_t get_records(struct kshark_context *kshark_ctx,
			  struct rec_list ***rec_list, enum rec_type type)
{
	struct event_filter *adv_filter;
	struct kshark_task_list *task;
	struct pevent_record *rec;
	struct rec_list **temp_next;
	struct rec_list **cpu_list;
	struct rec_list *temp_rec;
	size_t count, total = 0;
	int n_cpus;
	int pid;
	int cpu;

	n_cpus = tracecmd_cpus(kshark_ctx->handle);
	cpu_list = calloc(n_cpus, sizeof(*cpu_list));
	if (!cpu_list)
		return -ENOMEM;

	/* Just to shorten the name */
	if (type == REC_ENTRY)
		adv_filter = kshark_ctx->advanced_event_filter;

	for (cpu = 0; cpu < n_cpus; ++cpu) {
		count = 0;
		cpu_list[cpu] = NULL;
		temp_next = &cpu_list[cpu];

		rec = tracecmd_read_cpu_first(kshark_ctx->handle, cpu);
		while (rec) {
			*temp_next = temp_rec = calloc(1, sizeof(*temp_rec));
			if (!temp_rec)
				goto fail;

			temp_rec->next = NULL;

			switch (type) {
			case REC_RECORD:
				temp_rec->rec = rec;
				pid = pevent_data_pid(kshark_ctx->pevent, rec);
				break;
			case REC_ENTRY: {
				struct kshark_entry *entry;
				int ret;

				entry = &temp_rec->entry;
				kshark_set_entry_values(kshark_ctx, rec, entry);
				pid = entry->pid;
				/* Apply event filtering. */
				ret = FILTER_NONE;
				if (adv_filter->filters)
					ret = pevent_filter_match(adv_filter, rec);

				if (!kshark_show_event(kshark_ctx, entry->event_id) ||
				    ret != FILTER_MATCH) {
					unset_event_filter_flag(kshark_ctx, entry);
				}

				/* Apply task filtering. */
				if (!kshark_show_task(kshark_ctx, entry->pid)) {
					entry->visible &= ~kshark_ctx->filter_mask;
				}
				free_record(rec);
				break;
			} /* REC_ENTRY */
			}

			task = kshark_add_task(kshark_ctx, pid);
			if (!task) {
				free_record(rec);
				goto fail;
			}

			temp_next = &temp_rec->next;

			++count;
			rec = tracecmd_read_data(kshark_ctx->handle, cpu);
		}

		total += count;
	}

	*rec_list = cpu_list;
	return total;

 fail:
	free_rec_list(cpu_list, n_cpus, type);
	return -ENOMEM;
}

static int pick_next_cpu(struct rec_list **rec_list, int n_cpus,
			 enum rec_type type)
{
	uint64_t ts = 0;
	uint64_t rec_ts;
	int next_cpu = -1;
	int cpu;

	for (cpu = 0; cpu < n_cpus; ++cpu) {
		if (!rec_list[cpu])
			continue;

		switch (type) {
		case REC_RECORD:
			rec_ts = rec_list[cpu]->rec->ts;
			break;
		case REC_ENTRY:
			rec_ts = rec_list[cpu]->entry.ts;
			break;
		}
		if (!ts || rec_ts < ts) {
			ts = rec_ts;
			next_cpu = cpu;
		}
	}

	return next_cpu;
}

/**
 * @brief Load the content of the trace data file into an array of
 *	  kshark_entries. This function provides an abstraction of the
 *	  entries from the raw data that is read, however the "latency"
 *	  and the "info" fields can be accessed only via the offset
 *	  into the file. This makes the access to these two fields much
 *	  slower.
 *	  If one or more filters are set, the "visible" fields of each entry
 *	  is updated according to the criteria provided by the filters. The
 *	  field "filter_mask" of the session's context is used to control the
 *	  level of visibility/invisibility of the filtered entries.
 * @param kshark_ctx: Input location for context pointer.
 * @param data_rows: Output location for the trace data. The user is
 *		     responsible for freeing the elements of the outputted
 *		     array.
 * @returns The size of the outputted data in the case of success, or a
 *	    negative error code on failure.
 */
ssize_t kshark_load_data_entries(struct kshark_context *kshark_ctx,
				struct kshark_entry ***data_rows)
{
	struct kshark_entry **rows;
	struct rec_list **rec_list;
	enum rec_type type = REC_ENTRY;
	size_t count, total = 0;
	int n_cpus;

	if (*data_rows)
		free(*data_rows);

	/*
	 * TODO: Getting the records separately slows this function
	 *       down, instead of just accessing the records when
	 *	 setting up the kernel entries. But this keeps the
	 *	 code simplier. We should revisit to see if we can
	 *	 bring back the performance.
	 */
	total = get_records(kshark_ctx, &rec_list, type);
	if (total < 0)
		goto fail;

	n_cpus = tracecmd_cpus(kshark_ctx->handle);

	rows = calloc(total, sizeof(struct kshark_entry *));
	if (!rows)
		goto fail_free;

	for (count = 0; count < total; count++) {
		int next_cpu;

		next_cpu = pick_next_cpu(rec_list, n_cpus, type);

		if (next_cpu >= 0) {
			rows[count] = &rec_list[next_cpu]->entry;
			rec_list[next_cpu] = rec_list[next_cpu]->next;
		}
	}

	free_rec_list(rec_list, n_cpus, type);
	*data_rows = rows;
	return total;

 fail_free:
	free_rec_list(rec_list, n_cpus, type);
	for (count = 0; count < total; count++) {
		if (!rows[count])
			break;
		free(rows[count]);
	}
	free(rows);
 fail:
	fprintf(stderr, "Failed to allocate memory during data loading.\n");
	return -ENOMEM;
}

/**
 * @brief Load the content of the trace data file into an array of
 *	  pevent_records. Use this function only if you need fast access
 *	  to all fields of the record.
 * @param kshark_ctx: Input location for the session context pointer.
 * @param data_rows: Output location for the trace data. Use free_record()
 *	 	     to free the elements of the outputted array.
 * @returns The size of the outputted data in the case of success, or a
 *	    negative error code on failure.
 */
ssize_t kshark_load_data_records(struct kshark_context *kshark_ctx,
				struct pevent_record ***data_rows)
{
	struct pevent_record **rows;
	struct pevent_record *rec;
	struct rec_list **rec_list;
	struct rec_list *temp_rec;
	enum rec_type type = REC_RECORD;
	size_t count, total = 0;
	int n_cpus;

	total = get_records(kshark_ctx, &rec_list, REC_RECORD);
	if (total < 0)
		goto fail;

	rows = calloc(total, sizeof(struct pevent_record *));
	if (!rows)
		goto fail;

	n_cpus = tracecmd_cpus(kshark_ctx->handle);

	for (count = 0; count < total; count++) {
		int next_cpu;

		next_cpu = pick_next_cpu(rec_list, n_cpus, type);

		if (next_cpu >= 0) {
			rec = rec_list[next_cpu]->rec;
			rows[count] = rec;

			temp_rec = rec_list[next_cpu];
			rec_list[next_cpu] = rec_list[next_cpu]->next;
			free(temp_rec);
			/* The record is still referenced in rows */
		}
	}

	/* There should be no records left in rec_list */
	free_rec_list(rec_list, n_cpus, type);
	*data_rows = rows;
	return total;

 fail:
	fprintf(stderr, "Failed to allocate memory during data loading.\n");
	return -ENOMEM;
}

static struct pevent_record *kshark_read_at(struct kshark_context *kshark_ctx,
					    uint64_t offset)
{
	/*
	 * It turns that tracecmd_read_at() is not thread-safe.
	 * TODO: Understand why and see if this can be fixed.
	 * For the time being use a mutex to protect the access.
	 */
	pthread_mutex_lock(&kshark_ctx->input_mutex);

	struct pevent_record *data = tracecmd_read_at(kshark_ctx->handle,
						      offset, NULL);

	pthread_mutex_unlock(&kshark_ctx->input_mutex);

	return data;
}

static const char *kshark_get_latency(struct pevent *pe,
				      struct pevent_record *record)
{
	if (!record)
		return NULL;

	trace_seq_reset(&seq);
	pevent_data_lat_fmt(pe, &seq, record);
	return seq.buffer;
}

static const char *kshark_get_info(struct pevent *pe,
				   struct pevent_record *record,
				   struct event_format *event)
{
	char *pos;

	if (!record || !event)
		return NULL;

	trace_seq_reset(&seq);
	pevent_event_info(&seq, event, record);

	/*
	 * The event info string contains a trailing newline.
	 * Remove this newline.
	 */
	if ((pos = strchr(seq.buffer, '\n')) != NULL)
		*pos = '\0';

	return seq.buffer;
}

/**
 * @brief Dump into a string the content of one entry. The function allocates
 *	  a null terminated string and returns a pointer to this string. The
 *	  user has to free the returned string.
 * @param entry: A Kernel Shark entry to be printed.
 * @returns The returned string contains a semicolon-separated list of data
 *	    fields.
 */
char* kshark_dump_entry(struct kshark_entry *entry)
{
	const char *event_name, *task, *lat, *info;
	struct kshark_context *kshark_ctx;
	struct pevent_record *data;
	struct event_format *event;
	char *temp_str, *entry_str;
	int event_id, size = 0;

	kshark_ctx = NULL;
	if (!kshark_instance(&kshark_ctx) || !init_thread_seq())
		return NULL;

	data = kshark_read_at(kshark_ctx, entry->offset);

	event_id = pevent_data_type(kshark_ctx->pevent, data);
	event = pevent_data_event_from_type(kshark_ctx->pevent, event_id);

	event_name = event? event->name : "[UNKNOWN EVENT]";
	task = pevent_data_comm_from_pid(kshark_ctx->pevent, entry->pid);
	lat = kshark_get_latency(kshark_ctx->pevent, data);

	size = asprintf(&temp_str, "%li %s-%i; CPU %i; %s;",
			entry->ts,
			task,
			entry->pid,
			entry->cpu,
			lat);

	info = kshark_get_info(kshark_ctx->pevent, data, event);
	if (size > 0) {
		size = asprintf(&entry_str, "%s %s; %s; 0x%x",
				temp_str,
				event_name,
				info,
				entry->visible);

		free(temp_str);
	}

	free_record(data);

	if (size > 0)
		return entry_str;

	return NULL;
}
