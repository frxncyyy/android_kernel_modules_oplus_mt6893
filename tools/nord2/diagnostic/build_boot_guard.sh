#!/bin/sh
# Build the diagnostic PID 1 launcher as a static arm64 binary.
#
# KERNEL must point at the Linux 6.6 tree that provides tools/include/nolibc
# and the generated UAPI headers; run `make headers_install` there first.
# The installed Android init stays PID 1 - this binary only launches it.
set -eu
KERNEL=${KERNEL:-../kernel-6.6}
CC=${CC:-clang}
OUT=${1:-android_boot_guard}
exec "$CC" --target=aarch64-linux-gnu -fuse-ld=lld -static -nostdlib -Oz \
	-fno-ident -Wl,--build-id=none \
	-I "$KERNEL/tools/include/nolibc" -I "$KERNEL/usr/include" \
	-o "$OUT" "$(dirname "$0")/android_boot_guard.c"
