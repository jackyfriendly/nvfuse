/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*/

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>

#include <sys/types.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_eal.h>
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include <rte_lcore.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_aio.h"
#include "nvfuse_misc.h"
#include "nvfuse_debug.h"

#define DEINIT_IOM	1
#define UMOUNT		1

#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

#define RANDOM		1
#define SEQUENTIAL	0

#define MAX_AIO_CTX	256

/* global ipc_context */
struct nvfuse_ipc_context _g_ipc_ctx;
struct nvfuse_ipc_context *g_ipc_ctx = &_g_ipc_ctx;
/* global params */
struct nvfuse_params _g_params;
struct nvfuse_params *g_params = &_g_params;

int perf_aio(struct nvfuse_handle *nvh, s64 file_size, s32 block_size, s32 is_rand, s32 is_read,
	     s32 direct, s32 qdepth, s32 runtime);
void perf_usage(char *cmd);
void _print_stats(struct perf_stat_aio *cur_stat, char *name);

static s32 nvfuse_aio_test_rw(struct nvfuse_handle *nvh, s8 *str, s64 file_size, u32 io_size,
		       u32 qdepth, u32 is_read, u32 is_direct, u32 is_rand, s32 runtime)
{
	struct nvfuse_aio_queue aioq;
	struct nvfuse_aio_req *list[MAX_AIO_CTX];
	s32 cur_depth = 0;
	s32 cnt, idx;
	s32 ret;

	s32 last_progress = 0;
	s32 curr_progress = 0;
	s32 flags;
	s64 file_allocated_size;
	struct stat stat_buf;
	struct timeval tv;
	struct user_context user_ctx;

	user_ctx.file_size = file_size;
	user_ctx.io_size = io_size;
	user_ctx.qdepth = qdepth;
	user_ctx.is_read = is_read;
	user_ctx.is_rand = is_rand;

	dprintf_info(AIO, " aiotest %s filesize = %0.3fMB io_size = %d qdpeth = %d (%c) direct (%d)\n", str,
	       (double)file_size / (1024 * 1024), io_size, qdepth, is_read ? 'R' : 'W', is_direct);

	flags = O_RDWR | O_CREAT;
	if (is_direct)
		flags |= O_DIRECT;

	user_ctx.fd = nvfuse_openfile_path(nvh, str, flags, 0);
	if (user_ctx.fd < 0) {
		dprintf_info(AIO, " Error: file open or create \n");
		return -1;
	}

	dprintf_info(AIO, " start fallocate %s size %lu \n", str, (long)file_size);
	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);

	/* temp test */
	//nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	dprintf_info(AIO, " finish fallocate %s size %lu \n", str, (long)file_size);

	ret = nvfuse_getattr(nvh, str, &stat_buf);
	if (ret) {
		dprintf_error(AIO, " No such file %s\n", str);
		return -1;
	}
	/* NOTE: Allocated size may differ from requested size. */
	file_allocated_size = stat_buf.st_size;

	dprintf_info(AIO, " requested size %ldMB.\n", (long)file_size / NVFUSE_MEGA_BYTES);
	dprintf_info(AIO, " allocated size %ldMB.\n", (long)file_allocated_size / NVFUSE_MEGA_BYTES);

#if (NVFUSE_OS == NVFUSE_OS_LINUX)
	file_size = file_allocated_size;
#endif
	/* initialization of aio queue */
	ret = nvfuse_aio_queue_init(nvh->nvh_target, &aioq, qdepth);
	if (ret) {
		dprintf_error(AIO, " Error: aio queue init () with ret = %d\n ", ret);
		return -1;
	}

	user_ctx.io_curr = 0;
	user_ctx.io_remaining = file_size;

	/* user data buffer allocation */
	user_ctx.user_buf = nvfuse_alloc_aligned_buffer(io_size * qdepth);
	if (user_ctx.user_buf == NULL) {
		dprintf_error(AIO, " Error: malloc()\n");
		return -1;
	}
	user_ctx.buf_ptr = 0;

	gettimeofday(&tv, NULL);
	while (user_ctx.io_remaining > 0 || cur_depth) {
		//dprintf_info(AIO, " total depth = %d arq depth = %d\n", cur_depth, aioq.arq_cur_depth);
        for (idx = 0; idx < MAX_AIO_CTX; idx++) {
            if (cur_depth >= (s32)qdepth || user_ctx.io_remaining == 0)
                break;

            list[idx] = nvfuse_aio_test_alloc_req(nvh, &user_ctx);
            if (list[idx] == NULL) 
                break;

            cur_depth++;
        }

		/* progress bar */
		curr_progress = (user_ctx.io_curr * 100 / file_size);
		if (curr_progress != last_progress) {
			printf(".");
			if (curr_progress % 10 == 0) {
				printf("%d%% %.3fMB/s\n", curr_progress,
				       (double)user_ctx.io_curr / NVFUSE_MEGA_BYTES / nvfuse_time_since_now(&tv));
			}
			fflush(stdout);
			last_progress = curr_progress;
		}

        /* aio submission */
        ret = nvfuse_io_submit(nvh, &aioq, idx, list);
        if (ret) {
            dprintf_error(AIO, " Error: queue submision \n");
            goto CLOSE_FD;
        }

RETRY_WAIT_COMPLETION:
		//dprintf_info(AIO, " Submission depth = %d\n", cur_depth);
		/* aio completion */
		cnt = nvfuse_io_getevents(&nvh->nvh_sb, &aioq, 1, MAX_AIO_CTX, list);
        for (idx = 0; idx < cnt; idx++) {
			cur_depth--;
            nvfuse_aio_test_callback(list[idx]);
        }

		if (runtime && nvfuse_time_since_now(&tv) >= (double)runtime) {
			if (cur_depth) {
				goto RETRY_WAIT_COMPLETION;
			}
			break;
		}
	}

	assert(cur_depth == 0);

CLOSE_FD:
	nvfuse_aio_queue_deinit(nvh, &aioq);
	nvfuse_free_aligned_buffer(user_ctx.user_buf);
	nvfuse_fsync(nvh, user_ctx.fd);
	nvfuse_closefile(nvh, user_ctx.fd);

	return 0;
}

int perf_aio(struct nvfuse_handle *nvh, s64 file_size, s32 block_size, s32 is_rand, s32 is_read,
	     s32 direct, s32 qdepth, s32 runtime)
{
	struct timeval tv;
	char str[FNAME_SIZE];
	s32 res;

	sprintf(str, "file_allocate_test");

	gettimeofday(&tv, NULL);

	res = nvfuse_aio_test_rw(nvh, str, file_size, block_size, qdepth, is_read ? READ : WRITE, direct,
				 is_rand, runtime);
	if (res < 0) {
		printf(" Error: aio write test \n");
		goto AIO_ERROR;
	}

AIO_ERROR:
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0) {
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}

	return 0;
}

void perf_usage(char *cmd)
{
	printf("\nOptions for NVFUSE application: \n");
	printf("\t-S: file size (in MB)\n");
	printf("\t-B: block size (in B)\n");
	printf("\t-E: ioengine (e.g., libaio, sync)\n");
	printf("\t-Q: qdepth \n");
	printf("\t-R: random (e.g., rand or sequential)\n");
	printf("\t-D: direct I/O \n");
	printf("\t-W: write workload (e.g., write or read)\n");
}

#define USE_AIO 1
static int core_argc = 0;
static char *core_argv[128];
static int app_argc = 0;
static char *app_argv[128];

static int file_size;
static int block_size;
static int ioengine;
static int qdepth;
static int is_rand = 0; /* sequential set to as default */
static int direct_io = 0; /* buffered I/O set to as default */
static int is_write = 0; /* write workload set to as default */
static int runtime = 0; /* runtime in seconds */

void _print_stats(struct perf_stat_aio *cur_stat, char *name)
{
	double io_per_second, mb_per_second;
	double average_latency, min_latency, max_latency;
	u64 tsc_rate = spdk_get_ticks_hz();

	io_per_second = (double)cur_stat->aio_lat_total_count / ((double)cur_stat->aio_execution_tsc /
			tsc_rate);
	mb_per_second = (double)cur_stat->aio_total_size / (1024 * 1024) / ((double)
			cur_stat->aio_execution_tsc / tsc_rate);
	average_latency = (double)(cur_stat->aio_lat_total_tsc / cur_stat->aio_lat_total_count) * 1000 *
			  1000 / tsc_rate;
	min_latency = (double)cur_stat->aio_lat_min_tsc * 1000 * 1000 / tsc_rate;
	max_latency = (double)cur_stat->aio_lat_max_tsc * 1000 * 1000 / tsc_rate;

	printf("\n NVFUSE AIO Queue Perf Statistics. \n");
	printf(" %s stat result \n", name);
	printf("------------------------------------\n");
	printf(" %s exeuction time = %.3f sec\n", name, (double)cur_stat->aio_execution_tsc / tsc_rate);
	printf(" %s iops = %.0f IOPS (%.3f KIOPS)\n", name, io_per_second, io_per_second / 1024);
	printf(" %s bandwidth = %.3f MB/s\n", name, mb_per_second);
	printf(" %s avg latency = %.3f us \n", name, average_latency);
	printf(" %s min latency = %.3f us \n", name, min_latency);
	printf(" %s max latency = %.3f us \n", name, max_latency);
	printf(" %s usr cpu utilization = %3.0f %% (%f sec)\n", name,
	       (double)tv_to_sec(&cur_stat->aio_usr_time)
	       / ((double)cur_stat->aio_execution_tsc / tsc_rate) * 100,
	       (double)tv_to_sec(&cur_stat->aio_usr_time));
	printf(" %s sys cpu utilization = %3.0f %% (%f sec)\n", name,
	       (double)tv_to_sec(&cur_stat->aio_sys_time)
	       / ((double)cur_stat->aio_execution_tsc / tsc_rate) * 100,
	       (double)tv_to_sec(&cur_stat->aio_sys_time));

	printf("------------------------------------\n");
}

static void print_stats(s32 num_cores)
{
	struct rte_ring *stat_rx_ring;
	struct rte_mempool *stat_message_pool;
	union perf_stat *per_core_stat, *cur_stat, sum_stat;
	s32 ret;
	s32 cur;
	s8 name[128];

	per_core_stat = malloc(sizeof(union perf_stat) * num_cores);
	if (per_core_stat == NULL) {
		fprintf(stderr, " Error: malloc() \n");
	}

	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, AIO_STAT);
	if (ret < 0)
		return;

	memset(per_core_stat, 0x00, sizeof(union perf_stat) * num_cores);
	memset(&sum_stat, 0x00, sizeof(union perf_stat));
	sum_stat.stat_aio.aio_lat_min_tsc = ~0;
	sum_stat.stat_aio.aio_lat_max_tsc = 0;


	for (cur = 0; cur < num_cores; cur++) {
		cur_stat = per_core_stat + cur;
		ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)cur_stat);
		if (ret < 0)
			return;

		sprintf(name, "lcore %d", cur);

		_print_stats((struct perf_stat_aio *)cur_stat, name);

		sum_stat.stat_aio.aio_execution_tsc += cur_stat->stat_aio.aio_execution_tsc;
		sum_stat.stat_aio.aio_lat_total_count += cur_stat->stat_aio.aio_lat_total_count;	// io count
		sum_stat.stat_aio.aio_lat_total_tsc += cur_stat->stat_aio.aio_lat_total_tsc;		// latency total
		sum_stat.stat_aio.aio_total_size += cur_stat->stat_aio.aio_total_size;			// io amount
		sum_stat.stat_aio.aio_lat_min_tsc =
			MIN(sum_stat.stat_aio.aio_lat_min_tsc, cur_stat->stat_aio.aio_lat_min_tsc);
		sum_stat.stat_aio.aio_lat_max_tsc =
			MAX(sum_stat.stat_aio.aio_lat_max_tsc, cur_stat->stat_aio.aio_lat_max_tsc);

		timeval_add(&sum_stat.stat_aio.aio_sys_time, &cur_stat->stat_aio.aio_sys_time);
		timeval_add(&sum_stat.stat_aio.aio_usr_time, &cur_stat->stat_aio.aio_usr_time);

		nvfuse_rusage_add(&sum_stat.stat_aio.aio_result_rusage, &cur_stat->stat_aio.aio_result_rusage);
	}

	sum_stat.stat_aio.aio_execution_tsc /= num_cores;
	sprintf(name, "group");
	_print_stats((struct perf_stat_aio *)&sum_stat, name);

	free(per_core_stat);
}

static void perf_run(void *arg1, void *arg2)
{
	struct nvfuse_handle *nvh;

	printf(" start perf (lcore = %d)...\n", rte_lcore_id());

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(g_ipc_ctx, g_params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return;
	}

	printf("\n");

	if (ioengine == AIO) {
		perf_aio(nvh, ((s64)file_size * MB), block_size, is_rand, is_write ? WRITE : READ, direct_io,
			       qdepth, runtime);
	} else {
		printf(" sync io is not supported \n");;
	}

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);

	spdk_app_stop(0);
	exit(0);
}

static void reactor_run(void *arg)
{
	struct spdk_event *event;
	u32 i;

	/* Send events to start all I/O */
	SPDK_ENV_FOREACH_CORE(i) {
		printf(" allocate event on lcore = %d \n", i);
		if (i == 1) {
			event = spdk_event_allocate(i, perf_run,
						    NULL, NULL);
			spdk_event_call(event);
		}
	}
}

int main(int argc, char *argv[])
{
	int ret = 0;
	char op;

	if (argc == 1) {
		goto INVALID_ARGS;
	}

	printf(" %s (pid %d, parent pid %d)\n", argv[0], getpid(), getppid());

	/* distinguish cmd line into core args and app args */
	nvfuse_distinguish_core_and_app_options(argc, argv,
						&core_argc, core_argv,
						&app_argc, app_argv);

	/* core parameter paser */
	ret = nvfuse_parse_args(core_argc, core_argv, g_params);
	if (ret < 0)
		return -1;

	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(app_argc, app_argv, "S:B:E:Q:RDWT:")) != -1) {
		switch (op) {
		case 'S':
			file_size = atoi(optarg);
			break;
		case 'B':
			block_size = atoi(optarg);
			if ((block_size % CLUSTER_SIZE) | (block_size < CLUSTER_SIZE)) {
				printf("\n Error: block size (%d) is not alinged with 4KB\n", block_size);
				goto INVALID_ARGS;
			}
			break;
		case 'E':
			if (!strcmp(optarg, "libaio")) {
				ioengine = AIO;
			} else if (!strcmp(optarg, "sync")) {
				ioengine = SYNC;
			} else {
				fprintf(stderr, "\n Invalid ioengine type = %s", optarg);
				goto INVALID_ARGS;
			}
			break;
		case 'Q':
			qdepth = atoi(optarg);
			if (qdepth == 0) {
				fprintf(stderr, "\n Invalid qdepth = %d\n", qdepth);
				goto INVALID_ARGS;
			}
			break;
		case 'R':
			is_rand = 1;
			break;
		case 'D':
			direct_io = 1;
			break;
		case 'W':
			is_write = 1;
			break;
		case 'T':
			runtime = atoi(optarg);
			if (runtime == 0) {
				fprintf(stderr, "\n Invalid runtime = %d\n", runtime);
				goto INVALID_ARGS;
			}
			break;
		default:
			goto INVALID_ARGS;
		}
	}

	ret = nvfuse_configure_spdk(g_ipc_ctx, g_params, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

#ifndef NVFUSE_USE_CEPH_SPDK
	spdk_app_start(&g_params->opts, reactor_run, NULL);
#else
	spdk_app_start(reactor_run, NULL);
#endif

	spdk_app_fini();

	print_stats(1);

	return 0;

INVALID_ARGS:
	;
	nvfuse_core_usage(argv[0]);
	perf_usage(argv[0]);
	nvfuse_core_usage_example(argv[0]);
	return -1;
}
