// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Jason Ekstrand <jason@jlekstrand.net>
 *    Maarten Lankhorst <maarten.lankhorst@linux.intel.com>
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pciaccess.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/sysmacros.h>
#else
#define major(__v__) (((__v__) >> 8) & 0xff)
#define minor(__v__) ((__v__) & 0xff)
#endif
#include <time.h>

#include "config.h"
#include "drmtest.h"
#include "igt_syncobj.h"
#include "ioctl_wrappers.h"
#include "xe_ioctl.h"
#include "xe_query.h"

uint32_t xe_cs_prefetch_size(int fd)
{
	return 2048;
}

uint32_t xe_vm_create(int fd, uint32_t flags, uint64_t ext)
{
	struct drm_xe_vm_create create = {
		.extensions = ext,
		.flags = flags,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &create), 0);

	return create.vm_id;
}

void xe_vm_unbind_all_async(int fd, uint32_t vm, uint32_t exec_queue,
			    uint32_t bo, struct drm_xe_sync *sync,
			    uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, bo, 0, 0, 0,
			    DRM_XE_VM_BIND_OP_UNMAP_ALL, DRM_XE_VM_BIND_FLAG_ASYNC,
			    sync, num_syncs, 0, 0);
}

void xe_vm_bind_array(int fd, uint32_t vm, uint32_t exec_queue,
		      struct drm_xe_vm_bind_op *bind_ops,
		      uint32_t num_bind, struct drm_xe_sync *sync,
		      uint32_t num_syncs)
{
	struct drm_xe_vm_bind bind = {
		.vm_id = vm,
		.num_binds = num_bind,
		.vector_of_binds = (uintptr_t)bind_ops,
		.num_syncs = num_syncs,
		.syncs = (uintptr_t)sync,
		.exec_queue_id = exec_queue,
	};

	igt_assert(num_bind > 1);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_VM_BIND, &bind), 0);
}

int  __xe_vm_bind(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
		  uint64_t offset, uint64_t addr, uint64_t size, uint32_t op,
		  uint32_t flags, struct drm_xe_sync *sync, uint32_t num_syncs,
		  uint32_t region, uint64_t ext)
{
	struct drm_xe_vm_bind bind = {
		.extensions = ext,
		.vm_id = vm,
		.num_binds = 1,
		.bind.obj = bo,
		.bind.obj_offset = offset,
		.bind.range = size,
		.bind.addr = addr,
		.bind.op = op,
		.bind.flags = flags,
		.bind.region = region,
		.num_syncs = num_syncs,
		.syncs = (uintptr_t)sync,
		.exec_queue_id = exec_queue,
	};

	if (igt_ioctl(fd, DRM_IOCTL_XE_VM_BIND, &bind))
		return -errno;

	return 0;
}

void  __xe_vm_bind_assert(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
			  uint64_t offset, uint64_t addr, uint64_t size,
			  uint32_t op, uint32_t flags, struct drm_xe_sync *sync,
			  uint32_t num_syncs, uint32_t region, uint64_t ext)
{
	igt_assert_eq(__xe_vm_bind(fd, vm, exec_queue, bo, offset, addr, size,
				   op, flags, sync, num_syncs, region, ext), 0);
}

void xe_vm_bind(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
		uint64_t addr, uint64_t size,
		struct drm_xe_sync *sync, uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, 0, bo, offset, addr, size,
			    DRM_XE_VM_BIND_OP_MAP, 0, sync, num_syncs, 0, 0);
}

void xe_vm_unbind(int fd, uint32_t vm, uint64_t offset,
		  uint64_t addr, uint64_t size,
		  struct drm_xe_sync *sync, uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, 0, 0, offset, addr, size,
			    DRM_XE_VM_BIND_OP_UNMAP, 0, sync, num_syncs, 0, 0);
}

void xe_vm_prefetch_async(int fd, uint32_t vm, uint32_t exec_queue, uint64_t offset,
			  uint64_t addr, uint64_t size,
			  struct drm_xe_sync *sync, uint32_t num_syncs,
			  uint32_t region)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, 0, offset, addr, size,
			    DRM_XE_VM_BIND_OP_PREFETCH, DRM_XE_VM_BIND_FLAG_ASYNC,
			    sync, num_syncs, region, 0);
}

void xe_vm_bind_async(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
		      uint64_t offset, uint64_t addr, uint64_t size,
		      struct drm_xe_sync *sync, uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, bo, offset, addr, size,
			    DRM_XE_VM_BIND_OP_MAP, DRM_XE_VM_BIND_FLAG_ASYNC, sync,
			    num_syncs, 0, 0);
}

void xe_vm_bind_async_flags(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
			    uint64_t offset, uint64_t addr, uint64_t size,
			    struct drm_xe_sync *sync, uint32_t num_syncs,
			    uint32_t flags)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, bo, offset, addr, size,
			    DRM_XE_VM_BIND_OP_MAP, DRM_XE_VM_BIND_FLAG_ASYNC | flags,
			    sync, num_syncs, 0, 0);
}

void xe_vm_bind_userptr_async(int fd, uint32_t vm, uint32_t exec_queue,
			      uint64_t userptr, uint64_t addr, uint64_t size,
			      struct drm_xe_sync *sync, uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, 0, userptr, addr, size,
			    DRM_XE_VM_BIND_OP_MAP_USERPTR, DRM_XE_VM_BIND_FLAG_ASYNC,
			    sync, num_syncs, 0, 0);
}

void xe_vm_bind_userptr_async_flags(int fd, uint32_t vm, uint32_t exec_queue,
				    uint64_t userptr, uint64_t addr,
				    uint64_t size, struct drm_xe_sync *sync,
				    uint32_t num_syncs, uint32_t flags)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, 0, userptr, addr, size,
			    DRM_XE_VM_BIND_OP_MAP_USERPTR, DRM_XE_VM_BIND_FLAG_ASYNC |
			    flags, sync, num_syncs, 0, 0);
}

void xe_vm_unbind_async(int fd, uint32_t vm, uint32_t exec_queue,
			uint64_t offset, uint64_t addr, uint64_t size,
			struct drm_xe_sync *sync, uint32_t num_syncs)
{
	__xe_vm_bind_assert(fd, vm, exec_queue, 0, offset, addr, size,
			    DRM_XE_VM_BIND_OP_UNMAP, DRM_XE_VM_BIND_FLAG_ASYNC, sync,
			    num_syncs, 0, 0);
}

static void __xe_vm_bind_sync(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
			      uint64_t addr, uint64_t size, uint32_t op)
{
	__xe_vm_bind_assert(fd, vm, 0, bo, offset, addr, size, op, 0, NULL, 0,
			    0, 0);
}

void xe_vm_bind_sync(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
		     uint64_t addr, uint64_t size)
{
	__xe_vm_bind_sync(fd, vm, bo, offset, addr, size, DRM_XE_VM_BIND_OP_MAP);
}

void xe_vm_unbind_sync(int fd, uint32_t vm, uint64_t offset,
		       uint64_t addr, uint64_t size)
{
	__xe_vm_bind_sync(fd, vm, 0, offset, addr, size, DRM_XE_VM_BIND_OP_UNMAP);
}

void xe_vm_destroy(int fd, uint32_t vm)
{
	struct drm_xe_vm_destroy destroy = {
		.vm_id = vm,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_VM_DESTROY, &destroy), 0);
}

uint32_t __xe_bo_create_flags(int fd, uint32_t vm, uint64_t size, uint32_t flags,
			      uint32_t *handle)
{
	struct drm_xe_gem_create create = {
		.vm_id = vm,
		.size = size,
		.flags = flags,
	};
	int err;

	err = igt_ioctl(fd, DRM_IOCTL_XE_GEM_CREATE, &create);
	if (err)
		return err;

	*handle = create.handle;
	return 0;
}

uint32_t xe_bo_create_flags(int fd, uint32_t vm, uint64_t size, uint32_t flags)
{
	uint32_t handle;

	igt_assert_eq(__xe_bo_create_flags(fd, vm, size, flags, &handle), 0);

	return handle;
}

uint32_t xe_bo_create(int fd, int gt, uint32_t vm, uint64_t size)
{
	struct drm_xe_gem_create create = {
		.vm_id = vm,
		.size = size,
		.flags = vram_if_possible(fd, gt),
	};
	int err;

	err = igt_ioctl(fd, DRM_IOCTL_XE_GEM_CREATE, &create);
	igt_assert_eq(err, 0);

	return create.handle;
}

uint32_t xe_bind_exec_queue_create(int fd, uint32_t vm, uint64_t ext, bool async)
{
	struct drm_xe_engine_class_instance instance = {
		.engine_class = async ? DRM_XE_ENGINE_CLASS_VM_BIND_ASYNC :
			DRM_XE_ENGINE_CLASS_VM_BIND_SYNC,
	};
	struct drm_xe_exec_queue_create create = {
		.extensions = ext,
		.vm_id = vm,
		.width = 1,
		.num_placements = 1,
		.instances = to_user_pointer(&instance),
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &create), 0);

	return create.exec_queue_id;
}

uint32_t xe_exec_queue_create(int fd, uint32_t vm,
			  struct drm_xe_engine_class_instance *instance,
			  uint64_t ext)
{
	struct drm_xe_exec_queue_create create = {
		.extensions = ext,
		.vm_id = vm,
		.width = 1,
		.num_placements = 1,
		.instances = to_user_pointer(instance),
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &create), 0);

	return create.exec_queue_id;
}

uint32_t xe_exec_queue_create_class(int fd, uint32_t vm, uint16_t class)
{
	struct drm_xe_engine_class_instance instance = {
		.engine_class = class,
		.engine_instance = 0,
		.gt_id = 0,
	};
	struct drm_xe_exec_queue_create create = {
		.vm_id = vm,
		.width = 1,
		.num_placements = 1,
		.instances = to_user_pointer(&instance),
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &create), 0);

	return create.exec_queue_id;
}

void xe_exec_queue_destroy(int fd, uint32_t exec_queue)
{
	struct drm_xe_exec_queue_destroy destroy = {
		.exec_queue_id = exec_queue,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &destroy), 0);
}

uint64_t xe_bo_mmap_offset(int fd, uint32_t bo)
{
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = bo,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo), 0);

	return mmo.offset;
}

static void *__xe_bo_map(int fd, uint16_t bo, size_t size, int prot)
{
	uint64_t mmo;
	void *map;

	mmo = xe_bo_mmap_offset(fd, bo);
	map = mmap(NULL, size, prot, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	return map;
}

void *xe_bo_map(int fd, uint32_t bo, size_t size)
{
	return __xe_bo_map(fd, bo, size, PROT_WRITE);
}

void *xe_bo_mmap_ext(int fd, uint32_t bo, size_t size, int prot)
{
	return __xe_bo_map(fd, bo, size, prot);
}

int __xe_exec(int fd, struct drm_xe_exec *exec)
{
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_XE_EXEC, exec)) {
		err = -errno;
		igt_assume(err != 0);
	}
	errno = 0;
	return err;
}

void xe_exec(int fd, struct drm_xe_exec *exec)
{
	igt_assert_eq(__xe_exec(fd, exec), 0);
}

void xe_exec_sync(int fd, uint32_t exec_queue, uint64_t addr,
		  struct drm_xe_sync *sync, uint32_t num_syncs)
{
	struct drm_xe_exec exec = {
		.exec_queue_id = exec_queue,
		.syncs = (uintptr_t)sync,
		.num_syncs = num_syncs,
		.address = addr,
		.num_batch_buffer = 1,
	};

	igt_assert_eq(__xe_exec(fd, &exec), 0);
}

void xe_exec_wait(int fd, uint32_t exec_queue, uint64_t addr)
{
	struct drm_xe_sync sync = {
		.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL,
		.handle = syncobj_create(fd, 0),
	};

	xe_exec_sync(fd, exec_queue, addr, &sync, 1);

	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync.handle);
}

int64_t xe_wait_ufence(int fd, uint64_t *addr, uint64_t value,
		       struct drm_xe_engine_class_instance *eci,
		       int64_t timeout)
{
	struct drm_xe_wait_user_fence wait = {
		.addr = to_user_pointer(addr),
		.op = DRM_XE_UFENCE_WAIT_EQ,
		.flags = !eci ? DRM_XE_UFENCE_WAIT_SOFT_OP : 0,
		.value = value,
		.mask = DRM_XE_UFENCE_WAIT_U64,
		.timeout = timeout,
		.num_engines = eci ? 1 :0,
		.instances = eci ? to_user_pointer(eci) : 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE, &wait), 0);

	return wait.timeout;
}

/**
 * xe_wait_ufence_abstime:
 * @fd: xe device fd
 * @addr: address of value to compare
 * @value: expected value (equal) in @address
 * @eci: engine class instance
 * @timeout: absolute time when wait expire
 *
 * Function compares @value with memory pointed by @addr until they are equal.
 *
 * Returns elapsed time in nanoseconds if user fence was signalled.
 */
int64_t xe_wait_ufence_abstime(int fd, uint64_t *addr, uint64_t value,
			       struct drm_xe_engine_class_instance *eci,
			       int64_t timeout)
{
	struct drm_xe_wait_user_fence wait = {
		.addr = to_user_pointer(addr),
		.op = DRM_XE_UFENCE_WAIT_EQ,
		.flags = !eci ? DRM_XE_UFENCE_WAIT_SOFT_OP | DRM_XE_UFENCE_WAIT_ABSTIME : 0,
		.value = value,
		.mask = DRM_XE_UFENCE_WAIT_U64,
		.timeout = timeout,
		.num_engines = eci ? 1 : 0,
		.instances = eci ? to_user_pointer(eci) : 0,
	};
	struct timespec ts;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE, &wait), 0);
	igt_assert_eq(clock_gettime(CLOCK_MONOTONIC, &ts), 0);

	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

void xe_force_gt_reset(int fd, int gt)
{
	char reset_string[128];
	struct stat st;

	igt_assert_eq(fstat(fd, &st), 0);

	snprintf(reset_string, sizeof(reset_string),
		 "cat /sys/kernel/debug/dri/%d/gt%d/force_reset",
		 minor(st.st_rdev), gt);
	system(reset_string);
}
