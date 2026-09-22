#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the ADLA NPU kernel driver.
#
# The source is in common_drivers (`drivers/adla/kmd`) but has no Kconfig
# symbol: its Makefile is written to be invoked with M=, so it is built as an
# external module against the kernel that was just built, like the others.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -d "$workspace/sources/linux" || {
    echo "usage: $0 /root/ubuntu26_temp/<run>" >&2
    exit 2
}
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

toolchain=$(ls -d "$workspace"/sources/fenix/build/toolchains/gcc-arm-aarch64-none-linux-gnu*/bin 2>/dev/null | head -n 1)
test -n "$toolchain" || { echo "no Fenix aarch64 toolchain in the workspace" >&2; exit 1; }
cross="$toolchain/aarch64-none-linux-gnu-"

src="$workspace/sources/common_drivers/drivers/adla/kmd"
test -f "$src/Makefile" || { echo "no ADLA KMD source at $src" >&2; exit 1; }

out="$workspace/artifacts/npu"
mkdir -p "$out"

make -C "$workspace/sources/linux" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE="$cross" M="$src" modules

test -f "$src/adla_core.ko" || { echo "the NPU build produced no adla_core.ko" >&2; exit 1; }
cp "$src/adla_core.ko" "$out/adla_core.ko"

# The build happens inside the source tree here, unlike the drivers copied out
# of the integration checkout: this one is part of common_drivers and the tree
# is the workspace's own. Leave it clean so a later kernel target does not trip
# over these objects.
make -C "$workspace/sources/linux" ARCH=arm64 CROSS_COMPILE="$cross" M="$src" clean

echo "NPU module ready: $out"
ls -l "$out"
