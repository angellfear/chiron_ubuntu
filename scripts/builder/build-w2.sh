#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the Amlogic W2 Wi-Fi modules as external modules against the kernel
# that was just built, so vermagic and Module.symvers match exactly.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -d "$workspace/sources/w2/aml_drv" || {
    echo "usage: $0 /root/ubuntu26_temp/<run>" >&2
    exit 2
}
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

toolchain=$(ls -d "$workspace"/sources/fenix/build/toolchains/gcc-arm-aarch64-none-linux-gnu*/bin 2>/dev/null | head -n 1)
test -n "$toolchain" || { echo "no Fenix aarch64 toolchain in the workspace" >&2; exit 1; }

out="$workspace/artifacts/w2"
mkdir -p "$out"

# The vendor tree ships these without the executable bit in some snapshots.
chmod 0755 "$workspace/sources/w2/aml_drv/mkvers.sh" \
    "$workspace/sources/w2/aml_drv/mklink.sh" 2>/dev/null || true

make -C "$workspace/sources/linux" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE="$toolchain/aarch64-none-linux-gnu-" \
    M="$workspace/sources/w2/aml_drv" CONFIG_AML_FULLMAC=m modules

for module in w2.ko w2_comm.ko; do
    found=$(find "$workspace/sources/w2/aml_drv" -name "$module" | head -n 1)
    test -n "$found" || { echo "W2 build produced no $module" >&2; exit 1; }
    cp "$found" "$out/$module"
done

# Firmware and calibration come from the same locked source tree as the
# modules: a mismatched pair fails at firmware load, not at insmod. This is the
# one vendor firmware the image carries. The board's own factory copy is a
# different build that this driver drops under load, and Wi-Fi is what any
# later download would need -- see THIRD_PARTY.md.
for blob in wifi_w2_fw_sdio.bin aml_wifi_rf.txt; do
    cp "$workspace/sources/w2/common/$blob" "$out/$blob"
done

echo "W2 modules and firmware ready: $out"
ls -l "$out"
