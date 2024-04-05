/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#include "igt.h"
#include "igt_kms.h"

#define DISABLE_FILTER			0
#define MIN_VALUE			1
#define MAX_VALUE			255
#define INCREMENT_VALUE			10

/**
 * TEST: kms sharpness filter
 * Category: Display
 * Description: Test to validate content adaptive sharpness filter
 * Driver requirement: xe
 * Mega feature: General Display Features
 * Test category: functionality test
 * Functionality: casf
 *
 * SUBTEST: filter-strength
 * Description: Verify that varying strength (0-255), affects the degree of sharpeness applied.
 *
 * SUBTEST: user-defined-filter-strength
 * Description: Verify user provided strength(0-255),which affects the degree of sharpeness applied.
 */

IGT_TEST_DESCRIPTION("Test to validate content adaptive sharpness filter");

/*
 * Until the CRC support is added test needs to be invoked with
 * --interactive|--i to manually verify if "sharpened" image
 * is seen without corruption for each subtest.
 */

enum test_type {
	TEST_FILTER_STRENGTH,
};

const char *files[] = {
	"golden-640.png",
	"nature-1920.png",
	"butterfly.png"
};

const char *png;
int filter_value = DISABLE_FILTER;
char arrow, res;
int height, width;

typedef struct {
	int drm_fd;
	enum pipe pipe_id;
	struct igt_fb fb[4];
	igt_pipe_t *pipe;
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *plane[4];
	drmModeModeInfo *mode;
	int filter_strength;
	uint64_t modifier;
	const char *modifier_name;
	uint32_t format;
	char name[40];
} data_t;

static int read_value_from_keyboard(void)
{
	char input[10];
	int value;
	char *endptr;

	while (1) {
		printf("Enter a value between 0 and 255 (or 'q' to skip): ");
		if (scanf("%9s", input) != 1) {
			printf("Input error. Please try again.\n");
			/* Clear the input buffer */
			while (getchar() != '\n');
			continue;
		}

		/* 'q' was pressed, return -1 to indicate skip */
		if (input[0] == 'q' && input[1] == '\0')
			return -1;

		/* Convert the input to an integer */
		value = strtol(input, &endptr, 10);

		if (*endptr == '\0' && value >= 0 && value <= 255)
			return value;
		else
			printf("Invalid input. Please enter a number between 0 and 255 or 'q' to skip.\n");

		/* Clear the input buffer */
		while (getchar() != '\n');
	}
}

static void set_filter_strength_on_pipe(data_t *data)
{
	igt_pipe_set_prop_value(&data->display, data->pipe_id,
				IGT_CRTC_SHARPNESS_STRENGTH,
				data->filter_strength);
}

static void paint_image(igt_fb_t *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	int img_x, img_y, img_w, img_h;

	img_x = img_y = 0;
	img_w = fb->width;
	img_h = fb->height;

	igt_paint_image(cr, png, img_x, img_y, img_w, img_h);

	igt_put_cairo_ctx(cr);
}

static void setup_fb(int fd, uint32_t format, uint64_t modifier, struct igt_fb *fb)
{
	int fb_id;

	fb_id = igt_create_fb(fd, width, height, format, modifier, fb);
	igt_assert(fb_id);

	paint_image(fb);
}

static void cleanup(data_t *data)
{
	for (int i = 0; i < ARRAY_SIZE(data->fb); i++)
		igt_remove_fb(data->drm_fd, &data->fb[i]);

	igt_output_set_pipe(data->output, PIPE_NONE);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void test_sharpness_filter(data_t *data,  enum test_type type)
{
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = data->mode;
	int ret;

	igt_display_reset(&data->display);
	igt_output_set_pipe(output, data->pipe_id);

	data->plane[0] = igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);
	igt_skip_on_f(!igt_plane_has_format_mod(data->plane[0], data->format, data->modifier),
		      "No requested format/modifier on pipe %s\n", kmstest_pipe_name(data->pipe_id));

	setup_fb(data->drm_fd, data->format, data->modifier, &data->fb[0]);
	igt_plane_set_fb(data->plane[0], &data->fb[0]);
	igt_plane_set_size(data->plane[0], mode->hdisplay, mode->vdisplay);

	/* Set filter strength property */
	set_filter_strength_on_pipe(data);
	igt_debug("Sharpened image should be observed for filter strength > 0\n");

	ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	igt_assert_eq(ret, 0);

}

static bool has_sharpness_filter(igt_pipe_t *pipe)
{
	return igt_pipe_obj_has_prop(pipe, IGT_CRTC_SHARPNESS_STRENGTH);
}

static void set_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {

		if (pipe != PIPE_A)
			continue;

		data->output = output;
		data->pipe_id = pipe;
		data->pipe = &display->pipes[data->pipe_id];
		data->mode = igt_output_get_mode(data->output);

		if (!has_sharpness_filter(data->pipe))
			continue;

		igt_output_set_pipe(output, pipe);

		if (!intel_pipe_output_combo_valid(display)) {
			igt_output_set_pipe(output, PIPE_NONE);
			continue;
		}
	}
}

data_t data = {};

igt_main
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_require(data.drm_fd >= 0);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Verify that varying strength(0-255), affects "
		     "the degree of sharpeness applied.");
	igt_subtest("filter-strength") {
		data.modifier = DRM_FORMAT_MOD_LINEAR;
		data.format = DRM_FORMAT_XRGB8888;
		data.filter_strength = filter_value;

		igt_info("Press 'h' for HD, 'f' for FHD, 'k' for 4K to select resolutions.\n");
retry:		res = getchar();
		if ((res != 'h') && (res != 'f') && (res != 'k'))
			goto retry;
		png = (res == 'h') ? files[0] : ((res == 'f') ? files[1] : files[2]);
		width = (res == 'h') ? 640 : ((res == 'f') ? 1920 : 3840);
		height = (res == 'h') ? 480 : ((res == 'f') ? 1080 : 2160);

		/* Run with sharpness filter disable */
		set_output(&data);
		test_sharpness_filter(&data, TEST_FILTER_STRENGTH);

		igt_info("Press up-arrow to increase valuea & down arrow to decrease it and 'q' for escape\n");

		/* Read the arrow key input */
		while ((arrow = getchar()) != 'q') {
			if (arrow != 65 && arrow != 66)
				continue;

			/* If up-aarow key is pressed, increment the value by INCREMENT_VALUE if it's within range */
			if (arrow == 65) {
				if( filter_value + INCREMENT_VALUE <= MAX_VALUE)
					filter_value += INCREMENT_VALUE;
				else
					filter_value = MAX_VALUE;
			}
			/* If down-arrow key is pressed, decrement the value by INCREMENT_VALUE if it's within range */
			else if (arrow == 66) {
			       if (filter_value - INCREMENT_VALUE >= MIN_VALUE)
					filter_value -= INCREMENT_VALUE;
				else
					filter_value = MIN_VALUE;
			}
			data.filter_strength = filter_value;
			igt_info("pipe-%s-%s-strength-%d \n", kmstest_pipe_name(data.pipe_id), data.output->name, filter_value);
			test_sharpness_filter(&data, TEST_FILTER_STRENGTH);
		}
		/* Clear the input buffer */
		while (getchar() != '\n');
		cleanup(&data);
	}

	igt_describe("Verify user provided strength(0-255),which affects "
		     "the degree of sharpeness applied.");
	igt_subtest("user-defined-filter-strength") {
		data.modifier = DRM_FORMAT_MOD_LINEAR;
		data.format = DRM_FORMAT_XRGB8888;
		set_output(&data);
recheck:
		igt_info("Press 'h' for HD, 'f' for FHD, 'k' for 4K to select resolutions.\n");
		res = getchar();
		if ((res != 'h') && (res != 'f') && (res != 'k'))
			goto recheck;
		png = (res == 'h') ? files[0] : ((res == 'f') ? files[1] : files[2]);
		width = (res == 'h') ? 640 : ((res == 'f') ? 1920 : 3840);
		height = (res == 'h') ? 480 : ((res == 'f') ? 1080 : 2160);

		/* Read the arrow key input */
		while (1) {
			filter_value = read_value_from_keyboard();
			if (filter_value == -1) {
				printf("Skipping input as 'q' key was pressed.\n");
				break;
			} else {
				data.filter_strength = filter_value;
				igt_info("pipe-%s-%s-strength-%d \n", kmstest_pipe_name(data.pipe_id), data.output->name, filter_value);
				test_sharpness_filter(&data, TEST_FILTER_STRENGTH);
			}
		}
		cleanup(&data);
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
