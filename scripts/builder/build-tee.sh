#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the normal-world OP-TEE userspace: libteec, tee-supplicant and the
# Chiron client that talks to the secure world.
#
# Android's tee-supplicant and libteec are bionic-linked and cannot run here,
# so the same protocol is built against glibc from the pinned optee_client.
#
# RPMB_EMU defaults to 1 upstream, which makes tee-supplicant emulate an RPMB
# partition in memory and never open /dev/mmcblk0rpmb. The secure world then
# finds no TA version table and refuses to load any TA with "RPMB inaccessible".
# This board has real RPMB, so the emulation is turned off -- and it is baked
# in at compile time, so the build directory is always fresh.
#
# libteeacl is not built: it needs an aarch64 libuuid this image does not carry
# and nothing here uses it.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -d "$workspace/sources/optee_client" || {
    echo "usage: $0 /root/ubuntu26_temp/<run>" >&2
    exit 2
}
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

# The toolchain this userspace is built with. Deliberately not the
# Fenix kernel toolchain: these are glibc binaries for the target rootfs,
# not kernel objects.
cross=aarch64-linux-gnu-
command -v "${cross}gcc" >/dev/null || {
    echo "the ${cross}gcc cross toolchain is not installed" >&2
    exit 1
}

src="$workspace/sources/optee_client"
out="$workspace/artifacts/tee"
build="$workspace/build-tee"
rm -rf "$build"
mkdir -p "$out" "$build"

make -j"$(nproc)" -C "$src" CROSS_COMPILE="$cross" O="$build" RPMB_EMU=0 \
    build-libteec build-tee-supplicant

# The assumptions the RPMB and shared-memory choices rest on, checked against
# the kernel this is being built for rather than assumed: the Amlogic OP-TEE
# backend advertises TEE_GEN_CAP_GP alone, which keeps libteec on the
# driver-allocated shared memory path the vendor client uses, and the UAPI is
# the standard one.
smc_backend="$workspace/sources/common_drivers/drivers/aml_tee/optee/smc_abi.c"
uapi_header="$workspace/sources/linux/include/uapi/linux/tee.h"
grep -q 'gen_caps = TEE_GEN_CAP_GP,' "$smc_backend" || {
    echo "the OP-TEE backend no longer advertises TEE_GEN_CAP_GP alone" >&2
    exit 1
}
grep -q 'TEE_IOC_OPEN_SESSION' "$uapi_header" || {
    echo "the TEE UAPI header is not the expected one" >&2
    exit 1
}

cp "$build/tee-supplicant/tee-supplicant" "$out/"
cp "$build/libteec/libteec.so.2.0.0" "$out/"

# The Chiron client. chiron_preload_fw links against the libteec just built
# and finds it at /usr/local/lib on the board.
"${cross}gcc" -O2 -Wall -Wextra -o "$out/chiron_preload_fw" \
    "$integration_root/src/chiron-tee/chiron_preload_fw.c" \
    -I"$src/libteec/include" \
    -L"$build/libteec" -l:libteec.so.2.0.0 \
    -Wl,-rpath,/usr/local/lib

rm -rf "$build"
echo "TEE userspace ready: $out"
ls -l "$out"
