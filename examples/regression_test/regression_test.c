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
#include <assert.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_aio.h"
#include "nvfuse_misc.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_debug.h"
#include "spdk/env.h"
#include <rte_lcore.h>

#define DEINIT_IOM	1
#define UMOUNT		1

#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

#if 0
#define MB (1024*1024)
#define GB (1024*1024*1024)
#define TB ((s64)1024*1024*1024*1024)
#endif

#define RT_TEST_TYPE MILL_TEST

#define MAX_TEST    1
/* quick test */
#define QUICK_TEST  2
/* 1 million create/delete test */
#define MILL_TEST   3

#define MAX_AIO_CTX	256

/* global ipc_context */
static struct nvfuse_ipc_context _g_ipc_ctx;
static struct nvfuse_ipc_context *g_ipc_ctx = &_g_ipc_ctx;
/* global params */
static struct nvfuse_params _g_params;
static struct nvfuse_params *g_params = &_g_params;

static s32 last_percent;
static s32 test_type = QUICK_TEST;

void rt_progress_reset(void);
void rt_progress_report(s32 curr, s32 max);
char *rt_decode_test_type(s32 type);
int rt_create_files(struct nvfuse_handle *nvh, u32 arg);
int rt_create_dirs(struct nvfuse_handle *nvh, u32 arg);
int rt_create_max_sized_file(struct nvfuse_handle *nvh, u32 arg);
int rt_gen_aio_rw(struct nvfuse_handle *nvh, s64 file_size, s32 block_size, s32 is_rand, s32 direct,
		  s32 qdepth);
int rt_create_max_sized_file_aio_4KB(struct nvfuse_handle *nvh, u32 is_rand);
int rt_create_max_sized_file_aio_128KB(struct nvfuse_handle *nvh, u32 is_rand);
int rt_create_4KB_files(struct nvfuse_handle *nvh, u32 arg);
void rt_usage(char *cmd);
static int rt_main(void *arg);
static void print_stats(s32 num_cores, s32 num_tc);
void regression_run(void *arg1, void *arg2);
void reactor_run(void* arg);
int main(int argc, char *argv[]);

void rt_progress_reset(void)
{
	last_percent = 0;
}

void rt_progress_report(s32 curr, s32 max)
{
	int curr_percent;

	/* FIXME: */
	//if (rte_lcore_id() == 1) {
	curr_percent = (curr + 1) * 100 / max;

	if	(curr_percent != last_percent) {
		last_percent = curr_percent;
		printf(".");
		if (curr_percent % 10 == 0)
			printf("%d%%\n", curr_percent);
		fflush(stdout);
	}
	//}
}

char *rt_decode_test_type(s32 type)
{
	switch (type) {
	case MAX_TEST:
		return "MAX_TEST";
	case QUICK_TEST:
		return "QUICK_TEST";
	case MILL_TEST:
		return "MILL_TEST";
	}

	return NULL;
}

int rt_create_files(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 fd;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		max_inodes = stat.f_ffree; /* # of free inodes */
		break;
	case QUICK_TEST:
		max_inodes = 100;
		break;
	case MILL_TEST:
		/* # of free inodes */
		max_inodes = stat.f_ffree < 1000000 ? stat.f_ffree : 1000000;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	nvh->nvh_sb.bp_set_index_tsc = 0;
	nvh->nvh_sb.bp_set_index_count = 0;
	nvh->nvh_sb.nvme_io_tsc = 0;
	nvh->nvh_sb.nvme_io_count = 0;

	/* create null files */
	printf(" Start: creating null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		sprintf(buf, "file%d\n", i);

		fd = nvfuse_openfile_path(nvh, buf, O_RDWR | O_CREAT, 0);
		if (fd == -1) {
			printf(" Error: open() \n");
			return -1;
		}
		nvfuse_closefile(nvh, fd);
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	printf(" Finish: creating null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));
	printf(" bp tree cpu = %f sec\n",
	       (double)nvh->nvh_sb.bp_set_index_tsc / (double)spdk_get_ticks_hz());
	printf(" sync meta i/o = %f sec\n", (double)nvh->nvh_sb.nvme_io_tsc / (double)spdk_get_ticks_hz());

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* lookup null files */
	printf(" Start: looking up null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		struct stat st_buf;
		int res;

		sprintf(buf, "file%d\n", i);

		res = nvfuse_getattr(nvh, buf, &st_buf);
		if (res) {
			printf(" No such file %s\n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: looking up null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* delete null files */
	printf(" Start: deleting null files (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		sprintf(buf, "file%d\n", i);

		res = nvfuse_rmfile_path(nvh, buf);
		if (res) {
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: deleting null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));

	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

#if 0
	nvfuse_print_ictx_list_count(&nvh->nvh_sb, BUFFER_TYPE_DIRTY);
	nvfuse_print_ictx_list(&nvh->nvh_sb, BUFFER_TYPE_DIRTY);
#endif

	return 0;
}

int rt_create_dirs(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs stat;
	s8 buf[FNAME_SIZE];
	s32 max_inodes;
	s32 i;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &stat) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		max_inodes = stat.f_ffree; /* # of free inodes */
		break;
	case QUICK_TEST:
		max_inodes = 100;
		break;
	case MILL_TEST:
		/* # of free inodes */
		max_inodes = stat.f_ffree < 1000000 ? stat.f_ffree : 1000000;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* create null directories */
	printf(" Start: creating null directories (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		sprintf(buf, "dir%d\n", i);
		res = nvfuse_mkdir_path(nvh, buf, 0644);
		if (res < 0) {
			printf(" Error: create dir = %s \n", buf);
			return res;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	printf(" Finish: creating null directories (0x%x) %.3f OPS (%.f sec). \n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));
	printf(" bp tree cpu = %f sec\n",
	       (double)nvh->nvh_sb.bp_set_index_tsc / (double)spdk_get_ticks_hz());
	printf(" sync meta i/o = %f sec\n", (double)nvh->nvh_sb.nvme_io_tsc / (double)spdk_get_ticks_hz());

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);
	/* lookup null directories */
	printf(" Start: looking up null directories (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		struct stat st_buf;
		int res;

		sprintf(buf, "dir%d\n", i);

		res = nvfuse_getattr(nvh, buf, &st_buf);
		if (res) {
			printf(" No such directory %s\n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}
	printf(" Finish: looking up null directories (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);
	/* delete null directories */
	printf(" Start: deleting null directories (0x%x).\n", max_inodes);
	for (i = 0; i < max_inodes; i++) {
		sprintf(buf, "dir%d\n", i);

		res = nvfuse_rmdir_path(nvh, buf);
		if (res) {
			printf(" rmfile = %s error \n", buf);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, max_inodes);
	}

	nvfuse_check_flush_dirty(&nvh->nvh_sb, 1);

	printf(" Finish: deleting null files (0x%x) %.3f OPS (%.f sec).\n", max_inodes,
	       max_inodes / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));

#if 0
	nvfuse_print_ictx_list_count(&nvh->nvh_sb, BUFFER_TYPE_DIRTY);
	nvfuse_print_ictx_list(&nvh->nvh_sb, BUFFER_TYPE_DIRTY);
#endif

	return 0;
}

int rt_create_max_sized_file(struct nvfuse_handle *nvh, u32 arg)
{
	struct statvfs statvfs_buf;
	struct stat stat_buf;
	char str[128];
	struct timeval tv;
	s64 file_size;
	s64 file_allocated_size;
	s32 res;
	s32 fid;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	sprintf(str, "file_allocate_test");

	switch (test_type) {
	case MAX_TEST:
		file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;
		break;
	case QUICK_TEST:
		file_size = 100 * MB;
		break;
	case MILL_TEST:
		file_size = (s64)1 * TB;
		file_size = (file_size > (s64)statvfs_buf.f_bfree * CLUSTER_SIZE) ?
			    (s64)(statvfs_buf.f_bfree / 2) * CLUSTER_SIZE :
			    (s64)file_size;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	fid = nvfuse_openfile_path(nvh, str, O_RDWR | O_CREAT, 0);
	if (fid < 0) {
		printf(" Error: file open or create \n");
		return -1;
	}
	nvfuse_closefile(nvh, fid);

	gettimeofday(&tv, NULL);
	printf("\n Start: Fallocate and Deallocate (file %s size %luMB). \n", str, (long)file_size / MB);
	/* pre-allocation of data blocks*/
	nvfuse_fallocate(nvh, str, 0, file_size);

	res = nvfuse_getattr(nvh, str, &stat_buf);
	if (res) {
		printf(" No such file %s\n", str);
		return -1;
	}

	/* NOTE: Allocated size may differ from requested size. */
	file_allocated_size = stat_buf.st_size;

	printf(" requested size %ldMB.\n", (long)file_size / MB);
	printf(" allocated size %ldMB.\n", (long)file_allocated_size / MB);

	printf(" nvfuse fallocate throughput %.3fMB/s (%0.3fs).\n",
	       (double)file_allocated_size / MB / nvfuse_time_since_now(&tv), nvfuse_time_since_now(&tv));

	gettimeofday(&tv, NULL);
	printf(" Start: rmfile %s size %luMB \n", str, (long)file_allocated_size / MB);
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0) {
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}
	printf(" nvfuse rmfile throughput %.3fMB/s\n",
	       (double)file_allocated_size / MB / nvfuse_time_since_now(&tv));

	printf("\n Finish: Fallocate and Deallocate.\n");

	return NVFUSE_SUCCESS;
}

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
				printf("%d%% %.3fMB avg req\n", curr_progress,
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

int rt_gen_aio_rw(struct nvfuse_handle *nvh, s64 file_size, s32 block_size, s32 is_rand, s32 direct,
		  s32 qdepth)
{
	struct timeval tv;
	char str[FNAME_SIZE];
	s32 res;

	sprintf(str, "file_allocate_test");

	gettimeofday(&tv, NULL);

	/* write phase */
	{
		res = nvfuse_aio_test_rw(nvh, str, file_size, block_size, qdepth, WRITE, direct, is_rand, 0);
		if (res < 0) {
			printf(" Error: aio write test \n");
			goto AIO_ERROR;
		}
		printf(" nvfuse aio write through %.3f MB/s\n", (double)file_size / MB / nvfuse_time_since_now(&tv));

		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			printf(" Error: rmfile = %s\n", str);
			return -1;
		}
	}

	gettimeofday(&tv, NULL);
	/* read phase */
	{
		res = nvfuse_aio_test_rw(nvh, str, file_size, block_size, qdepth, READ, direct, is_rand, 0);
		if (res < 0) {
			printf(" Error: aio read test \n");
			goto AIO_ERROR;
		}
		printf(" nvfuse aio read through %.3f MB/s\n", (double)file_size / MB / nvfuse_time_since_now(&tv));

		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			printf(" Error: rmfile = %s\n", str);
			return -1;
		}
	}

	return 0;

AIO_ERROR:
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0) {
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}
	return -1;
}

int rt_create_max_sized_file_aio_4KB(struct nvfuse_handle *nvh, u32 is_rand)
{
	struct statvfs statvfs_buf;
	s32 direct;
	s32 qdepth;
	s64 file_size;
	s32 block_size;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;
		break;
	case QUICK_TEST:
		file_size = 100 * MB;
		break;
	case MILL_TEST:
		file_size = (s64)128 * GB;
		file_size = (file_size > (s64)statvfs_buf.f_bfree * CLUSTER_SIZE) ?
			    (s64)(statvfs_buf.f_bfree / 2) * CLUSTER_SIZE :
			    (s64)file_size;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	direct = 1;
	qdepth = 128;
	block_size = 4096;
	res = rt_gen_aio_rw(nvh, file_size, block_size, is_rand, direct, qdepth);

	return res;
}

int rt_create_max_sized_file_aio_128KB(struct nvfuse_handle *nvh, u32 is_rand)
{
	struct statvfs statvfs_buf;
	s32 direct;
	s32 qdepth;
	s64 file_size;
	s32 block_size;
	s32 res;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0) {
		printf(" statfs error \n");
		return -1;
	}

	switch (test_type) {
	case MAX_TEST:
		file_size = (s64) statvfs_buf.f_bfree * CLUSTER_SIZE;
		break;
	case QUICK_TEST:
		file_size = 100 * MB;
		break;
	case MILL_TEST:
		file_size = (s64)128 * GB;
		file_size = (file_size > (s64)statvfs_buf.f_bfree * CLUSTER_SIZE) ?
			    (s64)(statvfs_buf.f_bfree / 2) * CLUSTER_SIZE :
			    (s64)file_size;
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	direct = 1;
	qdepth = 128;
	block_size = 128 * 1024;
	res = rt_gen_aio_rw(nvh, file_size, block_size, is_rand, direct, qdepth);

	return res;
}

int rt_create_4KB_files(struct nvfuse_handle *nvh, u32 arg)
{
	struct timeval tv;
	struct statvfs statvfs_buf;
	char str[FNAME_SIZE];
	s32 res;
	s32 nr;
	int i;

	if (nvfuse_statvfs(nvh, NULL, &statvfs_buf) < 0) {
		printf(" statfs error \n");
		return -1;
	}

#if (RT_TEST_TYPE == MAX_TEST)

#elif (RT_TEST_TYPE == QUICK_TEST)

#elif (RT_TEST_TYPE == MILL_TEST)

#endif
	switch (test_type) {
	case MAX_TEST:
		nr = statvfs_buf.f_bfree / 2;
		break;
	case QUICK_TEST:
		nr = 100;
		break;
	case MILL_TEST:
		nr = statvfs_buf.f_bfree / 2;
		if (nr > 1000000) {
			nr = 1000000;
		}
		break;
	default:
		printf(" Invalid test type = %d\n", test_type);
		return -1;
	}

	printf(" # of files = %d \n", nr);

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	printf(" Start: creating 4KB files (0x%x).\n", nr);
	/* create files*/
	for (i = 0; i < nr; i++) {
		sprintf(str, "file%d", i);
		res = nvfuse_mkfile(nvh, str, "4096");
		if (res < 0) {
			printf(" mkfile error = %s\n", str);
			return -1;
		}

		/* update progress percent */
		rt_progress_report(i, nr);
	}
	printf(" Finish: creating 4KB files (0x%x) %.3f OPS (%0.3fs).\n", nr, nr / nvfuse_time_since_now(&tv),
	       nvfuse_time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	printf(" Start: looking up 4KB files (0x%x).\n", nr);
	/* lookup files */
	for (i = 0; i < nr; i++) {
		struct stat st_buf;
		int res;

		sprintf(str, "file%d", i);
		res = nvfuse_getattr(nvh, str, &st_buf);
		if (res) {
			printf(" No such file %s\n", str);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, nr);
	}
	printf(" Finish: looking up 4KB files (0x%x) %.3f OPS (%0.3fs).\n", nr, nr / nvfuse_time_since_now(&tv),
	       nvfuse_time_since_now(&tv));

	/* reset progress percent */
	rt_progress_reset();
	gettimeofday(&tv, NULL);

	/* delete files */
	printf(" Start: deleting 4KB files (0x%x).\n", nr);
	for (i = 0; i < nr; i++) {
		sprintf(str, "file%d", i);
		res = nvfuse_rmfile_path(nvh, str);
		if (res < 0) {
			printf(" rmfile error = %s \n", str);
			return -1;
		}
		/* update progress percent */
		rt_progress_report(i, nr);
	}
	printf(" Finish: deleting 4KB files (0x%x) %.3f OPS (%0.3fs).\n", nr, nr / nvfuse_time_since_now(&tv),
	       nvfuse_time_since_now(&tv));

	return NVFUSE_SUCCESS;

}

#define RANDOM		1
#define SEQUENTIAL	0

struct regression_test_ctx {
	s32(*function)(struct nvfuse_handle *nvh, u32 arg);
	s8 test_name[128];
	s32 arg;
	s32 pass_criteria; /* compare return code */
	s32 pass_criteria_ignore; /* no compare */
}
rt_ctx[] = {
	{ rt_create_files, "Creating Max Number of Files.", 0, 0, 0},
	{ rt_create_dirs, "Creating Max Number of Directories.", 0, 0, 0},
	{ rt_create_max_sized_file, "Creating Maximum Sized Single File.", 0, 0, 0},
	{ rt_create_max_sized_file_aio_4KB, "Creating Maximum Sized Single File with 4KB Sequential AIO Read and Write.", SEQUENTIAL, 0, 0},
	{ rt_create_max_sized_file_aio_4KB, "Creating Maximum Sized Single File with 4KB Random AIO Read and Write.", RANDOM, 0, 0},
	{ rt_create_max_sized_file_aio_128KB, "Creating Maximum Sized Single File with 128KB Sequential AIO Read and Write.", SEQUENTIAL, 0, 0 },
	{ rt_create_max_sized_file_aio_128KB, "Creating Maximum Sized Single File with 128KB Random AIO Read and Write.", RANDOM, 0, 0 },
	{ rt_create_4KB_files, "Creating 4KB files with fsync.", 0, 0, 0}
};

void rt_usage(char *cmd)
{
	printf("\nOptions for NVFUSE application: \n");
	printf("\t-T: test type (e.g., 1: max_test, 2: quick_test, 3: million test \n");
}

static int rt_main(void *arg)
{
	struct nvfuse_handle *nvh;
	struct regression_test_ctx *cur_rt_ctx;
	struct rte_ring *stat_rx_ring;
	struct rte_mempool *stat_message_pool;
	union perf_stat perf_stat;
	union perf_stat _perf_stat_rusage;
	struct perf_stat_rusage *rusage_stat;
	struct timeval tv;
	double execution_time;
	s32 ret;
	s32 id = (s32)(s64 *)arg;

	printf(" Perform test %s thread id = %d... \n", rt_decode_test_type(test_type), id);

	rusage_stat = (struct perf_stat_rusage *) &_perf_stat_rusage;

	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(g_ipc_ctx, g_params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, (struct rte_mempool **)&stat_message_pool, RT_STAT);
	if (ret < 0)
		return -1;

	printf("\n");

	cur_rt_ctx = rt_ctx;

	getrusage(RUSAGE_THREAD, &rusage_stat->start);
	/* Test Case Handler with Regression Test Context Array */
	while (cur_rt_ctx < rt_ctx + NUM_ELEMENTS(rt_ctx)) {
		s32 index = cur_rt_ctx - rt_ctx + 1;

		printf(" lcore = %d Regression Test %d: %s\n", rte_lcore_id(), index, cur_rt_ctx->test_name);
		gettimeofday(&tv, NULL);
		ret = cur_rt_ctx->function(nvh, cur_rt_ctx->arg);
		if (!cur_rt_ctx->pass_criteria &&
		    ret != cur_rt_ctx->pass_criteria) {
			printf(" Failed Regression Test %d.\n", index);
			goto RET;
		}

		execution_time = nvfuse_time_since_now(&tv);

		memset(&perf_stat, 0x00, sizeof(union perf_stat));

		perf_stat.stat_rt.stat_type = RT_STAT;
		perf_stat.stat_rt.lcore_id = (s32)arg;
		perf_stat.stat_rt.sequence = (index - 1);
		perf_stat.stat_rt.total_time = execution_time;

		printf(" rt stat sequence = %d\n", index - 1);
		nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, &perf_stat);

		printf(" lcore = %d Regression Test %d: passed successfully.\n\n", rte_lcore_id(), index);
		cur_rt_ctx++;
	}
	/* rusage */
	getrusage(RUSAGE_THREAD, &rusage_stat->end);
	nvfuse_rusage_diff(&rusage_stat->start, &rusage_stat->end, &rusage_stat->result);
	print_rusage(&rusage_stat->result, "test", 1, execution_time);
	//rusage_stat->tag = 0xDEADDEAD;

	{
		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
		if (ret < 0)
			return -1;

		nvfuse_stat_ring_put(stat_rx_ring, stat_message_pool, (union perf_stat *)rusage_stat);
	}

	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
RET:
	return 0;
}

static void print_stats(s32 num_cores, s32 num_tc)
{
	struct rte_ring *stat_rx_ring;
	struct rte_mempool *stat_message_pool;
	union perf_stat *per_core_stat, *cur_stat, sum_stat, temp_stat;
	s32 ret;
	s32 cur;
	s32 tc;
	s32 i;
	s8 name[128];
	double group_exec_time = 0.0;

	per_core_stat = malloc(sizeof(union perf_stat) * num_cores * num_tc);
	if (per_core_stat == NULL) {
		fprintf(stderr, " Error: malloc() \n");
	}

	/* stat ring lookup */
	ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RT_STAT);
	if (ret < 0) {
		printf(" Error ring lookup error in %s\n", __FUNCTION__);
		return;
	}

	memset(per_core_stat, 0x00, sizeof(union perf_stat) * num_cores * num_tc);
	memset(&sum_stat, 0x00, sizeof(union perf_stat));

#if 0
	printf(" perf stat size = %d \n", sizeof(union perf_stat));
	printf(" rt stat size = %d \n", sizeof(struct perf_stat_rt));
	printf(" aio stat size = %d \n", sizeof(struct perf_stat_aio));
	printf(" ipc stat msg = %d \n", PERF_STAT_SIZE);
#endif

	/* gather rt stats */
	for (i = 0; i < num_cores * num_tc; i++) {
		ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
		if (ret < 0)
			return;

		assert(temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id < num_cores * num_tc);

#if 0
		printf(" stat type = %d \n", temp_stat.stat_rt.stat_type);
		printf(" seq = %d \n", temp_stat.stat_rt.sequence);
		printf(" core = %d \n", temp_stat.stat_rt.lcore_id);
		printf(" index = %d \n", (temp_stat.stat_rt.sequence * num_cores + temp_ssprintf(name, "Avg", i);
					  print_rusage(&sum_stat->result, name, num_cores, group_exec_time); tat.stat_rt.lcore_id));
		printf(" \n");
#endif
		cur_stat = per_core_stat + (temp_stat.stat_rt.sequence * num_cores + temp_stat.stat_rt.lcore_id);
		memcpy(cur_stat, &temp_stat, sizeof(union perf_stat));
	}

	for (tc = 0; tc < num_tc; tc++) {
		double tc_total = 0.0;
		printf(" TC %d %s\n", tc, rt_ctx[tc].test_name);
		for (cur = 0; cur < num_cores; cur++) {
			cur_stat = per_core_stat + tc * num_cores + cur;
			tc_total += cur_stat->stat_rt.total_time;
			group_exec_time += cur_stat->stat_rt.total_time;
			printf(" Per core %d execution = %.6f\n", cur, cur_stat->stat_rt.total_time);
		}
		printf(" TC %d %s Avg execution = %.6f sec \n", tc, rt_ctx[tc].test_name, tc_total / num_cores);
		printf("\n");
	}

	group_exec_time /= num_cores;

	printf("Summary: Avg execution = %.6f sec\n", group_exec_time);

	free(per_core_stat);

	/* Device Level Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_dev *sum_stat = (struct perf_stat_dev *)&_sum_stat;
		struct perf_stat_dev *cur_stat;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_dev));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, DEVICE_STAT);
		if (ret < 0)
			return;

		/* gather dev stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_dev *)&temp_stat;

			sum_stat->total_io_count += cur_stat->total_io_count;
			sum_stat->read_io_count += cur_stat->read_io_count;
			sum_stat->write_io_count += cur_stat->write_io_count;
		}

		printf(" Device Total I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->total_io_count * CLUSTER_SIZE / MB / group_exec_time);
		printf(" Device Read I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->read_io_count * CLUSTER_SIZE / MB / group_exec_time);
		printf(" Device Write I/O bandwidth = %.3f MB/s\n",
		       (double)sum_stat->write_io_count * CLUSTER_SIZE / MB / group_exec_time);

		printf(" Device Total I/O Amount = %.3f MB\n",
		       (double)sum_stat->total_io_count * CLUSTER_SIZE / MB);
		printf(" Device Read I/O Amount = %.3f MB\n", (double)sum_stat->read_io_count * CLUSTER_SIZE / MB);
		printf(" Device Write I/O Amount = %.3f MB\n",
		       (double)sum_stat->write_io_count * CLUSTER_SIZE / MB);
	}

	/* IPC Stat */
	if (nvfuse_process_model_is_dataplane()) {
		union perf_stat _sum_stat;
		struct perf_stat_ipc *sum_stat = (struct perf_stat_ipc *)&_sum_stat;
		struct perf_stat_ipc *cur_stat;
		s32 type;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_ipc));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, IPC_STAT);
		if (ret < 0)
			return;

		/* gather dev stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_ipc *)&temp_stat;
			for (type = APP_REGISTER_REQ; type < HEALTH_CHECK_CPL; type++) {
				sum_stat->total_tsc[type] += cur_stat->total_tsc[type];
				sum_stat->total_count[type] += cur_stat->total_count[type];
			}

			printf(" Core %d Container Alloc Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[CONTAINER_ALLOC_REQ] / cur_stat->total_count[CONTAINER_ALLOC_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d Container Free Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[CONTAINER_RELEASE_REQ] / cur_stat->total_count[CONTAINER_RELEASE_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d BUFFER Alloc Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[BUFFER_ALLOC_REQ] / cur_stat->total_count[BUFFER_ALLOC_REQ] /
			       spdk_get_ticks_hz() * 1000000);
			printf(" Core %d BUFFER Free Latency = %f us\n", i,
			       (double)cur_stat->total_tsc[BUFFER_FREE_REQ] / cur_stat->total_count[BUFFER_FREE_REQ] /
			       spdk_get_ticks_hz() * 1000000);
		}

		printf(" Avg Container Alloc Latency = %f us\n",
		       (double)sum_stat->total_tsc[CONTAINER_ALLOC_REQ] / sum_stat->total_count[CONTAINER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg Container Free Latency = %f us\n",
		       (double)sum_stat->total_tsc[CONTAINER_RELEASE_REQ] / sum_stat->total_count[CONTAINER_RELEASE_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg BUFFER Alloc Latency = %f us\n",
		       (double)sum_stat->total_tsc[BUFFER_ALLOC_REQ] / sum_stat->total_count[BUFFER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Avg BUFFER Free Latency = %f us\n",
		       (double)sum_stat->total_tsc[BUFFER_FREE_REQ] / sum_stat->total_count[BUFFER_FREE_REQ] /
		       spdk_get_ticks_hz() * 1000000);
	}

	printf("\n");
	/* Rusage Stat */
	{
		union perf_stat _sum_stat;
		struct perf_stat_rusage *sum_stat = (struct perf_stat_rusage *)&_sum_stat;
		struct perf_stat_rusage *cur_stat;

		memset(sum_stat, 0x00, sizeof(struct perf_stat_rusage));

		/* stat ring lookup */
		ret = perf_stat_ring_lookup(&stat_rx_ring, &stat_message_pool, RUSAGE_STAT);
		if (ret < 0)
			return;

		/* gather rusage stats */
		for (i = 0; i < num_cores; i++) {
			ret = nvfuse_stat_ring_get(stat_rx_ring, stat_message_pool, (union perf_stat *)&temp_stat);
			if (ret < 0)
				return;

			cur_stat = (struct perf_stat_rusage *)&temp_stat;

			//(group_exec_time / num_cores);
			sprintf(name, "core %d", i);
			print_rusage(&cur_stat->result, name, 1, group_exec_time);

			nvfuse_rusage_add(&cur_stat->result, &sum_stat->result);
			//printf(" tag = %x\n", cur_stat->tag);
		}

		sprintf(name, "Avg");
		print_rusage(&sum_stat->result, name, num_cores, group_exec_time);
	}
}

void regression_run(void *arg1, void *arg2)
{
	rt_main((void *)1);
	spdk_app_stop(0);
}

void reactor_run(void *arg)
{
	struct spdk_event *event;
	u32 i;

	/* Send events to start all I/O */
	SPDK_ENV_FOREACH_CORE(i) {
		printf(" allocate event on lcore = %d \n", i);
		if (i == 1) {
			event = spdk_event_allocate(i, regression_run,
						    NULL, NULL);
			spdk_event_call(event);
		}
	}
}

int main(int argc, char *argv[])
{
	int core_argc = 0;
	char *core_argv[128];
	int app_argc = 0;
	char *app_argv[128];
	char op;
	int ret = 0;
	int num_cores = 0;
//#define USE_LCORE_ID
#ifdef USE_LCORE_ID
	int lcore_id;
#endif

	/* distinguish cmd line into core args and app args */
	nvfuse_distinguish_core_and_app_options(argc, argv,
						&core_argc, core_argv,
						&app_argc, app_argv);

	ret = nvfuse_parse_args(core_argc, core_argv, g_params);
	if (ret < 0)
		return -1;

	ret = nvfuse_configure_spdk(g_ipc_ctx, g_params, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		return -1;

	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(app_argc, app_argv, "T:")) != -1) {
		switch (op) {
		case 'T':
			test_type = atoi(optarg);
			if (test_type < MAX_TEST || test_type > MILL_TEST) {
				fprintf(stderr, " Invalid test type = %d", test_type);
				goto INVALID_ARGS;
			}
			break;
		default:
			goto INVALID_ARGS;
		}
	}

	printf(" launch primary lcore = %d \n", rte_lcore_id());

#ifndef NVFUSE_USE_CEPH_SPDK
	spdk_app_start(&g_params->opts, reactor_run, NULL);
#else
	spdk_app_start(reactor_run, NULL);
#endif

	spdk_app_fini();

	num_cores++;

	print_stats(num_cores, NUM_ELEMENTS(rt_ctx));

	nvfuse_deinit_spdk(g_ipc_ctx);

	return ret;

INVALID_ARGS:
	;
	nvfuse_core_usage(argv[0]);
	rt_usage(argv[0]);
	nvfuse_core_usage_example(argv[0]);
	return -1;
}
