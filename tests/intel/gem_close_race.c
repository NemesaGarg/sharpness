/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/syscall.h>

#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_mman.h"
#include "igt.h"
#include "igt_aux.h"
#include "igt_device_scan.h"
/**
 * TEST: gem close race
 * Description: Test try to race gem_close against workload submission.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: buffer management
 * Feature: synchronization
 * Test category: GEM_Legacy
 *
 * SUBTEST: basic-process
 * Description: Basic workload submission.
 *
 * SUBTEST: basic-threads
 * Description: Share buffer handle across different drm fd's and trying to
 *		race gem_close against continuous workload with minimum timeout.
 *
 * SUBTEST: contexts
 * Description: Share buffer handle across different drm fd's and trying to
 *		race gem_close against continuous workload in other contexts.
 *
 * SUBTEST: gem-close-race
 * Description: Share buffer handle across different drm fd's and trying to
 *		race of gem_close against continuous workload.
 *
 * SUBTEST: multigpu-basic-process
 * Description: Basic workload submission on multi-GPU machine.
 * Sub-category: MultiGPU
 * Functionality: buffer management on MultiGPU
 * Feature: multigpu, synchronization
 *
 * SUBTEST: multigpu-basic-threads
 * Description: Run basic-threads race on multi-GPU machine.
 * Sub-category: MultiGPU
 * Functionality: buffer management on MultiGPU
 * Feature: multigpu, synchronization
 *
 * SUBTEST: process-exit
 * Description: Test try to race gem_close against submission of continuous workload.
 */

#define OBJECT_SIZE (256 * 1024)

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)

IGT_TEST_DESCRIPTION("Test try to race gem_close against workload submission.");

static uint32_t devid;
static bool has_64bit_relocations;
static bool has_softpin;
static uint64_t exec_addr;
static uint64_t data_addr;

static void selfcopy(int fd, uint32_t ctx, uint32_t handle, int loops)
{
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_exec_object2 gem_exec[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_create create;
	uint32_t buf[16], *b = buf;
	int err;

	memset(reloc, 0, sizeof(reloc));

	*b = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
	if (has_64bit_relocations)
		*b += 2;
	b++;
	*b++ = 0xcc << 16 | 1 << 25 | 1 << 24 | (4*1024);
	*b++ = 0;
	*b++ = 1 << 16 | 1024;

	reloc[0].offset = (b - buf) * sizeof(*b);
	reloc[0].target_handle = handle;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	reloc[0].presumed_offset = data_addr;
	*b++ = data_addr;
	if (has_64bit_relocations)
		*b++ = CANONICAL(data_addr) >> 32;

	*b++ = 512 << 16;
	*b++ = 4*1024;

	reloc[1].offset = (b - buf) * sizeof(*b);
	reloc[1].target_handle = handle;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = 0;
	reloc[1].presumed_offset = data_addr;
	*b++ = data_addr;
	if (has_64bit_relocations)
		*b++ = CANONICAL(data_addr) >> 32;

	*b++ = MI_BATCH_BUFFER_END;
	*b++ = 0;

	memset(gem_exec, 0, sizeof(gem_exec));
	gem_exec[0].handle = handle;

	memset(&create, 0, sizeof(create));
	create.handle = 0;
	create.size = 4096;
	drmIoctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create);
	gem_exec[1].handle = create.handle;
	gem_exec[1].offset = CANONICAL(exec_addr);
	gem_exec[0].offset = CANONICAL(data_addr);
	if (has_softpin) {
		gem_exec[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		gem_exec[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
				     EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	} else {
		gem_exec[1].relocation_count = 2;
		gem_exec[1].relocs_ptr = to_user_pointer(reloc);
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.flags |= I915_EXEC_NO_RELOC;
	execbuf.buffers_ptr = to_user_pointer(gem_exec);
	execbuf.buffer_count = 2;
	execbuf.batch_len = (b - buf) * sizeof(*b);
	if (HAS_BLT_RING(devid))
		execbuf.flags |= I915_EXEC_BLT;
	execbuf.rsvd1 = ctx;

	err = __gem_write(fd, create.handle, 0, buf, sizeof(buf));
	if (err == -EOPNOTSUPP) {
		void *ptr;

		ptr = __gem_mmap__device_coherent(fd, create.handle, 0, sizeof(buf), PROT_WRITE);
		if (!ptr) {
			err = errno;
		} else {
			memcpy(ptr, buf, sizeof(buf));
			gem_munmap(ptr, sizeof(buf));
		}
	}

	if (!err)
		while (loops-- && __gem_execbuf(fd, &execbuf) == 0)
			;

	drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &create.handle);
}

static uint32_t load(int fd)
{
	uint32_t handle;

	handle = gem_create(fd, OBJECT_SIZE);
	if (handle == 0)
		return 0;

	selfcopy(fd, 0, handle, 100);
	return handle;
}

static void process(int fd, int child)
{
	uint32_t handle;

	fd = drm_reopen_driver(fd);

	handle = load(fd);
	if ((child & 63) == 63)
		gem_read(fd, handle, 0, &handle, sizeof(handle));

	/* leave fd to be closed by process termination */
}

struct crashme {
	int fd;
} crashme;

static void crashme_now(int sig)
{
	close(crashme.fd);
}

#define usec(x) (1000*(x))
#define msec(x) usec(1000*(x))

static void thread(int fd, struct drm_gem_open name,
		   int timeout, unsigned int flags)
#define CONTEXTS 0x1
{
	struct sigevent sev;
	struct sigaction act;
	struct itimerspec its;
	uint32_t *history;
#define N_HISTORY (256)
	timer_t timer;

	history = malloc(sizeof(*history) * N_HISTORY);
	igt_assert(history);

	memset(&act, 0, sizeof(act));
	act.sa_handler = crashme_now;
	igt_assert(sigaction(SIGRTMIN, &act, NULL) == 0);

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_SIGNAL | SIGEV_THREAD_ID;
	sev.sigev_notify_thread_id = gettid();
	sev.sigev_signo = SIGRTMIN;
	igt_assert(timer_create(CLOCK_MONOTONIC, &sev, &timer) == 0);

	igt_until_timeout(timeout) {
		unsigned int n = 0;

		memset(history, 0, sizeof(*history) * N_HISTORY);

		crashme.fd = drm_reopen_driver(fd);

		memset(&its, 0, sizeof(its));
		its.it_value.tv_nsec = msec(1) + (rand() % msec(150));
		igt_assert(timer_settime(timer, 0, &its, NULL) == 0);

		do {
			uint32_t ctx = 0;

			if (drmIoctl(crashme.fd,
				     DRM_IOCTL_GEM_OPEN,
				     &name))
				break;

			if (flags & CONTEXTS)
				__gem_context_create(crashme.fd, &ctx);

			selfcopy(crashme.fd, ctx, name.handle, 1);

			ctx = history[n % N_HISTORY];
			if (ctx)
				drmIoctl(crashme.fd,
					 DRM_IOCTL_GEM_CLOSE,
					 &ctx);
			history[n % N_HISTORY] = name.handle;
			n++;
		} while (1);

		/* leave fd to be closed by process termination */
	}

	timer_delete(timer);
	free(history);
}

static void multigpu_threads(int timeout, unsigned int flags, int gpu_count)
{
	int size = sysconf(_SC_NPROCESSORS_ONLN);

	size /= gpu_count;
	if (size < 1)
		size = 1;

	igt_multi_fork(gpu, gpu_count) {
		struct drm_gem_open name;
		int fd = __drm_open_driver_another(gpu, DRIVER_INTEL);

		igt_assert(fd > 0);

		igt_fork(child, size)
			thread(fd, name, timeout, flags);

		igt_waitchildren();
		gem_quiescent_gpu(fd);
		drm_close_driver(fd);
	}

	igt_waitchildren();
}

static void threads(int timeout, unsigned int flags)
{
	struct drm_gem_open name;
	int fd;

	fd = drm_open_driver(DRIVER_INTEL);
	name.name = gem_flink(fd, gem_create(fd, OBJECT_SIZE));

	igt_fork(child, sysconf(_SC_NPROCESSORS_ONLN))
		thread(fd, name, timeout, flags);
	igt_waitchildren();

	gem_quiescent_gpu(fd);
	drm_close_driver(fd);
}

igt_main
{
	int gpu_count;

	igt_fixture {
		int fd;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		devid = intel_get_drm_devid(fd);
		has_64bit_relocations = intel_gen(devid) >= 8;
		has_softpin = !gem_has_relocations(fd);
		exec_addr = gem_detect_safe_start_offset(fd);
		data_addr = gem_detect_safe_alignment(fd);
		exec_addr = max_t(exec_addr, exec_addr, data_addr);
		data_addr += exec_addr;

		gpu_count = igt_device_filter_count();

		igt_fork_hang_detector(fd);
		drm_close_driver(fd);
	}

	igt_describe("Basic workload submission.");
	igt_subtest("basic-process") {
		int fd = drm_open_driver(DRIVER_INTEL);

		igt_fork(child, 1)
			process(fd, child);
		igt_waitchildren();

		gem_quiescent_gpu(fd);
		drm_close_driver(fd);
	}

	igt_describe("Basic workload submission on multi-GPU machine.");
	igt_subtest("multigpu-basic-process") {
		igt_require(gpu_count > 1);

		igt_multi_fork(child, gpu_count) {
			int fd = __drm_open_driver_another(child, DRIVER_INTEL);

			igt_assert(fd > 0);
			process(fd, child);
			gem_quiescent_gpu(fd);
			drm_close_driver(fd);
		}

		igt_waitchildren();
	}

	igt_describe("Share buffer handle across different drm fd's and trying to race "
		     " gem_close against continuous workload with minimum timeout.");
	igt_subtest("basic-threads")
		threads(1, 0);

	igt_describe("Run basic-threads race on multi-GPU machine.");
	igt_subtest("multigpu-basic-threads") {
		igt_require(gpu_count > 1);
		multigpu_threads(1, 0, gpu_count);
	}

	igt_describe("Test try to race gem_close against submission of continuous"
		     " workload.");
	igt_subtest("process-exit") {
		int fd = drm_open_driver(DRIVER_INTEL);

		igt_fork(child, 768)
			process(fd, child);
		igt_waitchildren();

		gem_quiescent_gpu(fd);
		drm_close_driver(fd);
	}

	igt_describe("Share buffer handle across different drm fd's and trying to race"
		     " gem_close against continuous workload in other contexts.");
	igt_subtest("contexts")
		threads(30, CONTEXTS);

	igt_describe("Share buffer handle across different drm fd's and trying to race of"
		     " gem_close against continuous workload.");
	igt_subtest("gem-close-race")
		threads(150, 0);

	igt_fixture
	    igt_stop_hang_detector();
}
