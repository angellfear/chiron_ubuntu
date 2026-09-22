#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build the OV13B10 camera path: the loopback module, the calibration adapter
# and the two helpers against the ISP runtime of the pinned package.
#
#   v4l2loopback.ko          the public node the bridge publishes into. Built
#                            against the kernel that was just built -- vermagic
#                            and Module.symvers have to match or it will not
#                            load. Carries our capture_active attribute, which
#                            is what lets the bridge idle.
#   libchiron-ov13b10.so     the Chiron sensor callbacks and the calibration
#                            remap, compiled against the ispaml 0.8 headers.
#   chiron-camera-bridge-bin copies NV12 frames from the private ISP node to
#                            the loopback node while something is capturing.
#   chiron-camera-privacy    watches SW_CAMERA_LENS_COVER and takes access to
#                            the public node away when the shutter closes.
#
# The ISP runtime -- ispService and the four vendor libraries -- has no source
# and is taken from the pinned ispaml 0.8-202512~1.5 package that
# prepare-workspace.sh has already fetched and checksummed. It is staged into
# artifacts here rather than carried in the tree so that everything in the
# image is traceable to a pinned input.
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

isp_root="$workspace/sources/ispaml-0.8"
test -f "$isp_root/usr/include/aml_isp_adapt.h" || {
    echo "no ispaml 0.8 tree in $isp_root; run prepare-workspace.sh first" >&2
    exit 1
}

out="$workspace/artifacts/camera"
build="$workspace/build-camera"
rm -rf "$build"
mkdir -p "$out" "$build"

# --- the loopback module -------------------------------------------------
#
# Pinned at 0.12.7, the accepted release. The node number and
# card label are not cosmetic: udev matches ChironCamera by name and the bridge
# opens /dev/video35 by number.
cp -r "$workspace/sources/v4l2loopback" "$build/v4l2loopback"
make -C "$workspace/sources/linux" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE="$cross" M="$build/v4l2loopback" modules

found=$(find "$build/v4l2loopback" -name v4l2loopback.ko | head -n 1)
test -n "$found" || { echo "the v4l2loopback build produced no module" >&2; exit 1; }
cp "$found" "$out/v4l2loopback.ko"

# --- the ISP runtime -----------------------------------------------------
#
# Linked against below, never shipped. ispService and its four libraries are
# the proprietary ISP runtime: the image carries none of it, and
# chiron-vendor-fetch installs it on the board from this same pinned package.

# --- the calibration adapter and the two helpers -------------------------
#
# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
#
# The adapter feeds the factory Android SDR calibration to this ISP, whose
# calibration enum gained four core and three optional slots since the tables
# were captured; the index remap is the whole point of the file and was
# verified against the 0.1 and 0.8 headers. -Wl,-z,defs is deliberate: a
# missing symbol here would otherwise surface as an LD_PRELOAD failure at
# service start rather than at build time.
cp -r "$integration_root/src/chiron-camera" "$build/src"
"${cross}gcc" -I"$isp_root/usr/include" \
    -O2 -pipe -fPIC -Wall -Wextra -Werror \
    -shared -Wl,-z,defs -Wl,-soname,libchiron-ov13b10.so \
    -o "$out/libchiron-ov13b10.so" \
    "$build/src/chiron_ov13b10.c" \
    -L"$isp_root/usr/lib" -lmediaAPI -lispaml -lm -lpthread

"${cross}gcc" -O2 -Wall -Wextra -o "$out/chiron-camera-bridge-bin" \
    "$build/src/chiron-camera-bridge.c"
"${cross}gcc" -O2 -Wall -Wextra -o "$out/chiron-camera-privacy" \
    "$build/src/chiron-camera-privacy.c"

install -m 0644 "$build/src/ov13b10-sdr.calib" "$out/ov13b10-sdr.calib"
install -m 0644 "$build/src/ov13b10-sdr.calib.sha256" "$out/ov13b10-sdr.calib.sha256"
install -m 0755 "$build/src/chiron-camera-bridge" "$out/chiron-camera-bridge"

for binary in chiron-camera-bridge-bin chiron-camera-privacy libchiron-ov13b10.so; do
    file "$out/$binary" | grep -q "ARM aarch64" || {
        echo "$binary is not an aarch64 binary" >&2
        exit 1
    }
done

# The adapter is loaded with LD_PRELOAD in front of ispService, so it has to
# resolve against the runtime staged beside it and nothing else.
for library in libmediaAPI.so libispaml.so; do
    "${cross}readelf" -d "$out/libchiron-ov13b10.so" | grep -q "$library" || {
        echo "libchiron-ov13b10.so does not link against $library" >&2
        exit 1
    }
done

rm -rf "$build"
echo "Camera runtime, module and helpers ready: $out"
ls -l "$out"
