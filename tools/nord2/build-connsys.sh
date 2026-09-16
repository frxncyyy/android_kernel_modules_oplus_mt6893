#!/usr/bin/env bash
# Build the MediaTek connectivity stack (connsys) for the 6.6 port.
#
# Every module here is its own Kbuild root, exactly like the touch and
# tri-state roots.  The order matters: each module links against the
# device-modules symbol table plus the symbol tables of the connectivity
# modules built before it, so the script accumulates KBUILD_EXTRA_SYMBOLS as it
# goes.  Notes that cost a lot to rediscover:
#
#   * TOP must be this repository's root.  Many of these Kbuilds compute
#     TOP := $(srctree)/.. (which is <repo>/..) and then fail to find
#     $(TOP)/vendor/mediatek/... (gen4m's conninfra headers, btif, connfem).
#   * The kernel picks Kbuild over Makefile in an M= directory, so the Android
#     and Kleaf wrapper Makefiles next to these roots are inert; drive the
#     roots directly instead of the build/connac2x/6893 wrappers, which expect
#     an $(O)/../vendor/... output layout that does not exist here.
#   * gen4m dispatches on SEGMENT/MODULE_NAME: SEGMENT=SP plus
#     MODULE_NAME=wlan_drv_gen4m_6893 selects Kbuild.6893, which then sets
#     WLAN_CHIP_ID/MTK_COMBO_CHIP/CONNAC_VER/HIF itself -- do not pass those.
#   * MDDP and CCCI are switched off for gen4m because this port ships no modem
#     stack; they are the only reason it would need mddp.ko and the
#     eccci/ccci_md_all modem modules at load time.
#   * wlan/adaptor is built with CONNAC_VER=2_0 so it produces the stock
#     wmt_chrdev_wifi.ko name from connac2x code; the tree's own build/connac2x
#     rule would name it wmt_chrdev_wifi_connac2, and gen4m's depends list
#     expects the plain name.
#   * The .ko files land back in the source tree (no separate output dir): the
#     M= path is relative and O= is the same depth from the repository root as
#     the kernel checkout.
#
# Usage: tools/nord2/build-connsys.sh [--kernel DIR] [--out DIR]
# Defaults match the workspace layout: this repository is at work/src/modules,
# the kernel at work/src/kernel-6.6 and the build output at work/build/baseline.
# A build environment file (work/build-env.sh) is sourced when present so the
# llvm-* helpers are on PATH.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
kernel="$(cd "${KERNEL:-$repo/../kernel-6.6}" && pwd)"
out="$(cd "${OUT:-$repo/../../build/baseline}" && pwd)"
[ -f "$repo/../../build-env.sh" ] && . "$repo/../../build-env.sh"

dev="$repo/kernel/kernel_device_modules-6.6"
mods="$repo/vendor/mediatek/kernel_modules/connectivity"
module_rel=../../src/modules/kernel/kernel_device_modules-6.6
syms="$dev/Module.symvers"

build() { # <relative path under connectivity/> <symvers subdir> [make vars...]
  local rel=$1 symdir=$2
  shift 2
  printf '=== connsys: %s\n' "$rel"
  make -C "$kernel" O="$out" ARCH=arm64 LLVM=1 \
    HOSTCFLAGS="$HOSTCFLAGS" HOSTLDFLAGS="$HOSTLDFLAGS" \
    KCONFIG_EXT_PREFIX="$module_rel/" DEVICE_MODULES_REL_DIR="$module_rel" \
    DEVICE_MODULES_PATH="$dev" \
    DEVCIE_MODULES_INCLUDE="-I$dev/include -I$dev/include/uapi" \
    TOP="$repo" \
    M="$mods/$rel" \
    KBUILD_EXTRA_SYMBOLS="$syms" "$@" -j"$(nproc)" modules
  syms="$syms $mods/$symdir/Module.symvers"
}

build conninfra                     conninfra
build connfem                       connfem
build wlan/adaptor                  wlan/adaptor \
  MODULE_NAME=wmt_chrdev_wifi CONNAC_VER=2_0
build wlan/core/gen4m               wlan/core/gen4m \
  MODULE_NAME=wlan_drv_gen4m_6893 SEGMENT=SP \
  CONFIG_MTK_WIFI_CCCI_SUPPORT=n CONFIG_MTK_WIFI_MDDP_SUPPORT=n
build bt/mt66xx/6893                bt/mt66xx/6893
