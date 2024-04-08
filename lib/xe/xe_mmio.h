/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#include "lib/intel_io.h"
#include "lib/igt_sizes.h"

#ifndef XE_MMIO_H
#define XE_MMIO_H

#define TILE_MMIO_SIZE		SZ_16M
#define GGTT_OFFSET_IN_TILE	SZ_8M

typedef uint64_t xe_ggtt_pte_t;

struct xe_mmio {
	int fd;
	struct intel_mmio_data intel_mmio;
};

void xe_mmio_vf_access_init(int pf_fd, int vf_id, struct xe_mmio *mmio);
void xe_mmio_access_init(int pf_fd, struct xe_mmio *mmio);
void xe_mmio_access_fini(struct xe_mmio *mmio);

uint32_t xe_mmio_read32(struct xe_mmio *mmio, uint32_t offset);
uint64_t xe_mmio_read64(struct xe_mmio *mmio, uint32_t offset);

void xe_mmio_write32(struct xe_mmio *mmio, uint32_t offset, uint32_t val);
void xe_mmio_write64(struct xe_mmio *mmio, uint32_t offset, uint64_t val);

uint32_t xe_mmio_gt_read32(struct xe_mmio *mmio, int gt, uint32_t offset);
uint64_t xe_mmio_gt_read64(struct xe_mmio *mmio, int gt, uint32_t offset);

void xe_mmio_gt_write32(struct xe_mmio *mmio, int gt, uint32_t offset, uint32_t val);
void xe_mmio_gt_write64(struct xe_mmio *mmio, int gt, uint32_t offset, uint64_t val);

xe_ggtt_pte_t xe_mmio_ggtt_read(struct xe_mmio *mmio, int gt, uint32_t pte_offset);
void xe_mmio_ggtt_write(struct xe_mmio *mmio, int gt, uint32_t pte_offset, xe_ggtt_pte_t pte);

#endif	/* XE_MMIO_H */
