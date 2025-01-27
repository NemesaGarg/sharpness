/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * TEST: kms vrr
 * Category: Display
 * Description: Test to validate diffent features of VRR
 * Driver requirement: i915, xe
 * Functionality: adaptive_sync
 * Mega feature: Adaptive Sync
 * Test category: functionality test
 */

#include "igt.h"
#include "i915/intel_drrs.h"
#include "sw_sync.h"
#include <fcntl.h>
#include <signal.h>

/**
 * SUBTEST: cmrr
 * Description: Test to validate the content rate to exactly match with the
 * 		requested rate without any frame drops.
 * Functionality: CMRR
 *
 * SUBTEST: flip-basic
 * Description: Tests that VRR is enabled and that the difference between flip
 *              timestamps converges to the requested rate
 *
 * SUBTEST: flip-basic-fastset
 * Description: Tests that VRR is enabled without modeset and that the difference
 *              between flip timestamps converges to the requested rate
 *
 * SUBTEST: flip-dpms
 * Description: Tests with DPMS that VRR is enabled and that the difference
 *              between flip timestamps converges to the requested rate.
 * Functionality: adaptive_sync, dpms
 *
 * SUBTEST: flip-suspend
 * Description: Tests that VRR is enabled and that the difference between flip
 *              timestamps converges to the requested rate in a suspend test
 * Functionality: adaptive_sync, suspend
 *
 * SUBTEST: flipline
 * Description: Make sure that flips happen at flipline decision boundary.
 *
 * SUBTEST: seamless-rr-switch-vrr
 * Description: Test to switch RR seamlessly without modeset.
 * Functionality: adaptive_sync, lrr
 *
 * SUBTEST: seamless-rr-switch-drrs
 * Description: Test to switch RR seamlessly without modeset.
 * Functionality: adaptive_sync, drrs
 *
 * SUBTEST: seamless-rr-switch-virtual
 * Description: Test to create a Virtual Mode in VRR range and switch to it
 * 		without a full modeset.
 * Functionality: LRR
 *
 * SUBTEST: max-min
 * Description: Oscillates between highest and lowest refresh each frame for
 *              manual flicker profiling
 *
 * SUBTEST: negative-basic
 * Description: Make sure that VRR should not be enabled on the Non-VRR panel.
 */

#define NSECS_PER_SEC (1000000000ull)

/*
 * Each test measurement step runs for ~5 seconds.
 * This gives a decent sample size + enough time for any adaptation to occur if necessary.
 */
#define TEST_DURATION_NS (5000000000ull)

#define CMRR_PRECISION_TOLERANCE	10
#define VREFRESH_MODIFIER	0.1

enum {
	TEST_BASIC = 1 << 0,
	TEST_DPMS = 1 << 1,
	TEST_SUSPEND = 1 << 2,
	TEST_FLIPLINE = 1 << 3,
	TEST_SEAMLESS_VRR = 1 << 4,
	TEST_SEAMLESS_DRRS = 1 << 5,
	TEST_SEAMLESS_VIRTUAL_RR = 1 << 6,
	TEST_FASTSET = 1 << 7,
	TEST_MAXMIN = 1 << 8,
	TEST_CMRR = 1 << 9,
	TEST_NEGATIVE = 1 << 10,
};

enum {
	HIGH_RR_MODE,
	LOW_RR_MODE,
	RR_MODES_COUNT,
};

typedef struct range {
	unsigned int min;
	unsigned int max;
} range_t;

typedef struct vtest_ns {
	uint64_t min;
	uint64_t rate_ns;
	uint64_t max;
} vtest_ns_t;

typedef struct data {
	igt_display_t display;
	int drm_fd;
	igt_plane_t *primary;
	igt_fb_t fb[2];
	range_t range;
	drmModeModeInfo switch_modes[RR_MODES_COUNT];
	vtest_ns_t vtest_ns;
	uint64_t duration_ns;
	bool static_image;
} data_t;

typedef void (*test_t)(data_t*, enum pipe, igt_output_t*, uint32_t);

/* Converts a timespec structure to nanoseconds. */
static uint64_t timespec_to_ns(struct timespec *ts)
{
	return ts->tv_sec * NSECS_PER_SEC + ts->tv_nsec;
}

/*
 * Gets an event from DRM and returns its timestamp in nanoseconds.
 * Asserts if the event from DRM is not matched with requested one.
 *
 * This blocks until the event is received.
 */
static uint64_t get_kernel_event_ns(data_t *data, uint32_t event)
{
	struct drm_event_vblank ev;

	igt_set_timeout(1, "Waiting for an event\n");
	igt_assert_eq(read(data->drm_fd, &ev, sizeof(ev)), sizeof(ev));
	igt_assert_eq(ev.base.type, event);
	igt_reset_timeout();

	return ev.tv_sec * NSECS_PER_SEC + ev.tv_usec * 1000ull;
}

/*
 * Returns the current CLOCK_MONOTONIC time in nanoseconds.
 * The regular IGT helpers can't be used since they default to
 * CLOCK_MONOTONIC_RAW - which isn't what the kernel uses for its timestamps.
 */
static uint64_t get_time_ns(void)
{
	struct timespec ts;
	memset(&ts, 0, sizeof(ts));
	errno = 0;

	if (!clock_gettime(CLOCK_MONOTONIC, &ts))
		return timespec_to_ns(&ts);

	igt_warn("Could not read monotonic time: %s\n", strerror(errno));
	igt_fail(-errno);

	return 0;
}

/* Returns the rate duration in nanoseconds for the given refresh rate. */
static uint64_t rate_from_refresh(uint64_t refresh)
{
	return refresh ? (NSECS_PER_SEC / refresh) : 0;
}

/* Instead of running on default mode, loop through the connector modes
 * and find the mode with max refresh rate to exercise full vrr range.
 */
static drmModeModeInfo
output_mode_with_maxrate(igt_output_t *output, unsigned int vrr_max)
{
	int i;
	drmModeConnectorPtr connector = output->config.connector;
	drmModeModeInfo mode = *igt_output_get_mode(output);

	igt_info("Default Mode: ");
	kmstest_dump_mode(&mode);

	for (i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].vrefresh > mode.vrefresh &&
		    connector->modes[i].vrefresh <= vrr_max)
			mode = connector->modes[i];

	return mode;
}

static drmModeModeInfo
low_rr_mode_with_same_res(igt_output_t *output, unsigned int vrr_min)
{
	int i;
	drmModeConnectorPtr connector = output->config.connector;
	drmModeModeInfo mode = *igt_output_get_mode(output);

	for (i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].hdisplay == mode.hdisplay &&
		    connector->modes[i].vdisplay == mode.vdisplay &&
		    connector->modes[i].clock < mode.clock &&
		    connector->modes[i].vrefresh < mode.vrefresh &&
		    connector->modes[i].vrefresh >= vrr_min)
			mode = connector->modes[i];

	return mode;
}

static drmModeModeInfo
virtual_rr_vrr_range_mode(igt_output_t *output, unsigned int virtual_refresh_rate)
{
	drmModeModeInfo mode = *igt_output_get_mode(output);
	uint64_t clock_hz = mode.clock * 1000;

	mode.vtotal = clock_hz / (mode.htotal * virtual_refresh_rate);
	mode.vrefresh = virtual_refresh_rate;

	return mode;
}

static bool
is_cmrr_mode(drmModeModeInfoPtr mode)
{
	int calculated_refresh, actual_refresh, pixel_clock_per_line;

	actual_refresh = mode->vrefresh * 100;
	pixel_clock_per_line = mode->clock * 1000 / mode->htotal;
	calculated_refresh = pixel_clock_per_line * 100 / mode->vtotal;

	if ((actual_refresh - calculated_refresh) < CMRR_PRECISION_TOLERANCE)
		return false;

	return true;
}

/* Read min and max vrr range from the connector debugfs. */
static range_t
get_vrr_range(data_t *data, igt_output_t *output)
{
	char buf[256];
	char *start_loc;
	int fd, res;
	range_t range;

	fd = igt_debugfs_connector_dir(data->drm_fd, output->name, O_RDONLY);
	igt_assert(fd >= 0);

	res = igt_debugfs_simple_read(fd, "vrr_range", buf, sizeof(buf));
	igt_require(res > 0);

	close(fd);

	igt_assert(start_loc = strstr(buf, "Min: "));
	igt_assert_eq(sscanf(start_loc, "Min: %u", &range.min), 1);

	igt_assert(start_loc = strstr(buf, "Max: "));
	igt_assert_eq(sscanf(start_loc, "Max: %u", &range.max), 1);

	return range;
}

/* Returns true if driver supports VRR. */
static bool has_vrr(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_VRR_CAPABLE);
}

/* Returns true if an output supports VRR. */
static bool vrr_capable(igt_output_t *output)
{
	return igt_output_get_prop(output, IGT_CONNECTOR_VRR_CAPABLE);
}

/* Toggles variable refresh rate on the pipe. */
static void set_vrr_on_pipe(data_t *data, enum pipe pipe,
			    bool need_modeset, bool enabled)
{
	igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED,
				enabled);

	igt_assert(igt_display_try_commit_atomic(&data->display,
						 need_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0,
						 NULL) == 0);
}

static void paint_bar(cairo_t *cr, unsigned int x, unsigned int y,
		      unsigned int w, unsigned int h,
		      unsigned int bar, unsigned int num_bars,
		      float start_r, float start_g, float start_b,
		      float end_r, float end_g, float end_b)
{
	float progress = (float)bar / (float)num_bars;
	float color[] = {
		start_r + progress * (end_r - start_r),
		start_g + progress * (end_g - start_g),
		start_b + progress * (end_b - start_b)
	};
	igt_paint_color(cr, x, y, w, h,
			color[0] > 0 ? color[0] : 0.0,
			color[1] > 0 ? color[1] : 0.0,
			color[2] > 0 ? color[2] : 0.0);
}

/* Prepare the display for testing on the given pipe. */
static void prepare_test(data_t *data, igt_output_t *output, enum pipe pipe)
{
	unsigned int num_bars = 256;
	drmModeModeInfo mode;
	cairo_t *cr;
	int bar_width, bar_height, bar_remaining, horizontal_bar_height;
	int num_painted_fbs;

	mode = *igt_output_get_mode(output);

	data->vtest_ns.min = rate_from_refresh(data->range.min);
	data->vtest_ns.max = rate_from_refresh(data->range.max);

	/* If unspecified on the command line, default rate to the midpoint */
	if (data->vtest_ns.rate_ns == 0) {
		range_t *range = &data->range;
		data->vtest_ns.rate_ns = rate_from_refresh(
						(range->min + range->max) / 2);
	}

	if (data->duration_ns == 0)
		data->duration_ns = TEST_DURATION_NS;

	/* Prepare resources */
	igt_create_color_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0.50, 0.50, 0.50, &data->fb[0]);

	igt_create_color_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0.50, 0.50, 0.50, &data->fb[1]);

	bar_width = mode.hdisplay / num_bars;
	horizontal_bar_height = mode.vdisplay / 8;
	bar_height = mode.vdisplay - horizontal_bar_height * 2;
	bar_remaining = mode.hdisplay % bar_width;
	num_painted_fbs = data->static_image ? 2 : 1;
	for (int i = 0; i < num_painted_fbs; ++i) {
		cr = igt_get_cairo_ctx(data->drm_fd, &data->fb[i]);
		for (int j = 0; j < num_bars; ++j) {
			unsigned int width = bar_width;
			if (j == num_bars - 1)
				width += bar_remaining;

			/* Red->Green->Blue gradient */
			if (j < num_bars / 2)
				paint_bar(cr, j * bar_width, 0, width,
					  bar_height,
					  j, num_bars / 2,
					  1.0, 0.0, 0.0, 0.0, 1.0, 0.0);
			else
				paint_bar(cr, j * bar_width, 0, width,
					  bar_height,
					  j - num_bars / 2, num_bars / 2,
					  0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
		}
		igt_paint_color(cr, 0, mode.vdisplay - horizontal_bar_height,
				mode.hdisplay, horizontal_bar_height,
				1.00, 1.00, 1.00);
		igt_put_cairo_ctx(cr);
	}

	/* Take care of any required modesetting before the test begins. */
	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(data->primary, &data->fb[0]);

	/* Clear vrr_enabled state before enabling it, because
	 * it might be left enabled if the previous test fails.
	 */
	igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED, 0);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

/* Performs an atomic non-blocking page-flip on a pipe. */
static void
do_flip(data_t *data, igt_fb_t *fb)
{
	int ret;

	igt_set_timeout(1, "Scheduling page flip\n");

	igt_plane_set_fb(data->primary, fb);

	do {
		ret = igt_display_try_commit_atomic(&data->display,
				  DRM_MODE_ATOMIC_NONBLOCK |
				  DRM_MODE_PAGE_FLIP_EVENT,
				  data);
	} while (ret == -EBUSY);

	igt_assert_eq(ret, 0);
	igt_reset_timeout();
}

/*
 * Flips at the given rate and measures against the expected value.
 * Returns the pass rate as a percentage from 0 - 100.
 *
 * The VRR API is quite flexible in terms of definition - the driver
 * can arbitrarily restrict the bounds further than the absolute
 * min and max range. But VRR is really about extending the flip
 * to prevent stuttering or to match a source content rate.
 */
static uint32_t
flip_and_measure(data_t *data, igt_output_t *output, enum pipe pipe,
		 uint64_t *rates_ns, int num_rates, uint64_t duration_ns)
{
	uint64_t start_ns, last_event_ns, target_ns, exp_rate_ns;
	uint32_t total_flip = 0, total_pass = 0;
	bool front = false;
	vtest_ns_t vtest_ns = data->vtest_ns;
	uint64_t *threshold_hi, *threshold_lo;

	threshold_hi = malloc(sizeof(uint64_t) * num_rates);
	threshold_lo = malloc(sizeof(uint64_t) * num_rates);
	igt_assert(threshold_hi && threshold_lo);

	for (int i = 0; i < num_rates; ++i) {
		if ((rates_ns[i] <= vtest_ns.min) && (rates_ns[i] >= vtest_ns.max))
			exp_rate_ns = rates_ns[i];
		else
			exp_rate_ns = vtest_ns.max;

		/* Allow ~1 Hz deviation for different reasons causing delay. */
		threshold_hi[i] = NSECS_PER_SEC / (((float)NSECS_PER_SEC / exp_rate_ns) + 1);
		threshold_lo[i] = NSECS_PER_SEC / (((float)NSECS_PER_SEC / exp_rate_ns) - 1);

		igt_info("Requested rate[%d]: %"PRIu64" ns, Expected rate between: %"PRIu64" ns to %"PRIu64" ns\n",
				i, rates_ns[i], threshold_hi[i], threshold_lo[i]);
	}

	/* Align with the flip completion event to speed up convergence. */
	do_flip(data, &data->fb[0]);
	start_ns = last_event_ns = target_ns = get_kernel_event_ns(data,
							DRM_EVENT_FLIP_COMPLETE);

	for (int i = 0;;++i) {
		uint64_t event_ns, wait_ns;
		int64_t diff_ns;
		uint64_t rate_ns = rates_ns[(i + 1) % num_rates];
		uint64_t th_lo_ns = threshold_lo[i % num_rates];
		uint64_t th_hi_ns = threshold_hi[i % num_rates];

		front = !front;
		do_flip(data, front ? &data->fb[1] : &data->fb[0]);

		/* We need to cpture flip event instead of vblank event,
		 * because vblank is triggered after each frame, but depending
		 * on the vblank evasion time flip might or might not happen in
		 * that same frame.
		 */
		event_ns = get_kernel_event_ns(data, DRM_EVENT_FLIP_COMPLETE);

		igt_debug("event_ns - last_event_ns: %"PRIu64"\n",
						(event_ns - last_event_ns));

		/*
		 * Check if the difference between the two flip timestamps
		 * was within the required threshold from the expected rate.
		 */
		diff_ns = event_ns - last_event_ns;

		if (llabs(diff_ns) < th_lo_ns && llabs(diff_ns) > th_hi_ns)
			total_pass += 1;

		last_event_ns = event_ns;
		total_flip += 1;

		if (event_ns - start_ns > duration_ns)
			break;

		/*
		 * Burn CPU until next timestamp, sleeping isn't accurate enough.
		 * The target timestamp is based on the delta b/w event timestamps
		 * and whatever the time left to reach the expected refresh rate.
		 */
		diff_ns = event_ns - target_ns;
		wait_ns = ((diff_ns + rate_ns - 1) / rate_ns) * rate_ns;
		wait_ns -= diff_ns;
		target_ns = event_ns + wait_ns;

		while (get_time_ns() < target_ns - 10);
	}

	igt_info("Completed %u flips, %u were in threshold for [", total_flip, total_pass);
	for (int i = 0; i < num_rates; ++i) {
		igt_info("(%llu Hz) %"PRIu64"ns%s", (NSECS_PER_SEC/rates_ns[i]), rates_ns[i],
			 i < num_rates - 1 ? "," : "");
	}
	igt_info("]\n");

	return total_flip ? ((total_pass * 100) / total_flip) : 0;
}

static uint32_t
flip_and_measure_cmrr(data_t *data, igt_output_t *output, enum pipe pipe,
		      uint64_t duration_ns)
{
	uint64_t start_ns, last_event_ns, event_ns;
	uint32_t total_flip = 0, total_pass = 0;
	bool front = false;
	drmModeModeInfoPtr mode = igt_output_get_mode(output);
	uint64_t req_rate_ns = rate_from_refresh(mode->vrefresh + VREFRESH_MODIFIER);
	uint64_t exp_rate_ns = rate_from_refresh(mode->vrefresh);
	uint64_t threshold_ns = exp_rate_ns / mode->vdisplay; /* Upto 1 scan line. */

	igt_info("CMRR on: requested rate: %"PRIu64" ns (%.2f Hz) "
		 "expected rate: %"PRIu64" ns - %"PRIu64" ns (%.2f-%.2f Hz)\n",
		 req_rate_ns, (mode->vrefresh + VREFRESH_MODIFIER),
		 (exp_rate_ns - threshold_ns), (exp_rate_ns + threshold_ns),
		 (float)NSECS_PER_SEC / (exp_rate_ns + threshold_ns),
		 (float)NSECS_PER_SEC / (exp_rate_ns - threshold_ns));

	do_flip(data, &data->fb[0]);
	start_ns = last_event_ns = get_kernel_event_ns(data, DRM_EVENT_FLIP_COMPLETE);
	do {
		int64_t target_ns, wait_ns, diff_ns = exp_rate_ns;

		front = !front;
		do_flip(data, front ? &data->fb[1] : &data->fb[0]);

		event_ns = get_kernel_event_ns(data, DRM_EVENT_FLIP_COMPLETE);
		igt_debug("event_ns - last_event_ns: %"PRIu64" ns (%.2f Hz)\n",
			  event_ns - last_event_ns, (float)NSECS_PER_SEC / (event_ns - last_event_ns));

		diff_ns -= event_ns - last_event_ns;
		if (llabs(diff_ns) <= threshold_ns)
			total_pass += 1;

		last_event_ns = event_ns;
		total_flip += 1;

		diff_ns = event_ns - start_ns;
		wait_ns = ((diff_ns + req_rate_ns - 1) / req_rate_ns) * req_rate_ns;
		wait_ns -= diff_ns;
		target_ns = event_ns + wait_ns;
		while (get_time_ns() < target_ns - 10);
	} while (event_ns - start_ns <= duration_ns);

	igt_info("Completed %u flips, %u vblanks were in threshold for (%.2f Hz) %"PRIu64"ns.\n",
		 total_flip, total_pass, (mode->vrefresh + VREFRESH_MODIFIER), req_rate_ns);

	return total_flip ? ((total_pass * 100) / total_flip) : 0;
}

/* Basic VRR flip functionality test - enable, measure, disable, measure */
static void
test_basic(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t flags)
{
	uint32_t result;
	vtest_ns_t vtest_ns;
	range_t range;
	uint64_t rate[] = {0};

	prepare_test(data, output, pipe);
	range = data->range;
	vtest_ns = data->vtest_ns;
	rate[0] = vtest_ns.rate_ns;

	igt_info("VRR Test execution on %s, PIPE_%s with VRR range: (%u-%u) Hz\n",
		 output->name, kmstest_pipe_name(pipe), range.min, range.max);
	igt_info("Override Mode: ");
	kmstest_dump_mode(&data->switch_modes[HIGH_RR_MODE]);

	set_vrr_on_pipe(data, pipe, !(flags & TEST_FASTSET), true);

	/*
	 * Do a short run with VRR, but don't check the result.
	 * This is to make sure we were actually in the middle of
	 * active flipping before doing the DPMS/suspend steps.
	 */
	flip_and_measure(data, output, pipe, rate, 1, 250000000ull);

	if (flags & TEST_DPMS) {
		kmstest_set_connector_dpms(output->display->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(output->display->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);

	/*
	 * Check flipline mode by making sure that flips happen at flipline
	 * decision boundary.
	 *
	 * Example: if range is 40 - 60Hz and
	 * if refresh_rate > 60Hz:
	 *      Flip should happen at the flipline boundary & returned refresh rate
	 *      would be 60Hz.
	 * if refresh_rate is 50Hz:
	 *      Flip will happen right away so returned refresh rate is 50Hz.
	 * if refresh_rate < 40Hz:
	 *      h/w will terminate the vblank at Vmax which is obvious.
	 *      So, vblank termination should happen at Vmax, and flip done at
	 *      next Vmin.
	 */
	if (flags & TEST_FLIPLINE) {
		rate[0] = rate_from_refresh(range.max + 5);
		result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
		igt_assert_f(result > 75,
			     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR on threshold not reached, result was %u%%\n",
			     (range.max + 5), rate[0], result);
	}

	if (flags & ~(TEST_NEGATIVE | TEST_MAXMIN)) {
		rate[0] = vtest_ns.rate_ns;
		result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
		igt_assert_f(result > 75,
			     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR on threshold not reached, result was %u%%\n",
			     ((range.max + range.min) / 2), rate[0], result);
	}

	if (flags & TEST_FLIPLINE) {
		rate[0] = rate_from_refresh(range.min - 10);
		result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
		igt_assert_f(result < 50,
			     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR on threshold exceeded, result was %u%%\n",
			     (range.min - 10), rate[0], result);
	}

	if (flags & TEST_MAXMIN) {
		unsigned int range_min =
			/* For Intel h/w tweak the min rate, as h/w will terminate the vblank at Vmax. */
			is_intel_device(data->drm_fd) ? (range.min + 2) : range.min;
		uint64_t maxmin_rates[] = {vtest_ns.max, rate_from_refresh(range_min)};

		result = flip_and_measure(data, output, pipe, maxmin_rates, 2, data->duration_ns);
		igt_assert_f(result > 75,
			     "Refresh rates (%u/%u Hz) %"PRIu64"ns/%"PRIu64"ns: Target VRR on threshold not reached, result was %u%%\n",
			     range.max, range_min, maxmin_rates[0], maxmin_rates[1], result);
		return;
	}

	/*
	 * If we request VRR on a non-VRR panel, it is unlikely to reject the
	 * modeset. And the expected behavior is the same as disabling VRR on
	 * a VRR capable panel.
	 */
	set_vrr_on_pipe(data, pipe, !(flags & TEST_FASTSET), (flags & TEST_NEGATIVE) ? true : false);
	rate[0] = vtest_ns.rate_ns;
	result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
	igt_assert_f(result < 10,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR %s threshold exceeded, result was %u%%\n",
		     ((range.max + range.min) / 2), rate[0], (flags & TEST_NEGATIVE)? "on" : "off", result);
}

static void
test_seamless_rr_basic(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t flags)
{
	uint32_t result;
	vtest_ns_t vtest_ns;
	uint64_t rate[] = {0};
	bool vrr = !!(flags & TEST_SEAMLESS_VRR);

	igt_info("Use HIGH_RR Mode as default (VRR: %s): ", vrr ? "ON" : "OFF");
	kmstest_dump_mode(&data->switch_modes[HIGH_RR_MODE]);

	prepare_test(data, output, pipe);
	vtest_ns = data->vtest_ns;

	if (vrr)
		set_vrr_on_pipe(data, pipe, false, true);
	else {
		/*
		 * Sink with DRRS and VRR can be in downclock mode.
		 * so switch to High clock mode as test preparation
		 */
		igt_output_override_mode(output, &data->switch_modes[HIGH_RR_MODE]);
		igt_assert(igt_display_try_commit_atomic(&data->display, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0);
	}

	rate[0] = vtest_ns.max;
	result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
	igt_assert_f(result > 75,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR %s threshold not reached, result was %u%%\n",
		     data->range.max, rate[0], vrr ? "on" : "off", result);

	/* Switch to low rr mode without modeset. */
	igt_info("Switch to LOW_RR Mode (VRR: %s): ", vrr ? "ON" : "OFF");
	kmstest_dump_mode(&data->switch_modes[LOW_RR_MODE]);
	igt_output_override_mode(output, &data->switch_modes[LOW_RR_MODE]);
	igt_assert(igt_display_try_commit_atomic(&data->display, 0, NULL) == 0);

	rate[0] = vtest_ns.min;
	result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
	igt_assert_f(result > 75,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR %s threshold not reached, result was %u%%\n",
		     data->range.min, rate[0], vrr ? "on" : "off", result);

	/* Switch back to high rr mode without modeset. */
	igt_info("Switch back to HIGH_RR Mode (VRR: %s): ", vrr ? "ON" : "OFF");
	kmstest_dump_mode(&data->switch_modes[HIGH_RR_MODE]);
	igt_output_override_mode(output, &data->switch_modes[HIGH_RR_MODE]);
	igt_assert(igt_display_try_commit_atomic(&data->display, 0, NULL) == 0);

	rate[0] = vtest_ns.rate_ns;
	result = flip_and_measure(data, output, pipe, rate, 1, data->duration_ns);
	igt_assert_f(vrr ? (result > 75) : (result < 10),
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR %s threshold %s, result was %u%%\n",
		     ((data->range.max + data->range.min) / 2), rate[0],
		     vrr ? "on" : "off", vrr ? "not reached" : "exceeded", result);
}

static void
test_seamless_virtual_rr_basic(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t flags)
{
	uint32_t result;
	unsigned int vrefresh;
	uint64_t rate[] = {0};

	igt_info("Use HIGH_RR Mode as default\n");
	kmstest_dump_mode(&data->switch_modes[HIGH_RR_MODE]);

	prepare_test(data, output, pipe);
	rate[0] = rate_from_refresh(data->switch_modes[HIGH_RR_MODE].vrefresh);

	/*
	 * Sink with DRR and VRR can be in downclock mode so
	 * switch to highest refresh rate mode.
	 */
	igt_output_override_mode(output, &data->switch_modes[HIGH_RR_MODE]);
	igt_assert(igt_display_try_commit_atomic(&data->display, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0);

	result = flip_and_measure(data, output, pipe, rate, 1, TEST_DURATION_NS);
	igt_assert_f(result > 75,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target threshold not reached, result was %u%%\n",
		     data->switch_modes[HIGH_RR_MODE].vrefresh, rate[0], result);

	/* Switch to Virtual RR */
	for (vrefresh = data->range.min + 10; vrefresh < data->range.max; vrefresh += 10) {
		drmModeModeInfo virtual_mode = virtual_rr_vrr_range_mode(output, vrefresh);

		igt_info("Requesting Virtual Mode with Refresh Rate (%u Hz): \n", vrefresh);
		kmstest_dump_mode(&virtual_mode);

		igt_output_override_mode(output, &virtual_mode);
		igt_assert(igt_display_try_commit_atomic(&data->display, 0, NULL) == 0);

		rate[0] = rate_from_refresh(vrefresh);
		result = flip_and_measure(data, output, pipe, rate, 1, TEST_DURATION_NS);
		igt_assert_f(result > 75,
			     "Refresh rate (%u Hz) %"PRIu64"ns: Target threshold not reached, result was %u%%\n",
			     vrefresh, rate[0], result);
	}
}

static void
test_cmrr(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t flags)
{
	uint32_t result;
	int i;
	bool found = false;
	drmModeConnectorPtr connector = output->config.connector;
	drmModeModeInfo mode = *igt_output_get_mode(output);

	igt_info("CMRR test execution on %s, PIPE_%s with VRR range: (%u-%u) Hz\n",
		 output->name, kmstest_pipe_name(pipe), data->range.min, data->range.max);

	for (i = 0; i < connector->count_modes; i++) {
		if (is_cmrr_mode(&connector->modes[i])) {
			mode = connector->modes[i];

			found = true;
			break;
		}
	}

	igt_info("Selected mode: ");
	kmstest_dump_mode(&mode);

	if (!found) {
		igt_info("No CMRR mode found on %s, try to tweak the clock.\n", output->name);

		mode.clock = (mode.htotal * mode.vtotal * (mode.vrefresh + VREFRESH_MODIFIER)) / 1000;

		igt_info("Tweaked mode: ");
		kmstest_dump_mode(&mode);
	}

	igt_output_override_mode(output, &mode);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	prepare_test(data, output, pipe);
	result = flip_and_measure_cmrr(data, output, pipe, TEST_DURATION_NS * 2);
	igt_assert_f(result > 75,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target CMRR on threshold not reached, result was %u%%\n",
		     mode.vrefresh, rate_from_refresh(mode.vrefresh), result);
}

static void test_cleanup(data_t *data, enum pipe pipe, igt_output_t *output)
{
	if (vrr_capable(output))
		igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED, false);

	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_output_override_mode(output, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fb[1]);
	igt_remove_fb(data->drm_fd, &data->fb[0]);
}

static bool output_constraint(data_t *data, igt_output_t *output, uint32_t flags)
{
	if ((flags & (TEST_SEAMLESS_VRR | TEST_SEAMLESS_DRRS | TEST_CMRR)) &&
	    output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
		return false;

	if ((flags & TEST_SEAMLESS_DRRS) &&
	    !intel_output_has_drrs(data->drm_fd, output)) {
		igt_info("Selected panel won't support DRRS.\n");
		return false;
	}

	/* Reset output */
	igt_display_reset(&data->display);

	/* Capture VRR range */
	data->range = get_vrr_range(data, output);

	/*
	 * Override mode with max vrefresh.
	 *   - vrr_min range should be less than the override mode vrefresh.
	 *   - Limit the vrr_max range with the override mode vrefresh.
	 */
	data->switch_modes[HIGH_RR_MODE] = output_mode_with_maxrate(output, data->range.max);
	if (data->switch_modes[HIGH_RR_MODE].vrefresh < data->range.min)
		return false;

	data->range.max = data->switch_modes[HIGH_RR_MODE].vrefresh;
	igt_output_override_mode(output, &data->switch_modes[HIGH_RR_MODE]);

	/* Search for a low refresh rate mode. */
	if (!(flags & (TEST_SEAMLESS_VRR | TEST_SEAMLESS_DRRS | TEST_SEAMLESS_VIRTUAL_RR)))
		return true;

	data->switch_modes[LOW_RR_MODE] = low_rr_mode_with_same_res(output, data->range.min);
	if (data->switch_modes[LOW_RR_MODE].vrefresh == data->switch_modes[HIGH_RR_MODE].vrefresh)
		return false;

	data->range.min = data->switch_modes[LOW_RR_MODE].vrefresh;

	return true;
}

static bool config_constraint(data_t *data, igt_output_t *output, uint32_t flags)
{
	if (!has_vrr(output))
		return false;

	if (flags & TEST_SEAMLESS_DRRS)
		goto out;

	/* For Negative tests, panel should be non-vrr. */
	if ((flags & TEST_NEGATIVE) && vrr_capable(output))
		return false;

	if ((flags & ~TEST_NEGATIVE) && !vrr_capable(output))
		return false;

out:
	if (!output_constraint(data, output, flags))
		return false;

	return true;
}

/* Runs tests on outputs that are VRR capable. */
static void
run_vrr_test(data_t *data, test_t test, uint32_t flags)
{
	igt_output_t *output;

	for_each_connected_output(&data->display, output) {
		enum pipe pipe;

		if (!config_constraint(data, output, flags))
			continue;

		for_each_pipe(&data->display, pipe) {
			igt_output_set_pipe(output, pipe);

			if (!intel_pipe_output_combo_valid(&data->display)) {
				igt_output_set_pipe(output, PIPE_NONE);
				continue;
			}

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe), output->name)
				test(data, pipe, output, flags);

			test_cleanup(data, pipe, output);

			break;
		}
	}
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'd':
		data->duration_ns = atoi(optarg) * NSECS_PER_SEC;
		break;
	case 'r':
		data->vtest_ns.rate_ns = rate_from_refresh(atoi(optarg));
		break;
	case 's':
		data->static_image = true;
		break;
	}
	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "duration", .has_arg = true, .val = 'd', },
	{ .name = "refresh-rate", .has_arg = true, .val = 'r', },
	{ .name = "static-image", .has_arg = false, .val = 's', },
	{}
};

static const char help_str[] =
	"  --duration <duration-seconds>\t\tHow long to run the test for\n"
	"  --refresh-rate <refresh-hz>\t\tThe refresh rate to flip at\n"
	"  --static-image\t\tFlip a static image for flicker profiling\n";

static data_t data;

igt_main_args("drs:", long_opts, help_str, opt_handler, &data)
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Tests that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate");
	igt_subtest_with_dynamic("flip-basic")
		run_vrr_test(&data, test_basic, TEST_BASIC);

	igt_describe("Tests with DPMS that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate.");
	igt_subtest_with_dynamic("flip-dpms")
		run_vrr_test(&data, test_basic, TEST_DPMS);

	igt_describe("Tests that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate in a suspend test");
	igt_subtest_with_dynamic("flip-suspend")
		run_vrr_test(&data, test_basic, TEST_SUSPEND);

	igt_describe("Make sure that flips happen at flipline decision boundary.");
	igt_subtest_with_dynamic("flipline")
		run_vrr_test(&data, test_basic, TEST_FLIPLINE);

	igt_describe("Make sure that VRR should not be enabled on the Non-VRR panel.");
	igt_subtest_with_dynamic("negative-basic")
		run_vrr_test(&data, test_basic, TEST_NEGATIVE);

	igt_describe("Oscillates between highest and lowest refresh each frame for manual "
		     "flicker profiling");
	igt_subtest_with_dynamic("max-min")
		run_vrr_test(&data, test_basic, TEST_MAXMIN);

	igt_subtest_group {
		igt_fixture
			igt_require_intel(data.drm_fd);

		igt_describe("Test to switch RR seamlessly without modeset.");
		igt_subtest_with_dynamic("seamless-rr-switch-vrr")
			run_vrr_test(&data, test_seamless_rr_basic, TEST_SEAMLESS_VRR);

		igt_describe("Test to switch RR seamlessly without modeset.");
		igt_subtest_with_dynamic("seamless-rr-switch-drrs")
			run_vrr_test(&data, test_seamless_rr_basic, TEST_SEAMLESS_DRRS);

		igt_describe("Tests that VRR is enabled without modeset and that the difference "
			     "between flip timestamps converges to the requested rate");
		igt_subtest_with_dynamic("flip-basic-fastset")
			run_vrr_test(&data, test_basic, TEST_FASTSET);

		igt_describe("Test to switch to any custom virtual mode in VRR range without modeset.");
		igt_subtest_with_dynamic("seamless-rr-switch-virtual")
			run_vrr_test(&data, test_seamless_virtual_rr_basic, TEST_SEAMLESS_VIRTUAL_RR);

		igt_describe("Test to validate the the content rate exactly match with the "
			     "requested rate without any frame drops.");
		igt_subtest_with_dynamic("cmrr") {
			igt_require(intel_display_ver(intel_get_drm_devid(data.drm_fd)) >= 20);

			run_vrr_test(&data, test_cmrr, TEST_CMRR);
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
