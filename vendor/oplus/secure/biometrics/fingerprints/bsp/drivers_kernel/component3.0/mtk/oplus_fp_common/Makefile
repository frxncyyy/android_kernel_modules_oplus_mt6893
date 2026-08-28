obj-$(CONFIG_OPLUS_FINGERPRINT_COMMON) += oplus_fp_common.o
ccflags-$(CONFIG_OPLUS_FINGERPRINT_GKI_ENABLE) += -DCONFIG_OPLUS_FINGERPRINT_GKI_ENABLE

$(info [fp_bsp]LINUX_KERNEL_VERSION:$(LINUX_KERNEL_VERSION))

ifeq ($(CONFIG_OPLUS_FINGERPRINT_USE_PROC_OPS),y)
ccflags-y += -DUSE_PROC_OPS
endif

### Platform marco
ifeq ($(CONFIG_OPLUS_FINGERPRINT_QCOM),y)
ccflags-y += -DQCOM_PLATFORM
else ifeq ($(CONFIG_OPLUS_FINGERPRINT_MTK),y)
ccflags-y += -DMTK_PLATFORM
# op6893: back under its Kconfig gate, as bsp/drivers/ still has it.  The macro
# calls set_tee_worker_threads_on_big_core(), which the Trustonic driver
# defines but never EXPORT_SYMBOLs, so an unconditional -D leaves the module
# with an unresolved symbol.  4.19 shipped with the feature off
# (CONFIG_OPLUS_FINGERPRINT_ENABLE_TEE_BINDERCORE is absent from
# mt6893_defconfig), so off is also what this device was validated with.
ccflags-$(CONFIG_OPLUS_FINGERPRINT_ENABLE_TEE_BINDERCORE) += -DFP_TEE_BINDE_CORE_ENABLE
ifeq ($(CONFIG_OPLUS_FINGERPRINT_GKI_ENABLE),y)
# tp for gki.
#
# op6893: was $(srctree)/../vendor/..., which only resolves when the modules
# repo is unpacked next to the common kernel under a shared parent.  Here the
# two are siblings, so go through DEVICE_MODULES_PATH -- the one variable the
# build already points at kernel_device_modules-6.6 -- and back out of it.
#
# The header is taken from touchpanel_notify/ rather than
# oplus_touchscreen_v2/touchpanel_notify/: only the former has a Makefile of
# its own (oplus_bsp_tp_notify.ko), so it is the copy whose
# touchpanel_event_register_notifier() this module actually links against.
# The two declare an identical struct touchpanel_event; they differ in
# indentation and two extra EVENT_ACTION_* defines.
ccflags-y += -I$(DEVICE_MODULES_PATH)/../../vendor/oplus/kernel/touchpanel/touchpanel_notify
endif
endif
