/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __GED_GE_H__
#define __GED_GE_H__

#include <linux/types.h>
#include <ged_type.h>

/* Must be the same as item number in region_sizes[], which in
 * /vendor/mediatek/proprietary/hardware/gralloc_extra/ge_misc.cpp
 * If the video fg function fails, please check whether the value matches first.
 * error case: ged_ge_get: vendor don't support region id: 2x
 * Modify the value to match the file(ge_misc.cpp) structure size to support dimming extension
 */
/* op6893 6.6 bring-up: doing exactly what the note above asks.  This device
 * keeps its stock vendor partition, whose gralloc_extra was built against the
 * 4.19 kernel where this is 19 (android_kernel_oplus_mt6893
 * drivers/gpu/mediatek/ged/src/ged_ge.h:15).  With 23 here, GE_ALLOC sends
 * 4 + 4*19 = 80 bytes while ged_dispatch() demands >= 4 + 4*23 = 96 and
 * rejects every call ("Failed to region_num, it must be 23"), so ge_alloc and
 * ge_free fail, the Mali gralloc returns NO_RESOURCES, and SurfaceFlinger
 * aborts on "output buffer not gpu writeable".  Relaxing only the size check
 * would not help: ged_ge_alloc() overrides region_num with this constant and
 * then validates region_sizes[0..22], reading past what userspace sent.
 * Raise this back to 23 only together with a matching vendor gralloc_extra.
 */
#define GE_ALLOC_STRUCT_NUM 19
#define GE_MAX_REGION_SIZE 8192

GED_ERROR ged_ge_init(void);
int ged_ge_exit(void);
int ged_ge_alloc(int region_num, uint32_t *region_sizes);
int ged_ge_get(int ge_fd, int region_id, int u32_offset,
	int u32_size, uint32_t *output_data);
int ged_ge_set(int ge_fd, int region_id, int u32_offset,
	int u32_size, uint32_t *input_data);
int ged_dmabuf_set_name(int32_t share_fd, char *name);

#endif
