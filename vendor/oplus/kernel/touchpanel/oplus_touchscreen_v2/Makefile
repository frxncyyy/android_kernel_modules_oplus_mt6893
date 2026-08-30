#
# Makefile for the touchscreen drivers.
#
# op6893: upstream builds every module in this directory with kleaf, which puts the
# module root on the include path (`includes = ["."]` in oplus_local_modules.bzl).
# A plain kbuild M= build does not, so subdirectories could not find the headers
# that live here (touchpanel_common.h, tp_devices.h, ...).  One subdir-wide -I
# covers every subdirectory the way `includes = ["."]` does.  It has to be
# $(srctree)/$(src): in 6.6 $(src) is $(obj), i.e. relative to the *build* dir,
# and with O= pointing outside the source tree that resolves into the output
# mirror (which holds no headers).
subdir-ccflags-y += -I$(srctree)/$(src)

# op6893: these CONFIG_* have no Kconfig reachable from Kconfig.ext in this tree,
# so they only ever exist as make command-line variables and never reach
# autoconf.h.  The C code tests them with #ifdef / IS_ENABLED() / IS_MODULE(), so
# turn each one into the define Kconfig would have generated:
#   CONFIG_X=y -> -DCONFIG_X=1        (IS_BUILTIN / #ifdef)
#   CONFIG_X=m -> -DCONFIG_X_MODULE=1 (IS_MODULE, and IS_ENABLED via it)
# Same idea as ccflags-$(CONFIG_...) += -DCONFIG_... in the fingerprint Makefile,
# generalised so the =m case gets the _MODULE spelling the code actually tests.
TP_MAKE_CONFIGS := \
	TOUCHPANEL_MTK_PLATFORM \
	TOUCHPANEL_LATE_INIT \
	TOUCHPANEL_OPLUS \
	TOUCHPANEL_NOTIFY \
	TOUCHPANEL_CUSTOM \
	TOUCHPANEL_TRUSTED_TOUCH \
	TOUCHIRQ_UPDATE_QOS \
	TOUCHPANEL_NEW_SET_IRQ_WAKE \
	TOUCHPANEL_MULTI_NOFLASH \
	TOUCHPANEL_FAULT_INJECT_ENABLE \
	OPLUS_MTK_DRM_GKI_NOTIFY \
	OPLUS_TP_APK \
	REMOVE_OPLUS_FUNCTION \
	TOUCHPANEL_SYNAPTICS \
	TOUCHPANEL_FOCAL \
	TOUCHPANEL_GOODIX \
	TOUCHPANEL_NOVA \
	TOUCHPANEL_HIMAX \
	TOUCHPANEL_SAMSUNG \
	TOUCHPANEL_ILITEK \
	TOUCHPANEL_JADARD \
	TOUCHPANEL_ST \
	TOUCHPANEL_FOCAL_FT3518 \
	TOUCHPANEL_FOCAL_FT3658U \
	TOUCHPANEL_FOCAL_FT3658U_SPI \
	TOUCHPANEL_FOCAL_FT3681 \
	TOUCHPANEL_FOCAL_FT3683G \
	TOUCHPANEL_FOCAL_FT8057P

subdir-ccflags-y += $(foreach c,$(TP_MAKE_CONFIGS),\
	$(if $(filter y,$(CONFIG_$(c))),-DCONFIG_$(c)=1,\
	$(if $(filter m,$(CONFIG_$(c))),-DCONFIG_$(c)_MODULE=1,)))

# op6893: was $(srctree)/drivers/misc/mediatek/include/, but $(srctree) is the
# *common* GKI kernel here and has no such directory; the MediaTek headers live
# in the device-modules tree, reachable through DEVICE_MODULES_PATH.  Fixed the
# same way in the fingerprint Makefile.
subdir-ccflags-$(CONFIG_TOUCHPANEL_MTK_PLATFORM) += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/
subdir-ccflags-$(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY) += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/

ifeq ($(OPLUS_FEATURE_BSP_DRV_INJECT_TEST),1)
ccflags-y += -DCONFIG_TOUCHPANEL_FAULT_INJECT_ENABLE
$(info OPLUS_FEATURE_BSP_DRV_INJECT_TEST and CONFIG_TOUCHPANEL_FAULT_INJECT_ENABLE is Y)
else ifeq ($(OPLUS_FEATURE_BSP_DRV_VND_INJECT_TEST),1)
ccflags-y += -DCONFIG_TOUCHPANEL_FAULT_INJECT_ENABLE
$(info OPLUS_FEATURE_BSP_DRV_VND_INJECT_TEST and CONFIG_TOUCHPANEL_FAULT_INJECT_ENABLE is Y)
else
$(info CONFIG_TOUCHPANEL_FAULT_INJECT_ENABLE is N)
endif

oplus_bsp_tp_comon-y := util_interface/touch_interfaces.o
oplus_bsp_tp_comon-y += touch_comon_api/touch_comon_api.o
oplus_bsp_tp_comon-y += touchpanel_healthinfo/touchpanel_healthinfo.o
oplus_bsp_tp_comon-y += touchpanel_healthinfo/touchpanel_exception.o
oplus_bsp_tp_comon-y += touchpanel_autotest/touchpanel_autotest.o
oplus_bsp_tp_comon-y += touchpanel_prevention/touchpanel_prevention.o

oplus_bsp_tp_comon-y += touchpanel_common_driver.o
oplus_bsp_tp_comon-y += touchpanel_proc.o
oplus_bsp_tp_comon-y += tp_ioctl.o
oplus_bsp_tp_comon-y += message_list.o
oplus_bsp_tp_comon-y += touch_pen/touch_pen_core.o
oplus_bsp_tp_comon-y += touch_pen/touch_pen_algo.o

obj-$(CONFIG_TOUCHPANEL_OPLUS) += oplus_bsp_tp_comon.o

oplus_bsp_tp_notify-y += touchpanel_notify/touchpanel_event_notify.o
oplus_bsp_tp_comon-y += touchpanel_tui_support/touchpanel_tui_support.o
obj-$(CONFIG_TOUCHPANEL_NOTIFY) += oplus_bsp_tp_notify.o

# op6893: touch_custom/ has its own Makefile and Kconfig (CONFIG_TOUCHPANEL_CUSTOM)
# but no parent ever descended into it, so oplus_bsp_tp_custom was never built and
# modpost could not resolve tp_judge_ic_match_commandline / tp_util_get_vendor /
# preconfig_power_control / reconfig_power_control /
# display_esd_check_enable_bytouchpanel / request_firmware_select, which
# touchpanel_common_driver.c calls.  Upstream builds it as its own ko
# (oplus_bsp_tp_custom in oplus_local_modules.bzl, a ko_dep of every tp module).
obj-$(CONFIG_TOUCHPANEL_CUSTOM) += touch_custom/

obj-$(CONFIG_TOUCHPANEL_SYNAPTICS) += Synaptics/
obj-$(CONFIG_TOUCHPANEL_FOCAL) += Focal/
obj-$(CONFIG_TOUCHPANEL_GOODIX) += Goodix/
obj-$(CONFIG_TOUCHPANEL_NOVA) += Novatek/
obj-$(CONFIG_TOUCHPANEL_HIMAX) += Himax/
obj-$(CONFIG_TOUCHPANEL_SAMSUNG) += Samsung/
obj-$(CONFIG_TOUCHPANEL_ILITEK) += ilitek/
obj-$(CONFIG_TOUCHPANEL_JADARD) += Jadard/
obj-$(CONFIG_TOUCHPANEL_ST) += St/
