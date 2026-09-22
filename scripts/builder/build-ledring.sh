#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the AW21036 LED-ring driver as an external module against the kernel
# that was just built, so vermagic and Module.symvers match exactly.
#
# Four controllers on i2c3 drive 48 RGB pixels. The driver needs nothing but
# the LED class and I2C, both built into the kernel.
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

out="$workspace/artifacts/ledring"
build="$workspace/build-ledring"
rm -rf "$build"
mkdir -p "$out" "$build"

# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
cp -r "$integration_root/drivers/led/aw21036" "$build/"
make -C "$workspace/sources/linux" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE="$cross" M="$build/aw21036" modules

found=$(find "$build" -name aw21036.ko | head -n 1)
test -n "$found" || { echo "the LED ring build produced no aw21036.ko" >&2; exit 1; }
cp "$found" "$out/aw21036.ko"

rm -rf "$build"
echo "LED ring module ready: $out"
ls -l "$out"
