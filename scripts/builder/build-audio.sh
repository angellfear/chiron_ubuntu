#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the two audio codec drivers this board fits and common_drivers does not
# carry, as external modules against the kernel that was just built, so vermagic
# and Module.symvers match exactly.
#
#   es7210  Everest four-channel ADC, the amplifier hardware feedback on TDM-C
#   fs2105  FourSemi speaker amplifier, two of them on TDM-B
#
# Everything else in the audio path -- the AUGE fabric, the dummy codec -- comes
# from the kernel build: CONFIG_AMLOGIC_SND_SOC=m with AUGE and COMMON built in
# produces amlogic-snd-soc.ko. A kernel without it would have to rebuild that
# whole fabric out of tree as snd_soc_playback.ko; here it is an ordinary
# kernel module and there is nothing to reproduce.
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

out="$workspace/artifacts/audio"
build="$workspace/build-audio"
rm -rf "$build"
mkdir -p "$out" "$build"

# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
for driver in es7210 fs2105; do
    cp -r "$integration_root/drivers/audio/$driver" "$build/"
    make -C "$workspace/sources/linux" -j"$(nproc)" \
        ARCH=arm64 CROSS_COMPILE="$cross" M="$build/$driver" modules
done

for module in snd-soc-es7210.ko snd-soc-fs2105.ko; do
    found=$(find "$build" -name "$module" | head -n 1)
    test -n "$found" || { echo "the audio build produced no $module" >&2; exit 1; }
    cp "$found" "$out/$module"
done

rm -rf "$build"
echo "audio codec modules ready: $out"
ls -l "$out"
