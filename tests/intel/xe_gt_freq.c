// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022,2023 Intel Corporation
 */

/**
 * TEST: Test Xe GT frequency request functionality
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: frequency request
 * Test category: functionality test
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "igt_sysfs.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

#include <string.h>
#include <sys/time.h>

#define MAX_N_EXEC_QUEUES 16

/*
 * Too many intermediate components and steps before freq is adjusted
 * Specially if workload is under execution, so let's wait 100 ms.
 */
#define ACT_FREQ_LATENCY_US 100000

static int set_freq(int fd, int gt_id, const char *freq_name, uint32_t freq)
{
	int ret = -EAGAIN;
	char freq_attr[22];
	int gt_fd;

	snprintf(freq_attr, sizeof(freq_attr), "freq0/%s_freq", freq_name);
	gt_fd = xe_sysfs_gt_open(fd, gt_id);
	igt_assert(gt_fd >= 0);

	while (ret == -EAGAIN)
		ret = igt_sysfs_printf(gt_fd, freq_attr, "%u", freq);

	close(gt_fd);
	return ret;
}

static uint32_t get_freq(int fd, int gt_id, const char *freq_name)
{
	uint32_t freq;
	int err = -EAGAIN;
	char freq_attr[22];
	int gt_fd;

	snprintf(freq_attr, sizeof(freq_attr), "freq0/%s_freq", freq_name);
	gt_fd = xe_sysfs_gt_open(fd, gt_id);
	igt_assert(gt_fd >= 0);

	while (err == -EAGAIN)
		err = igt_sysfs_scanf(gt_fd, freq_attr, "%u", &freq);

	igt_debug("gt%d: %s freq %u\n", gt_id, freq_name, freq);

	close(gt_fd);
	return freq;
}

static uint32_t get_throttle(int fd, int gt_id, const char *throttle_file)
{
	uint32_t val;
	char throttle_attr[40];
	int gt_fd;

	snprintf(throttle_attr, sizeof(throttle_attr),
		 "freq0/throttle/%s", throttle_file);
	gt_fd = xe_sysfs_gt_open(fd, gt_id);
	igt_assert(gt_fd >= 0);

	igt_sysfs_scanf(gt_fd, throttle_attr, "%u", &val);

	igt_debug("gt%d/freq0/throttle/%s: %u\n", gt_id, throttle_file, val);

	close(gt_fd);
	return val;
}

/**
 * SUBTEST: throttle_basic_api
 * Description: Test basic throttle API
 */

static void test_throttle_basic_api(int fd, int gt_id)
{
	uint32_t status, reasons;

	status = get_throttle(fd, gt_id, "status");
	reasons = get_throttle(fd, gt_id, "reason_pl1");
	reasons |= get_throttle(fd, gt_id, "reason_pl2");
	reasons |= get_throttle(fd, gt_id, "reason_pl4");
	reasons |= get_throttle(fd, gt_id, "reason_prochot");
	reasons |= get_throttle(fd, gt_id, "reason_ratl");
	reasons |= get_throttle(fd, gt_id, "reason_thermal");
	reasons |= get_throttle(fd, gt_id, "reason_vr_tdc");
	reasons |= get_throttle(fd, gt_id, "reason_vr_thermalert");

	if (status)
		igt_assert(reasons);
	else
		igt_assert(!reasons);
}

/**
 * SUBTEST: freq_basic_api
 * Description: Test basic get and set frequency API
 */

static void test_freq_basic_api(int fd, int gt_id)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");
	uint32_t rpe = get_freq(fd, gt_id, "rpe");
	uint32_t rp0 = get_freq(fd, gt_id, "rp0");

	/*
	 * Negative bound tests
	 * RPn is the floor
	 * RP0 is the ceiling
	 */
	igt_assert(set_freq(fd, gt_id, "min", rpn - 1) < 0);
	igt_assert(set_freq(fd, gt_id, "min", rp0 + 1) < 0);
	igt_assert(set_freq(fd, gt_id, "max", rpn - 1) < 0);
	igt_assert(set_freq(fd, gt_id, "max", rp0 + 1) < 0);

	/* Assert min requests are respected from rp0 to rpn */
	igt_assert(set_freq(fd, gt_id, "min", rp0) > 0);
	igt_assert(get_freq(fd, gt_id, "min") == rp0);
	igt_assert(set_freq(fd, gt_id, "min", rpe) > 0);
	igt_assert(get_freq(fd, gt_id, "min") == rpe);
	igt_assert(set_freq(fd, gt_id, "min", rpn) > 0);
	igt_assert(get_freq(fd, gt_id, "min") == rpn);

	/* Assert max requests are respected from rpn to rp0 */
	igt_assert(set_freq(fd, gt_id, "max", rpn) > 0);
	igt_assert(get_freq(fd, gt_id, "max") == rpn);
	igt_assert(set_freq(fd, gt_id, "max", rpe) > 0);
	igt_assert(get_freq(fd, gt_id, "max") == rpe);
	igt_assert(set_freq(fd, gt_id, "max", rp0) > 0);
	igt_assert(get_freq(fd, gt_id, "max") == rp0);
}

/**
 * SUBTEST: freq_fixed_idle
 * Description: Test fixed frequency request with exec_queue in idle state
 *
 * SUBTEST: freq_fixed_exec
 * Description: Test fixed frequency request when spinner is run
 */

static void test_freq_fixed(int fd, int gt_id, bool gt_idle)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");
	uint32_t rpe = get_freq(fd, gt_id, "rpe");
	uint32_t rp0 = get_freq(fd, gt_id, "rp0");

	igt_debug("Starting testing fixed request\n");

	/*
	 * For Fixed freq we need to set both min and max to the desired value
	 * Then we check if hardware is actually operating at the desired freq
	 * And let's do this for all the 3 known Render Performance (RP) values.
	 */
	igt_assert(set_freq(fd, gt_id, "min", rpn) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rpn) > 0);
	usleep(ACT_FREQ_LATENCY_US);
	igt_assert(get_freq(fd, gt_id, "cur") == rpn);

	if (gt_idle) {
		/* Wait for GT to go in C6 as previous get_freq wakes up GT*/
		igt_assert_f(igt_wait(xe_is_gt_in_c6(fd, gt_id), 1000, 10),
			     "GT %d should be in C6\n", gt_id);
		igt_assert(get_freq(fd, gt_id, "act") == 0);
	} else {
		igt_assert(get_freq(fd, gt_id, "act") == rpn);
	}

	/* Refresh value of rpe, pcode could have adjusted it */
	rpe = get_freq(fd, gt_id, "rpe");

	igt_assert(set_freq(fd, gt_id, "min", rpe) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rpe) > 0);
	usleep(ACT_FREQ_LATENCY_US);
	igt_assert(get_freq(fd, gt_id, "cur") == rpe);

	if (gt_idle) {
		igt_assert_f(igt_wait(xe_is_gt_in_c6(fd, gt_id), 1000, 10),
			     "GT %d should be in C6\n", gt_id);
		igt_assert(get_freq(fd, gt_id, "act") == 0);
	} else {
		igt_assert(get_freq(fd, gt_id, "act") == rpe);
	}

	igt_assert(set_freq(fd, gt_id, "min", rp0) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rp0) > 0);
	usleep(ACT_FREQ_LATENCY_US);
	/*
	 * It is unlikely that PCODE will *always* respect any request above RPe
	 * So for this level let's only check if GuC PC is doing its job
	 * and respecting our request, by propagating it to the hardware.
	 */
	igt_assert(get_freq(fd, gt_id, "cur") == rp0);

	if (gt_idle) {
		igt_assert_f(igt_wait(xe_is_gt_in_c6(fd, gt_id), 1000, 10),
			     "GT %d should be in C6\n", gt_id);
		igt_assert(get_freq(fd, gt_id, "act") == 0);
	}

	igt_debug("Finished testing fixed request\n");
}

/**
 * SUBTEST: freq_range_idle
 * Description: Test range frequency request with exec_queue in idle state
 *
 * SUBTEST: freq_range_exec
 * Description: Test range frequency request when spinner is run
 */
static void test_freq_range(int fd, int gt_id, bool gt_idle)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");
	uint32_t rpe = get_freq(fd, gt_id, "rpe");
	uint32_t cur, act;

	igt_debug("Starting testing range request\n");

	igt_assert(set_freq(fd, gt_id, "min", rpn) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rpe) > 0);
	usleep(ACT_FREQ_LATENCY_US);
	cur = get_freq(fd, gt_id, "cur");
	igt_assert(rpn <= cur && cur <= rpe);

	if (gt_idle) {
		igt_assert_f(igt_wait(xe_is_gt_in_c6(fd, gt_id), 1000, 10),
			     "GT %d should be in C6\n", gt_id);
		igt_assert(get_freq(fd, gt_id, "act") == 0);
	} else {
		act = get_freq(fd, gt_id, "act");
		igt_assert(rpn <= act && act <= rpe);
	}

	igt_debug("Finished testing range request\n");
}

/**
 * SUBTEST: freq_low_max
 * Description: Test frequency request to minimal and maximum values
 */

static void test_freq_low_max(int fd, int gt_id)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");
	uint32_t rpe = get_freq(fd, gt_id, "rpe");

	/*
	 *  When max request < min request, max is ignored and min works like
	 * a fixed one. Let's assert this assumption
	 */
	igt_assert(set_freq(fd, gt_id, "min", rpe) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rpn) > 0);
	usleep(ACT_FREQ_LATENCY_US);

	/* Refresh value of rpe, pcode could have adjusted it */
	rpe = get_freq(fd, gt_id, "rpe");

	igt_assert(get_freq(fd, gt_id, "cur") == rpe);

	if (!xe_is_gt_in_c6(fd, gt_id))
		igt_assert(get_freq(fd, gt_id, "act") == rpe);
}

/**
 * SUBTEST: freq_suspend
 * Description: Check frequency after returning from suspend
 */

static void test_suspend(int fd, int gt_id)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");

	igt_assert(set_freq(fd, gt_id, "min", rpn) > 0);
	igt_assert(set_freq(fd, gt_id, "max", rpn) > 0);
	usleep(ACT_FREQ_LATENCY_US);
	igt_assert(get_freq(fd, gt_id, "cur") == rpn);

	igt_system_suspend_autoresume(SUSPEND_STATE_S3,
				      SUSPEND_TEST_NONE);

	igt_assert(get_freq(fd, gt_id, "min") == rpn);
	igt_assert(get_freq(fd, gt_id, "max") == rpn);
}

/**
 * SUBTEST: freq_reset
 * Description: test frequency reset only once
 *
 * SUBTEST: freq_reset_multiple
 * Description: test frequency reset multiple times
 */

static void test_reset(int fd, int gt_id, int cycles)
{
	uint32_t rpn = get_freq(fd, gt_id, "rpn");

	for (int i = 0; i < cycles; i++) {
		igt_assert_f(set_freq(fd, gt_id, "min", rpn) > 0,
			     "Failed after %d good cycles\n", i);
		igt_assert_f(set_freq(fd, gt_id, "max", rpn) > 0,
			     "Failed after %d good cycles\n", i);
		usleep(ACT_FREQ_LATENCY_US);
		igt_assert_f(get_freq(fd, gt_id, "cur") == rpn,
			     "Failed after %d good cycles\n", i);

		xe_force_gt_reset_async(fd, gt_id);

		igt_assert_f(get_freq(fd, gt_id, "min") == rpn,
			     "Failed after %d good cycles\n", i);
		igt_assert_f(get_freq(fd, gt_id, "max") == rpn,
			     "Failed after %d good cycles\n", i);
	}
}

static void test_spin(int fd, struct drm_xe_engine_class_instance *eci, bool fixed)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t addr = 0x1a0000;
	struct xe_spin_opts spin_opts = {
		.addr = addr,
		.preempt = false
	};
	struct xe_spin *spin;
	uint32_t exec_queue;
	uint32_t syncobj;
	size_t bo_size;
	uint32_t bo;
	uint32_t vm;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*spin);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id), 0);
	spin = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	syncobj = syncobj_create(fd, 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	xe_spin_init(spin, &spin_opts);

	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].handle = syncobj;

	exec.exec_queue_id = exec_queue;
	exec.address = addr;
	xe_exec(fd, &exec);

	xe_spin_wait_started(spin);
	usleep(50000);
	igt_assert(!syncobj_wait(fd, &syncobj, 1, 1, 0, NULL));

	igt_info("Running on GT %d Engine %s:%d\n", eci->gt_id,
		 xe_engine_class_string(eci->engine_class), eci->engine_instance);

	if (fixed)
		test_freq_fixed(fd, eci->gt_id, false);
	else
		test_freq_range(fd, eci->gt_id, false);

	xe_spin_end(spin);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	xe_exec_queue_destroy(fd, exec_queue);

	munmap(spin, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

igt_main
{
	int fd;
	int gt;
	struct drm_xe_engine_class_instance *hwe;
	uint32_t stash_min;
	uint32_t stash_max;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);

		/* The defaults are the same. Stashing the gt0 is enough */
		stash_min = get_freq(fd, 0, "min");
		stash_max = get_freq(fd, 0, "max");
	}

	igt_subtest("throttle_basic_api") {
		xe_for_each_gt(fd, gt)
			test_throttle_basic_api(fd, gt);
	}

	igt_subtest("freq_basic_api") {
		xe_for_each_gt(fd, gt)
			test_freq_basic_api(fd, gt);
	}

	igt_subtest("freq_fixed_idle") {
		xe_for_each_gt(fd, gt) {
			igt_require_f(igt_wait(xe_is_gt_in_c6(fd, gt), 1000, 10),
				      "GT %d should be in C6\n", gt);
			test_freq_fixed(fd, gt, true);
		}
	}

	igt_subtest("freq_fixed_exec") {
		xe_for_each_gt(fd, gt) {
			xe_for_each_engine(fd, hwe) {
				if (hwe->gt_id != gt)
					continue;
				test_spin(fd, hwe, true);
			}
		}
	}

	igt_subtest("freq_range_idle") {
		xe_for_each_gt(fd, gt) {
			igt_require_f(igt_wait(xe_is_gt_in_c6(fd, gt), 1000, 10),
				      "GT %d should be in C6\n", gt);
			test_freq_range(fd, gt, true);
		}
	}

	igt_subtest("freq_range_exec") {
		xe_for_each_gt(fd, gt) {
			xe_for_each_engine(fd, hwe) {
				if (hwe->gt_id != gt)
					continue;
				test_spin(fd, hwe, false);
			}
		}
	}

	igt_subtest("freq_low_max") {
		xe_for_each_gt(fd, gt) {
			test_freq_low_max(fd, gt);
		}
	}

	igt_subtest("freq_suspend") {
		xe_for_each_gt(fd, gt) {
			test_suspend(fd, gt);
		}
	}

	igt_subtest("freq_reset") {
		xe_for_each_gt(fd, gt) {
			test_reset(fd, gt, 1);
		}
	}

	igt_subtest("freq_reset_multiple") {
		xe_for_each_gt(fd, gt) {
			test_reset(fd, gt, 50);
		}
	}

	igt_fixture {
		xe_for_each_gt(fd, gt) {
			set_freq(fd, gt, "min", stash_min);
			set_freq(fd, gt, "max", stash_max);
		}
		drm_close_driver(fd);
	}
}
