#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the Himax touch module as an external module against the kernel that
# was just built, so vermagic and Module.symvers match exactly.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -f "$workspace/sources/himax/himax_common.c" || {
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

src="$workspace/sources/himax"
out="$workspace/artifacts/himax"
mkdir -p "$out"

# The tree carries objects from earlier builds and make has judged them current
# against newer sources before, producing a module silently missing the change
# that prompted the rebuild. Start from nothing.
make -C "$workspace/sources/linux" \
    ARCH=arm64 CROSS_COMPILE="$cross" M="$src" clean

# These three are not in this kernel's Kconfig, so nothing sets them: the vendor
# Kbuild gates `obj-m += himax_mmi.o` on COMMON=m and picks the in-cell HX83102
# sources off the other two. Without them the build succeeds and produces
# nothing at all -- no objects, no error. They have to be make variables, not
# KCPPFLAGS: the Kbuild tests them with $(filter ...) and derives the matching
# -D flags itself.
make -C "$workspace/sources/linux" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE="$cross" M="$src" \
    CONFIG_TOUCHSCREEN_HIMAX_COMMON=m \
    CONFIG_TOUCHSCREEN_HIMAX_INCELL=m \
    CONFIG_TOUCHSCREEN_HIMAX_IC_HX83102=m \
    modules

found=$(find "$src" -name himax_mmi.ko | head -n 1)
test -n "$found" || { echo "the Himax build produced no himax_mmi.ko" >&2; exit 1; }
cp "$found" "$out/himax_mmi.ko"

# The axis parameters are the whole point of rebuilding this module, and a stale
# object once produced one without invert_y that looked like a clean build. Each
# has to be present by name.
"${cross}objdump" -h "$out/himax_mmi.ko" >/dev/null
modinfo "$out/himax_mmi.ko" >"$out/himax_mmi.modinfo"
for required in swap_xy invert_x invert_y; do
    grep -q "^parm:[[:space:]]*${required}:" "$out/himax_mmi.modinfo" || {
        echo "himax_mmi.ko is missing the $required parameter" >&2
        exit 1
    }
done

echo "Himax touch module ready: $out"
grep -E '^(vermagic|depends|parm):' "$out/himax_mmi.modinfo"
ls -l "$out"
