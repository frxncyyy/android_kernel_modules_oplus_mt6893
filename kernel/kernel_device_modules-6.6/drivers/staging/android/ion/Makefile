# SPDX-License-Identifier: GPL-2.0
#
# MediaTek ION, ported from the 4.19 tree (drivers/staging/android/aosp_ion).
# Built as one module so /dev/ion exists for the vendor blobs that speak the ION
# ioctl ABI (vpud, libcodec2_vndk, libdpframework, libion_mtk).
#
# The module is mtk_ion.ko, not ion.ko: kbuild would otherwise have the composite
# module ion.o list ion.c's own ion.o among its components, and the link step
# tries to link ion.o into itself ("cannot open .../ion.o").

obj-$(CONFIG_ION) += mtk_ion.o
mtk_ion-y := ion.o ion-ioctl.o ion_heap.o
mtk_ion-$(CONFIG_ION_SYSTEM_HEAP)   += ion_system_heap.o ion_page_pool.o
mtk_ion-$(CONFIG_ION_CARVEOUT_HEAP) += ion_carveout_heap.o
mtk_ion-$(CONFIG_ION_CHUNK_HEAP)    += ion_chunk_heap.o
mtk_ion-$(CONFIG_ION_CMA_HEAP)      += ion_cma_heap.o
mtk_ion-$(CONFIG_MTK_ION_SEC_HEAP)  += mtk_ion_secure_heap.o

ccflags-y += -I$(src)
