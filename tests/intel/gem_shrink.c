/*
 * Copyright © 2016 Intel Corporation
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
 */

/** @file gem_shrink.c
 *
 * Exercise the shrinker by overallocating GEM objects
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_gt.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"
/**
 * TEST: gem shrink
 * Feature: mapping
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: buffer management
 * Test category: GEM_Legacy
 *
 * SUBTEST: execbuf1
 *
 * SUBTEST: execbuf1-oom
 *
 * SUBTEST: execbuf1-sanitycheck
 *
 * SUBTEST: execbuf1-userptr
 *
 * SUBTEST: execbuf1-userptr-dirty
 *
 * SUBTEST: execbufN
 *
 * SUBTEST: execbufN-oom
 *
 * SUBTEST: execbufN-sanitycheck
 *
 * SUBTEST: execbufN-userptr
 *
 * SUBTEST: execbufN-userptr-dirty
 *
 * SUBTEST: execbufX
 *
 * SUBTEST: execbufX-oom
 *
 * SUBTEST: execbufX-sanitycheck
 *
 * SUBTEST: execbufX-userptr
 *
 * SUBTEST: execbufX-userptr-dirty
 *
 * SUBTEST: get-pages
 *
 * SUBTEST: get-pages-dirty
 *
 * SUBTEST: get-pages-dirty-oom
 *
 * SUBTEST: get-pages-dirty-sanitycheck
 *
 * SUBTEST: get-pages-dirty-userptr
 *
 * SUBTEST: get-pages-dirty-userptr-dirty
 *
 * SUBTEST: get-pages-oom
 *
 * SUBTEST: get-pages-sanitycheck
 *
 * SUBTEST: get-pages-userptr
 *
 * SUBTEST: get-pages-userptr-dirty
 *
 * SUBTEST: hang
 *
 * SUBTEST: hang-oom
 *
 * SUBTEST: hang-sanitycheck
 *
 * SUBTEST: hang-userptr
 *
 * SUBTEST: hang-userptr-dirty
 *
 * SUBTEST: mmap-cpu
 *
 * SUBTEST: mmap-cpu-oom
 *
 * SUBTEST: mmap-cpu-sanitycheck
 *
 * SUBTEST: mmap-cpu-userptr
 *
 * SUBTEST: mmap-cpu-userptr-dirty
 *
 * SUBTEST: mmap-gtt
 *
 * SUBTEST: mmap-gtt-oom
 *
 * SUBTEST: mmap-gtt-sanitycheck
 *
 * SUBTEST: mmap-gtt-userptr
 *
 * SUBTEST: mmap-gtt-userptr-dirty
 *
 * SUBTEST: pread
 *
 * SUBTEST: pread-oom
 *
 * SUBTEST: pread-sanitycheck
 *
 * SUBTEST: pread-userptr
 *
 * SUBTEST: pread-userptr-dirty
 *
 * SUBTEST: pwrite
 *
 * SUBTEST: pwrite-oom
 *
 * SUBTEST: pwrite-sanitycheck
 *
 * SUBTEST: pwrite-userptr
 *
 * SUBTEST: pwrite-userptr-dirty
 *
 * SUBTEST: reclaim
 */

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

static void get_pages(int fd, uint64_t alloc)
{
	uint32_t handle = gem_create(fd, alloc);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, 0);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void get_pages_dirty(int fd, uint64_t alloc)
{
	uint32_t handle = gem_create(fd, alloc);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void pwrite_(int fd, uint64_t alloc)
{
	uint32_t tmp;
	uint32_t handle = gem_create(fd, alloc);
	for (int page = 0; page < alloc>>12; page++)
		gem_write(fd, handle, ((page << 12) + page % 4095) & ~3, &tmp, 4);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void pread_(int fd, uint64_t alloc)
{
	uint32_t tmp;
	uint32_t handle = gem_create(fd, alloc);
	for (int page = 0; page < alloc>>12; page++)
		gem_read(fd, handle, ((page << 12) + page % 4095) & ~3, &tmp, 4);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void mmap_gtt(int fd, uint64_t alloc)
{
	uint32_t handle = gem_create(fd, alloc);
	uint32_t *ptr = gem_mmap__gtt(fd, handle, alloc, PROT_WRITE);
	for (int page = 0; page < alloc>>12; page++)
		ptr[page<<10] = 0;
	munmap(ptr, alloc);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void mmap_cpu(int fd, uint64_t alloc)
{
	uint32_t handle = gem_create(fd, alloc);
	uint32_t *ptr = gem_mmap__cpu(fd, handle, 0, alloc, PROT_WRITE);
	for (int page = 0; page < alloc>>12; page++)
		ptr[page<<10] = 0;
	munmap(ptr, alloc);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static void execbuf1(int fd, uint64_t alloc)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;

	memset(&obj, 0, sizeof(obj));
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;

	obj.handle = gem_create(fd, alloc);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(fd, &execbuf);
	gem_madvise(fd, obj.handle, I915_MADV_DONTNEED);
}

/* Since we want to trigger oom (SIGKILL), we don't want small allocations
 * to fail and generate a false error (SIGSEGV)! So we redirect allocations
 * though GEM objects, which should be much more likely to trigger oom. There
 * are still small allocations within the kernel, so still a small chance of
 * ENOMEM instead of a full oom.
 */
static void *__gem_calloc(int fd, size_t count, size_t size, uint64_t *out_size)
{
	uint32_t handle;
	uint64_t total;
	void *ptr;

	total = count * size;
	total = (total + 4095) & -4096;

	handle = gem_create(fd, total);
	ptr = gem_mmap__cpu(fd, handle, 0, total, PROT_WRITE);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	gem_close(fd, handle);

	*out_size = total;
	return ptr;
}

static void execbufN(int fd, uint64_t alloc)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	int count = alloc >> 20;
	uint64_t obj_size;

	obj = __gem_calloc(fd, alloc + 1, sizeof(*obj), &obj_size);
	memset(&execbuf, 0, sizeof(execbuf));

	obj[count].handle = gem_create(fd, 4096);
	gem_write(fd, obj[count].handle, 0, &bbe, sizeof(bbe));

	for (int i = 1; i <= count; i++) {
		int j = count - i;

		obj[j].handle = gem_create(fd, 1 << 20);
		execbuf.buffers_ptr = to_user_pointer(&obj[j]);
		execbuf.buffer_count = i + 1;
		gem_execbuf(fd, &execbuf);
	}

	for (int i = 0; i <= count; i++)
		gem_madvise(fd, obj[i].handle, I915_MADV_DONTNEED);
	munmap(obj, obj_size);
}

static void execbufX(int fd, uint64_t alloc)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct intel_engine_data engines;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	const intel_ctx_t *ctx;
	int count = alloc >> 20;
	uint64_t obj_size;

	obj = __gem_calloc(fd, alloc + 1, sizeof(*obj), &obj_size);
	memset(&execbuf, 0, sizeof(execbuf));

	obj[count].handle = gem_create(fd, 4096);
	gem_write(fd, obj[count].handle, 0, &bbe, sizeof(bbe));

	ctx = intel_ctx_create_all_physical(fd);
	engines = intel_engine_list_for_ctx_cfg(fd, &ctx->cfg);

	for (int i = 1; i <= count; i++) {
		int j = count - i;

		obj[j+1].flags = 0;

		obj[j].handle = gem_create(fd, 1 << 20);
		obj[j].flags = EXEC_OBJECT_WRITE;

		execbuf.buffers_ptr = to_user_pointer(&obj[j]);
		execbuf.buffer_count = i + 1;
		execbuf.flags = engines.engines[j % engines.nengines].flags;
		execbuf.rsvd1 = ctx->id;
		gem_execbuf(fd, &execbuf);
	}

	for (int i = 0; i <= count; i++)
		gem_madvise(fd, obj[i].handle, I915_MADV_DONTNEED);
	munmap(obj, obj_size);

	intel_ctx_destroy(fd, ctx);
}

static void hang(int fd, uint64_t alloc)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	int count = alloc >> 20;
	uint64_t obj_size;

	obj = __gem_calloc(fd, alloc + 1, sizeof(*obj), &obj_size);
	memset(&execbuf, 0, sizeof(execbuf));

	obj[count].handle = gem_create(fd, 4096);
	gem_write(fd, obj[count].handle, 0, &bbe, sizeof(bbe));

	for (int i = 1; i <= count; i++) {
		int j = count - i;

		obj[j].handle = gem_create(fd, 1 << 20);
		execbuf.buffers_ptr = to_user_pointer(&obj[j]);
		execbuf.buffer_count = i + 1;
		gem_execbuf(fd, &execbuf);
	}

	gem_close(fd, igt_hang_ring(fd, 0).spin->handle);
	for (int i = 0; i <= count; i++)
		gem_madvise(fd, obj[i].handle, I915_MADV_DONTNEED);
	munmap(obj, obj_size);
}

static void userptr(int fd, uint64_t alloc, unsigned int flags)
#define UDIRTY (1 << 0)
{
	struct drm_i915_gem_userptr userptr;
	void *ptr;

	igt_assert((alloc & 4095) == 0);

	ptr = mmap(NULL, alloc,
		   PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
		   -1, 0);
	igt_assert(ptr != (void *)-1);

	memset(&userptr, 0, sizeof(userptr));
	userptr.user_size = alloc;
	userptr.user_ptr = to_user_pointer(ptr);
	do_ioctl(fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr);

	if (flags & UDIRTY)
		gem_set_domain(fd, userptr.handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	else
		gem_set_domain(fd, userptr.handle, I915_GEM_DOMAIN_GTT, 0);

	madvise(ptr, alloc, MADV_FREE);
}

static bool has_userptr(void)
{
	struct drm_i915_gem_userptr userptr;
	int fd = drm_open_driver(DRIVER_INTEL);
	int err;

	memset(&userptr, 0, sizeof(userptr));
	userptr.user_size = 8192;
	userptr.user_ptr = -4096;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr))
		err = errno;

	drm_close_driver(fd);

	return err == EFAULT;
}

static void leak(int fd, uint64_t alloc)
{
	char *ptr;

	ptr = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE | MAP_POPULATE,
		   -1, 0);
	if (ptr == MAP_FAILED)
		return;

	while (alloc) {
		alloc -= 4096;
		ptr[alloc] = 0;
	}
}

#define SOLO 1
#define USERPTR 2
#define USERPTR_DIRTY 4
#define OOM 8

static void run_test(int nchildren, uint64_t alloc,
		     void (*func)(int, uint64_t), unsigned flags)
{
	const int timeout = flags & SOLO ? 1 : 20;

	/* Each pass consumes alloc bytes and doesn't drop
	 * its reference to object (i.e. calls
	 * gem_madvise(DONTNEED) instead of gem_close()).
	 * After nchildren passes we expect each process
	 * to have enough objects to consume all of memory
	 * if left unchecked.
	 */

	if (flags & SOLO)
		nchildren = 1;

	/* Background load */
	if (flags & OOM) {
		igt_fork(child, nchildren) {
			igt_until_timeout(timeout) {
				int fd = drm_open_driver(DRIVER_INTEL);
				for (int pass = 0; pass < nchildren; pass++)
					leak(fd, alloc);
				drm_close_driver(fd);
			}
		}
	}

	if (flags & USERPTR) {
		igt_require(has_userptr());
		igt_fork(child, (nchildren + 1)/2) {
			igt_until_timeout(timeout) {
				int fd = drm_open_driver(DRIVER_INTEL);
				for (int pass = 0; pass < nchildren; pass++)
					userptr(fd, alloc, 0);
				drm_close_driver(fd);
			}
		}
		nchildren = (nchildren + 1)/2;
	}

	if (flags & USERPTR_DIRTY) {
		igt_require(has_userptr());
		igt_fork(child, (nchildren + 1)/2) {
			igt_until_timeout(timeout) {
				int fd = drm_open_driver(DRIVER_INTEL);
				for (int pass = 0; pass < nchildren; pass++)
					userptr(fd, alloc, UDIRTY);
				drm_close_driver(fd);
			}
		}
		nchildren = (nchildren + 1)/2;
	}

	/* Exercise major ioctls */
	igt_fork(child, nchildren) {
		igt_until_timeout(timeout) {
			int fd = drm_open_driver(DRIVER_INTEL);
			for (int pass = 0; pass < nchildren; pass++)
				func(fd, alloc);
			drm_close_driver(fd);
		}
	}
	igt_waitchildren();
}

static void reclaim(unsigned engine, int timeout)
{
	const uint64_t timeout_100ms = 100000000LL;
	int fd = drm_open_driver(DRIVER_INTEL);
	int debugfs = igt_debugfs_dir(fd);
	igt_spin_t *spin;
	volatile uint32_t *shared;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);

	shared = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	igt_fork(child, sysconf(_SC_NPROCESSORS_ONLN)) {
		do {
			igt_sysfs_printf(debugfs, "i915_drop_caches",
					"%d", DROP_BOUND | DROP_UNBOUND);
		} while (!*shared);
	}

	spin = igt_spin_new(fd, .ahnd = ahnd, .engine = engine);
	igt_until_timeout(timeout) {
		igt_spin_t *next = __igt_spin_new(fd, .ahnd = ahnd, .engine = engine);

		igt_spin_set_timeout(spin, timeout_100ms);
		gem_sync(fd, spin->handle);

		igt_spin_free(fd, spin);
		spin = next;
	}
	igt_spin_free(fd, spin);
	put_ahnd(ahnd);

	*shared = 1;
	igt_waitchildren();

	munmap((void *)shared, 4096);
	close(debugfs);
	drm_close_driver(fd);
}

igt_main
{
	const struct test {
		const char *name;
		void (*func)(int, uint64_t);
	} tests[] = {
		{ "get-pages", get_pages },
		{ "get-pages-dirty", get_pages_dirty },
		{ "pwrite", pwrite_ },
		{ "pread", pread_ },
		{ "mmap-gtt", mmap_gtt },
		{ "mmap-cpu", mmap_cpu },
		{ "execbuf1", execbuf1 },
		{ "execbufN", execbufN },
		{ "execbufX", execbufX },
		{ "hang", hang },
		{ NULL },
	};
	const struct mode {
		const char *suffix;
		unsigned flags;
	} modes[] = {
		{ "-sanitycheck", SOLO },
		{ "", 0 },
		{ "-userptr", USERPTR },
		{ "-userptr-dirty", USERPTR | USERPTR_DIRTY },
		{ "-oom", USERPTR | OOM },
		{ NULL },
	};
	uint64_t alloc_size = 0;
	int num_processes = 0;

	igt_fixture {
		const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
		uint64_t mem_size = igt_get_total_ram_mb();
		int fd;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		/*
		 * Spawn enough processes to use all memory, but each only
		 * uses a fraction of the available per-cpu memory.
		 * Individually the processes would be ok, but en masse
		 * we expect the shrinker to start purging objects,
		 * and possibly fail.
		 */
		alloc_size = (mem_size + ncpus - 1) / ncpus / 8;
		num_processes = ncpus + (mem_size / alloc_size);

		igt_info("Using %d processes and %'"PRIu64"MiB per process\n",
			 num_processes, alloc_size);

		alloc_size <<= 20;
		drm_close_driver(fd);
	}

	igt_subtest("reclaim")
		reclaim(I915_EXEC_DEFAULT, 2);

	for(const struct test *t = tests; t->name; t++) {
		for(const struct mode *m = modes; m->suffix; m++) {
			igt_subtest_f("%s%s", t->name, m->suffix) {
				igt_require_memory(num_processes, alloc_size,
						   CHECK_SWAP | CHECK_RAM);
				run_test(num_processes, alloc_size,
					 t->func, m->flags);
			}
		}
	}
}
