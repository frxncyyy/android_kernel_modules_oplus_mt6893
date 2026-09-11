// SPDX-License-Identifier: GPL-2.0
/*
 * SVP symbol stubs for the op6893 6.6 bring-up.
 *
 * The vendor mtk-vcodec dec/enc drivers reference two SVP (secure video
 * path) helpers, is_disable_map_sec() and dmabuf_to_secure_handle(),
 * whenever CONFIG_DEVICE_MODULES_ARM_SMMU_V3 is off.  On a full vendor
 * build those come from iommu_gz / mtk_sec_heap (trusted-memory
 * subsystem).  This board runs neither SMMU-v3 nor the trusted-memory
 * stack, so provide build-time stand-ins instead of pulling the whole
 * TEE dependency chain in.
 *
 * is_disable_map_sec() returning false short-circuits every SVP branch
 * in mtk_vcodec_dec.c/mtk_vcodec_enc.c (they all gate on it, usually
 * together with ctx->*->svp_mode), so dmabuf_to_secure_handle() is
 * never reached for normal (non-secure) playback -- the stub just
 * fails loudly if that ever changes.
 */

#include <linux/module.h>
#include <linux/dma-buf.h>
#include <linux/types.h>

bool is_disable_map_sec(void)
{
	return false;
}
EXPORT_SYMBOL_GPL(is_disable_map_sec);

u64 dmabuf_to_secure_handle(const struct dma_buf *dmabuf)
{
	pr_err("%s: secure video path is not supported on this build\n",
	       __func__);
	return 0;
}
EXPORT_SYMBOL_GPL(dmabuf_to_secure_handle);

/*
 * v2/mtk_vcodec_{dec,enc}_pm_plat.c call this under CONFIG_MTK_TASK_TURBO
 * (cpu_hint DVFS assist).  task_turbo.ko itself drags in the whole EAS
 * scheduler extension stack, which is not part of this bring-up; a noop
 * keeps the codec path functional without it.
 */
int enforce_ct_to_vip(int val, int caller_id)
{
	return 0;
}
EXPORT_SYMBOL_GPL(enforce_ct_to_vip);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("mtk-vcodec SVP/task-turbo symbol stubs (op6893 bring-up)");
