/*
 * Copyright © 2015 Intel Corporation
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
 * 	Jeff McGee <jeff.mcgee@intel.com>
 */

#include <intel_bufmgr.h>
#include <i915_drm.h>
#include "intel_reg.h"
#include "drmtest.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "gen7_media.h"
#include "gen8_media.h"
#include "media_spin.h"

static const uint32_t spin_kernel[][4] = {
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 }, /* mov (8)r4.0<1>:ud r0.0<8;8;1>:ud */
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 }, /* mov (2)r4.0<1>.ud r2.0<2;2;1>:ud */
	{ 0x00000001, 0x20880608, 0x00000000, 0x00000003 }, /* mov (1)r4.8<1>:ud 0x3 */
	{ 0x00000001, 0x20a00608, 0x00000000, 0x00000000 }, /* mov (1)r5.0<1>:ud 0 */
	{ 0x00000040, 0x20a00208, 0x060000a0, 0x00000001 }, /* add (1)r5.0<1>:ud r5.0<0;1;0>:ud 1 */
	{ 0x01000010, 0x20000200, 0x02000020, 0x000000a0 }, /* cmp.e.f0.0 (1)null<1> r1<0;1;0> r5<0;1;0> */
	{ 0x00110027, 0x00000000, 0x00000000, 0xffffffe0 }, /* ~f0.0 while (1) -32 */
	{ 0x0c800031, 0x20000a00, 0x0e000080, 0x040a8000 }, /* send.dcdp1 (16)null<1> r4.0<0;1;0> 0x040a8000 */
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 }, /* mov (8)r112<1>:ud r0.0<8;8;1>:ud */
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 }, /* send.ts (16)null<1> r112<0;1;0>:d 0x82000010 */
};

/*
 * This sets up the media pipeline,
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 */

#define BATCH_STATE_SPLIT 2048
/* VFE STATE params */
#define THREADS 0
#define MEDIA_URB_ENTRIES 2
#define MEDIA_URB_SIZE 2
#define MEDIA_CURBE_SIZE 2

/* Offsets needed in gen_emit_media_object. In media_spin library this
 * values do not matter.
 */
#define xoffset 0
#define yoffset 0

static void
gen7_render_flush(struct intel_batchbuffer *batch, uint32_t batch_end)
{
	int ret;

	ret = drm_intel_bo_subdata(batch->bo, 0, 4096, batch->buffer);
	if (ret == 0)
		ret = drm_intel_bo_mrb_exec(batch->bo, batch_end,
					    NULL, 0, 0, 0);
	igt_assert(ret == 0);
}

static uint32_t
gen7_fill_kernel(struct intel_batchbuffer *batch,
		 const uint32_t kernel[][4], size_t size)
{
	uint32_t offset;

	offset = intel_batchbuffer_copy_data(batch, kernel, size, 64);

	return offset;
}

static uint32_t
gen7_fill_surface_state(struct intel_batchbuffer *batch,
			const struct igt_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen7_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	int ret;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_batchbuffer_subdata_alloc(batch, sizeof(*ss), 64);
	offset = intel_batchbuffer_subdata_offset(batch, ss);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	ss->ss1.base_addr = buf->bo->offset;
	ret = drm_intel_bo_emit_reloc(batch->bo,
				intel_batchbuffer_subdata_offset(batch, ss) + 4,
				buf->bo, 0,
				read_domain, write_domain);
	igt_assert(ret == 0);

	ss->ss2.height = igt_buf_height(buf) - 1;
	ss->ss2.width  = igt_buf_width(buf) - 1;

	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen8_fill_surface_state(struct intel_batchbuffer *batch,
			const struct igt_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen8_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	int ret;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_batchbuffer_subdata_alloc(batch, sizeof(*ss), 64);
	offset = intel_batchbuffer_subdata_offset(batch, ss);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	ss->ss8.base_addr = buf->bo->offset;

	ret = drm_intel_bo_emit_reloc(batch->bo,
				intel_batchbuffer_subdata_offset(batch, ss) + 8 * 4,
				buf->bo, 0, read_domain, write_domain);
	igt_assert(ret == 0);

	ss->ss2.height = igt_buf_height(buf) - 1;
	ss->ss2.width  = igt_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen8_spin_curbe_buffer_data(struct intel_batchbuffer *batch,
			    uint32_t iters)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	curbe_buffer = intel_batchbuffer_subdata_alloc(batch, 64, 64);
	offset = intel_batchbuffer_subdata_offset(batch, curbe_buffer);
	*curbe_buffer = iters;

	return offset;
}

static uint32_t
gen7_fill_binding_table(struct intel_batchbuffer *batch,
			const struct igt_buf *dst)
{
	uint32_t *binding_table, offset;

	binding_table = intel_batchbuffer_subdata_alloc(batch, 32, 64);
	offset = intel_batchbuffer_subdata_offset(batch, binding_table);
	if (IS_GEN7(batch->devid))
		binding_table[0] = gen7_fill_surface_state(batch, dst,
						SURFACEFORMAT_R8_UNORM, 1);
	else
		binding_table[0] = gen8_fill_surface_state(batch, dst,
						SURFACEFORMAT_R8_UNORM, 1);

	return offset;
}

static uint32_t
gen8_fill_interface_descriptor(struct intel_batchbuffer *batch,
			       const struct igt_buf *dst,
			       const uint32_t kernel[][4],
			       size_t size)
{
	struct gen8_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = gen7_fill_binding_table(batch, dst);
	kernel_offset = gen7_fill_kernel(batch, kernel, size);

	idd = intel_batchbuffer_subdata_alloc(batch, sizeof(*idd), 64);
	offset = intel_batchbuffer_subdata_offset(batch, idd);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.constant_urb_entry_read_offset = 0;
	idd->desc5.constant_urb_entry_read_length = 1; /* grf 1 */

	idd->desc6.num_threads_in_tg = 1;

	return offset;
}

static void
gen8_emit_state_base_address(struct intel_batchbuffer *batch)
{
	OUT_BATCH(GEN8_STATE_BASE_ADDRESS | (16 - 2));

	/* general */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);
	OUT_BATCH(0);

	/* stateless data port */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);

	/* surface */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_SAMPLER, 0, BASE_ADDRESS_MODIFY);

	/* dynamic */
	OUT_RELOC(batch->bo,
		  I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
		  0, BASE_ADDRESS_MODIFY);

	/* indirect */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* instruction */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_INSTRUCTION, 0,
		  BASE_ADDRESS_MODIFY);

	/* general state buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* dynamic state buffer size */
	OUT_BATCH(1 << 12 | 1);
	/* indirect object buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* instruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	OUT_BATCH(1 << 12 | 1);
}

static void
gen8_emit_vfe_state(struct intel_batchbuffer *batch, uint32_t threads,
		    uint32_t urb_entries, uint32_t urb_size,
		    uint32_t curbe_size)
{
	OUT_BATCH(GEN7_MEDIA_VFE_STATE | (9 - 2));

	/* scratch buffer */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* number of threads & urb entries */
	OUT_BATCH(threads << 16 |
		urb_entries << 8);

	OUT_BATCH(0);

	/* urb entry size & curbe size */
	OUT_BATCH(urb_size << 16 |
		curbe_size);

	/* scoreboard */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_curbe_load(struct intel_batchbuffer *batch, uint32_t curbe_buffer)
{
	OUT_BATCH(GEN7_MEDIA_CURBE_LOAD | (4 - 2));
	OUT_BATCH(0);
	/* curbe total data length */
	OUT_BATCH(64);
	/* curbe data start address, is relative to the dynamics base address */
	OUT_BATCH(curbe_buffer);
}

static void
gen7_emit_interface_descriptor_load(struct intel_batchbuffer *batch,
				    uint32_t interface_descriptor)
{
	OUT_BATCH(GEN7_MEDIA_INTERFACE_DESCRIPTOR_LOAD | (4 - 2));
	OUT_BATCH(0);
	/* interface descriptor data length */
	if (IS_GEN7(batch->devid))
		OUT_BATCH(sizeof(struct gen7_interface_descriptor_data));
	else
		OUT_BATCH(sizeof(struct gen8_interface_descriptor_data));
	/* interface descriptor address, is relative to the dynamics base
	 * address
	 */
	OUT_BATCH(interface_descriptor);
}

static void
gen8_emit_media_state_flush(struct intel_batchbuffer *batch)
{
	OUT_BATCH(GEN8_MEDIA_STATE_FLUSH | (2 - 2));
	OUT_BATCH(0);
}

static void
gen_emit_media_object(struct intel_batchbuffer *batch,
		      unsigned int xoff, unsigned int yoff)
{
	OUT_BATCH(GEN7_MEDIA_OBJECT | (8 - 2));

	/* interface descriptor offset */
	OUT_BATCH(0);

	/* without indirect data */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* scoreboard */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* inline data (xoffset, yoffset) */
	OUT_BATCH(xoff);
	OUT_BATCH(yoff);
	if (AT_LEAST_GEN(batch->devid, 8) && !IS_CHERRYVIEW(batch->devid))
		gen8_emit_media_state_flush(batch);
}

static void
gen9_emit_state_base_address(struct intel_batchbuffer *batch)
{
	OUT_BATCH(GEN8_STATE_BASE_ADDRESS | (19 - 2));

	/* general */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);
	OUT_BATCH(0);

	/* stateless data port */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);

	/* surface */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_SAMPLER, 0, BASE_ADDRESS_MODIFY);

	/* dynamic */
	OUT_RELOC(batch->bo,
		  I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
		  0, BASE_ADDRESS_MODIFY);

	/* indirect */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* instruction */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_INSTRUCTION, 0,
		  BASE_ADDRESS_MODIFY);

	/* general state buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* dynamic state buffer size */
	OUT_BATCH(1 << 12 | 1);
	/* indirect object buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* intruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	OUT_BATCH(1 << 12 | 1);

	/* Bindless surface state base address */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);
	OUT_BATCH(0);
	OUT_BATCH(0xfffff000);
}

void
gen8_media_spinfunc(struct intel_batchbuffer *batch,
		    const struct igt_buf *dst, uint32_t spins)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush_with_context(batch, NULL);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen8_spin_curbe_buffer_data(batch, spins);
	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
					      spin_kernel, sizeof(spin_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen8_emit_state_base_address(batch);

	gen8_emit_vfe_state(batch, THREADS, MEDIA_URB_ENTRIES,
			    MEDIA_URB_SIZE, MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen_emit_media_object(batch, xoffset, yoffset);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}


void
gen9_media_spinfunc(struct intel_batchbuffer *batch,
		    const struct igt_buf *dst, uint32_t spins)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush_with_context(batch, NULL);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen8_spin_curbe_buffer_data(batch, spins);
	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
					      spin_kernel, sizeof(spin_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		  GEN9_FORCE_MEDIA_AWAKE_ENABLE |
		  GEN9_SAMPLER_DOP_GATE_DISABLE |
		  GEN9_PIPELINE_SELECTION_MASK |
		  GEN9_SAMPLER_DOP_GATE_MASK |
		  GEN9_FORCE_MEDIA_AWAKE_MASK);
	gen9_emit_state_base_address(batch);

	gen8_emit_vfe_state(batch, THREADS, MEDIA_URB_ENTRIES,
			    MEDIA_URB_SIZE, MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen_emit_media_object(batch, xoffset, yoffset);

	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		  GEN9_FORCE_MEDIA_AWAKE_DISABLE |
		  GEN9_SAMPLER_DOP_GATE_ENABLE |
		  GEN9_PIPELINE_SELECTION_MASK |
		  GEN9_SAMPLER_DOP_GATE_MASK |
		  GEN9_FORCE_MEDIA_AWAKE_MASK);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}
