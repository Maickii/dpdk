/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2020 Marvell International Ltd.
 */

#include <fnmatch.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>

#include <rte_common.h>
#include <rte_errno.h>
#include <rte_string_fns.h>

#include "eal_filesystem.h"
#include "eal_trace.h"

const char *
trace_mode_to_string(enum rte_trace_mode mode)
{
	switch (mode) {
	case RTE_TRACE_MODE_OVERWRITE: return "overwrite";
	case RTE_TRACE_MODE_DISCARD: return "discard";
	default: return "unknown";
	}
}

const char *
trace_area_to_string(enum trace_area_e area)
{
	switch (area) {
	case TRACE_AREA_HEAP: return "heap";
	case TRACE_AREA_HUGEPAGE: return "hugepage";
	default: return "unknown";
	}
}

static bool
trace_entry_compare(const char *name)
{
	struct trace_point_head *tp_list = trace_list_head_get();
	struct trace_point *tp;
	int count = 0;

	STAILQ_FOREACH(tp, tp_list, next) {
		if (strncmp(tp->name, name, TRACE_POINT_NAME_SIZE) == 0)
			count++;
		if (count > 1) {
			trace_err("found duplicate entry %s", name);
			rte_errno = EEXIST;
			return true;
		}
	}
	return false;
}

bool
trace_has_duplicate_entry(void)
{
	struct trace_point_head *tp_list = trace_list_head_get();
	struct trace_point *tp;

	/* Is duplicate trace name registered */
	STAILQ_FOREACH(tp, tp_list, next)
		if (trace_entry_compare(tp->name))
			return true;

	return false;
}

void
trace_uuid_generate(void)
{
	struct trace_point_head *tp_list = trace_list_head_get();
	struct trace *trace = trace_obj_get();
	struct trace_point *tp;
	uint64_t sz_total = 0;

	/* Go over the registered trace points to get total size of events */
	STAILQ_FOREACH(tp, tp_list, next) {
		const uint16_t sz = *tp->handle & __RTE_TRACE_FIELD_SIZE_MASK;
		sz_total += sz;
	}

	rte_uuid_t uuid = RTE_UUID_INIT(sz_total, trace->nb_trace_points,
		0x4370, 0x8f50, 0x222ddd514176ULL);
	rte_uuid_copy(trace->uuid, uuid);
}

static int
trace_session_name_generate(char *trace_dir)
{
	struct tm *tm_result;
	time_t tm;
	int rc;

	tm = time(NULL);
	if ((int)tm == -1)
		goto fail;

	tm_result = localtime(&tm);
	if (tm_result == NULL)
		goto fail;

	rc = rte_strscpy(trace_dir, eal_get_hugefile_prefix(),
			TRACE_PREFIX_LEN);
	if (rc == -E2BIG)
		rc = TRACE_PREFIX_LEN;
	trace_dir[rc++] = '-';

	rc = strftime(trace_dir + rc, TRACE_DIR_STR_LEN - rc,
			"%Y-%m-%d-%p-%I-%M-%S", tm_result);
	if (rc == 0)
		goto fail;

	return rc;
fail:
	rte_errno = errno;
	return -rte_errno;
}

static int
trace_dir_update(const char *str)
{
	struct trace *trace = trace_obj_get();
	int rc, remaining;

	remaining = sizeof(trace->dir) - trace->dir_offset;
	rc = rte_strscpy(&trace->dir[0] + trace->dir_offset, str, remaining);
	if (rc < 0)
		goto fail;

	trace->dir_offset += rc;
fail:
	return rc;
}

int
eal_trace_args_save(const char *optarg)
{
	struct trace *trace = trace_obj_get();
	char *trace_args;
	uint8_t nb_args;

	nb_args = trace->args.nb_args;

	if (nb_args >= TRACE_MAX_ARGS) {
		trace_err("ignoring trace %s as limit exceeds", optarg);
		return 0;
	}

	trace_args = calloc(1, (strlen(optarg) + 1));
	if (trace_args == NULL) {
		trace_err("fail to allocate memory for %s", optarg);
		return -ENOMEM;
	}

	memcpy(trace_args, optarg, strlen(optarg));
	trace->args.args[nb_args++] = trace_args;
	trace->args.nb_args = nb_args;
	return 0;
}

void
eal_trace_args_free(void)
{
	struct trace *trace = trace_obj_get();
	int i;

	for (i = 0; i < trace->args.nb_args; i++) {
		if (trace->args.args[i]) {
			free((void *)trace->args.args[i]);
			trace->args.args[i] = NULL;
		}
	}
}

int
trace_args_apply(const char *arg)
{
	char *str;

	str = strdup(arg);
	if (str == NULL)
		return -1;

	if (rte_trace_regexp(str, true) < 0) {
		trace_err("cannot enable trace for %s", str);
		free(str);
		return -1;
	}

	free(str);
	return 0;
}

int
eal_trace_dir_args_save(char const *optarg)
{
	struct trace *trace = trace_obj_get();
	uint32_t size = sizeof(trace->dir);
	char *dir_path = NULL;
	int rc;

	if (optarg == NULL) {
		trace_err("no optarg is passed");
		return -EINVAL;
	}

	if (strlen(optarg) >= size) {
		trace_err("input string is too big");
		return -ENAMETOOLONG;
	}

	dir_path = (char *)calloc(1, size);
	if (dir_path == NULL) {
		trace_err("fail to allocate memory");
		return -ENOMEM;
	}

	sprintf(dir_path, "%s/", optarg);
	rc = trace_dir_update(dir_path);

	free(dir_path);
	return rc;
}

int
trace_epoch_time_save(void)
{
	struct trace *trace = trace_obj_get();
	struct timespec epoch = { 0, 0 };
	uint64_t avg, start, end;

	start = rte_get_tsc_cycles();
	if (clock_gettime(CLOCK_REALTIME, &epoch) < 0) {
		trace_err("failed to get the epoch time");
		return -1;
	}
	end = rte_get_tsc_cycles();
	avg = (start + end) >> 1;

	trace->epoch_sec = (uint64_t) epoch.tv_sec;
	trace->epoch_nsec = (uint64_t) epoch.tv_nsec;
	trace->uptime_ticks = avg;

	return 0;
}

static int
trace_dir_default_path_get(char *dir_path)
{
	struct trace *trace = trace_obj_get();
	uint32_t size = sizeof(trace->dir);
	struct passwd *pwd;
	char *home_dir;

	/* First check for shell environment variable */
	home_dir = getenv("HOME");
	if (home_dir == NULL) {
		/* Fallback to password file entry */
		pwd = getpwuid(getuid());
		if (pwd == NULL)
			return -EINVAL;

		home_dir = pwd->pw_dir;
	}

	/* Append dpdk-traces to directory */
	if (snprintf(dir_path, size, "%s/dpdk-traces/", home_dir) < 0)
		return -ENAMETOOLONG;

	return 0;
}

int
trace_mkdir(void)
{
	struct trace *trace = trace_obj_get();
	char session[TRACE_DIR_STR_LEN];
	char *dir_path;
	int rc;

	if (!trace->dir_offset) {
		dir_path = calloc(1, sizeof(trace->dir));
		if (dir_path == NULL) {
			trace_err("fail to allocate memory");
			return -ENOMEM;
		}

		rc = trace_dir_default_path_get(dir_path);
		if (rc < 0) {
			trace_err("fail to get default path");
			free(dir_path);
			return rc;
		}

		rc = trace_dir_update(dir_path);
		free(dir_path);
		if (rc < 0)
			return rc;
	}

	/* Create the path if it t exist, no "mkdir -p" available here */
	rc = mkdir(trace->dir, 0700);
	if (rc < 0 && errno != EEXIST) {
		trace_err("mkdir %s failed [%s]", trace->dir, strerror(errno));
		rte_errno = errno;
		return -rte_errno;
	}

	rc = trace_session_name_generate(session);
	if (rc < 0)
		return rc;
	rc = trace_dir_update(session);
	if (rc < 0)
		return rc;

	rc = mkdir(trace->dir, 0700);
	if (rc < 0) {
		trace_err("mkdir %s failed [%s]", trace->dir, strerror(errno));
		rte_errno = errno;
		return -rte_errno;
	}

	RTE_LOG(INFO, EAL, "Trace dir: %s\n", trace->dir);
	return 0;
}

static int
trace_meta_save(struct trace *trace)
{
	char file_name[PATH_MAX];
	FILE *f;
	int rc;

	rc = snprintf(file_name, PATH_MAX, "%s/metadata", trace->dir);
	if (rc < 0)
		return rc;

	f = fopen(file_name, "w");
	if (f == NULL)
		return -errno;

	rc = rte_trace_metadata_dump(f);

	if (fclose(f))
		rc = -errno;

	return rc;
}


static inline int
trace_file_sz(struct __rte_trace_header *hdr)
{
	return sizeof(struct __rte_trace_stream_header) + hdr->offset;
}

static int
trace_mem_save(struct trace *trace, struct __rte_trace_header *hdr,
		uint32_t cnt)
{
	char file_name[PATH_MAX];
	FILE *f;
	int rc;

	rc = snprintf(file_name, PATH_MAX, "%s/channel0_%d", trace->dir, cnt);
	if (rc < 0)
		return rc;

	f = fopen(file_name, "w");
	if (f == NULL)
		return -errno;

	rc = fwrite(&hdr->stream_header, trace_file_sz(hdr), 1, f);
	rc = (rc == 1) ?  0 : -EACCES;

	if (fclose(f))
		rc = -errno;

	return rc;
}

int
rte_trace_save(void)
{
	struct trace *trace = trace_obj_get();
	struct __rte_trace_header *header;
	uint32_t count;
	int rc = 0;

	if (trace->nb_trace_mem_list == 0)
		return rc;

	rc = trace_meta_save(trace);
	if (rc)
		return rc;

	rte_spinlock_lock(&trace->lock);
	for (count = 0; count < trace->nb_trace_mem_list; count++) {
		header = trace->lcore_meta[count].mem;
		rc =  trace_mem_save(trace, header, count);
		if (rc)
			break;
	}
	rte_spinlock_unlock(&trace->lock);
	return rc;
}