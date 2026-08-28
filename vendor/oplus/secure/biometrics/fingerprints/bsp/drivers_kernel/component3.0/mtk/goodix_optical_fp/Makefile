ccflags-$(CONFIG_OPLUS_FINGERPRINT_GKI_ENABLE) += -DCONFIG_OPLUS_FINGERPRINT_GKI_ENABLE
ifeq ($(CONFIG_OPLUS_FINGERPRINT_QCOM),y)
ccflags-y += -DQCOM_PLATFORM
obj-y  += gf_spi.o gf_platform.o gf_netlink.o

else ifeq ($(CONFIG_OPLUS_FINGERPRINT_MTK),y)
# op6893: these were all $(srctree)/..., which points at the *common* kernel.
# Every one of these directories lives in the device-modules tree instead, so
# they resolve through DEVICE_MODULES_PATH.  With the old prefix the -I flags
# silently pointed at nothing and mtk_disp_notify.h was not found.
#
# $(srctree)/drivers/spi/trustzone/spi/Tlspi/inc is dropped: that path exists
# in no tree here, and nothing in the files we build includes from it.
subdir-ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include
subdir-ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/mt-plat
subdir-ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/spi/mediatek/$(MTK_PLATFORM)

#add for gki mtk lcd uiready
ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(DEVICE_MODULES_PATH)/drivers/gpu/drm/mediatek/mediatek_v2/
ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -DCONFIG_DRM_MEDIATEK_V2

# op6893: 6.6 is neither kernel-4.19 nor kernel-4.14/4.9, so neither arm of the
# original conditional fired and the Trustonic include path went missing.  The
# files built here talk to the TA through the userspace MobiCore client, not
# through in-kernel MobiCore headers, so the include is only added when the
# version really is one of the old ones -- but keep the 6.6 path pointed at the
# right tree for when a MobiCore header does get pulled in.
ifneq ($(filter kernel-4.19, $(LINUX_KERNEL_VERSION)),)
include $(DEVICE_MODULES_PATH)/drivers/tee/gud/Makefile.include
else ifneq ($(filter kernel-4.14 kernel-4.9, $(LINUX_KERNEL_VERSION)),)
include $(DEVICE_MODULES_PATH)/drivers/misc/mediatek/gud/Makefile.include
endif

ccflags-y += -DMTK_PLATFORM

gf_tee-y := gf_spi_tee.o gf_platform_mtk.o gf_netlink.o ../oplus_fp_common/fingerprint_event.o
obj-m += gf_tee.o
endif
