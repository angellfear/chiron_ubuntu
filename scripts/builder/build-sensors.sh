#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the two sensor drivers this board fits and the kernel does not carry,
# as external modules against the kernel that was just built, so vermagic and
# Module.symvers match exactly.
#
#   sc7a20e  Silan 3-axis accelerometer on i2c1, the panel rotation input
#   stk3b6x  Sensortek ambient light sensor on i2c4
#
# Both need only what is already built into the kernel -- the IIO core for one,
# the input core for the other -- so unlike a build against a prebuilt Image,
# which would have to prove the CRCs of every symbol it used, this is an
# ordinary out-of-tree build.
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

out="$workspace/artifacts/sensors"
build="$workspace/build-sensors"
rm -rf "$build"
mkdir -p "$out" "$build"

# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
for driver in sc7a20e stk3b6x; do
    cp -r "$integration_root/drivers/sensor/$driver" "$build/"
    make -C "$workspace/sources/linux" -j"$(nproc)" \
        ARCH=arm64 CROSS_COMPILE="$cross" M="$build/$driver" modules
done

for module in sc7a20e.ko stk3b6x.ko; do
    found=$(find "$build" -name "$module" | head -n 1)
    test -n "$found" || { echo "the sensor build produced no $module" >&2; exit 1; }
    cp "$found" "$out/$module"
done

rm -rf "$build"
echo "sensor modules ready: $out"
ls -l "$out"
