#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Cross-build libpanfrost-drain.so, the preloaded library that makes a Mesa
# panfrost context wait for its last job before it is torn down. Why it
# exists is in src/panfrost-drain/panfrost-drain.c.
#
# The DRM uapi headers come from the workspace kernel tree -- the one this image
# runs -- because the toolchain sysroot carries no panfrost_drm.h.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -d "$workspace/sources/fenix" || {
    echo "usage: $0 /root/ubuntu26_temp/<run>" >&2
    exit 2
}
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

toolchain=$(ls -d "$workspace"/sources/fenix/build/toolchains/gcc-arm-aarch64-none-linux-gnu*/bin 2>/dev/null | head -n 1)
test -n "$toolchain" || { echo "no Fenix aarch64 toolchain in the workspace" >&2; exit 1; }
uapi="$workspace/sources/linux/include/uapi/drm"
test -f "$uapi/panfrost_drm.h" || { echo "no panfrost_drm.h in $uapi" >&2; exit 1; }

out="$workspace/artifacts/panfrost-drain"
build="$workspace/build-panfrost-drain"
rm -rf "$build"
mkdir -p "$out" "$build"

# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
cp -r "$integration_root/src/panfrost-drain" "$build/"
# Only the drm directory: the rest of include/uapi would shadow the sysroot's
# own linux/ headers with kernel-internal ones.
mkdir -p "$build/panfrost-drain/uapi/drm"
cp "$uapi"/drm.h "$uapi"/drm_mode.h "$uapi"/panfrost_drm.h \
    "$build/panfrost-drain/uapi/drm/"
PATH="$toolchain:$PATH" make -C "$build/panfrost-drain"

lib="$build/panfrost-drain/libpanfrost-drain.so"
file "$lib" | grep -q "ARM aarch64" || {
    echo "libpanfrost-drain.so is not an aarch64 library" >&2
    exit 1
}
cp "$lib" "$out/libpanfrost-drain.so"

rm -rf "$build"
echo "panfrost drain library ready: $out"
ls -l "$out"
