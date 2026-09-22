#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
profile=${2:-}
mode=${3:-full}
test -d "$workspace/sources/fenix" || {
    echo "usage: $0 /root/ubuntu26_temp/<run> graphical [kernel-only]" >&2
    exit 2
}
case "$mode" in full|kernel-only) ;; *) echo "invalid mode: $mode" >&2; exit 2 ;; esac
case "$profile" in graphical) ;; *) echo "invalid profile: $profile" >&2; exit 2 ;; esac
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

fenix="$workspace/sources/fenix"
test -f "$fenix/config/boards/Chiron.conf" || {
    echo "the accepted Fenix Chiron board patch is not present" >&2
    echo "develop it in this temporary workspace; do not bypass this check" >&2
    exit 1
}
profile_config="$integration_root/config/fenix/$profile.conf"
test -f "$profile_config"

# Fenix defaults CCACHE_DIR to build/ccache, which made every disposable
# workspace start cold and discarded the cache with the workspace.  Keep one
# cache beside the source/download mirrors and link each clean build to it.
shared_ccache="$mirror_root/ccache"
mkdir -p "$shared_ccache" "$fenix/build"
if [ ! -e "$fenix/build/ccache" ]; then
    ln -s "$shared_ccache" "$fenix/build/ccache"
elif [ ! -L "$fenix/build/ccache" ]; then
    echo "workspace has a private compiler cache: $fenix/build/ccache" >&2
    echo "refusing to hide it; prepare a clean workspace or migrate it first" >&2
    exit 1
fi

export CHIRON_INTEGRATION_ROOT="$integration_root"
export LINUX_VENDOR_DIR="$workspace/sources/linux"
export LINUX_COMMON_DRIVERS_DIR="$workspace/sources/common_drivers"
export BUILD_IMAGES="$workspace/artifacts"

cd "$fenix"
# Fenix scripts are not written for `set -u` and reference unset variables
# freely (REUSE in setenv.sh); keep -e but drop -u for the whole Fenix call.
set +u
# shellcheck source=/dev/null
source env/setenv.sh config "$profile_config"

# Chiron output is squashfs + overlay loaded by the stock Amlogic U-Boot and
# its boot menu: no u-boot build and no Fenix disk image. Fenix provides the
# kernel (compiled and packaged as a deb with modules, built from
# chiron_defconfig selected by the Chiron board config) and the base rootfs
# cache; the Chiron layer packages them afterwards.
# kernel-only stops after Image/dtbs/modules for fast board iteration; no deb
# packaging, no rootfs, no assemble.
if [ "$mode" = kernel-only ]; then
    make kernel
    set -u
    exit 0
fi
# With the integration-owned LINUX_DIR outside Fenix's $BUILD, bindeb-pkg
# leaves the debs next to the source tree and Fenix's closing `mv $BUILD/*.deb`
# fails after everything is already built. Recover by collecting the debs
# ourselves instead of patching Fenix.
if ! make kernel-deb; then
    mkdir -p "$BUILD_IMAGES/debs"
    mv "$workspace"/sources/*.deb "$BUILD_IMAGES/debs/"
    rm -f "$workspace"/sources/*.buildinfo "$workspace"/sources/*.changes
fi
ls "$BUILD_IMAGES"/debs/*linux-image*.deb >/dev/null 2>&1 || {
    echo "kernel-deb produced no linux-image deb" >&2
    exit 1
}

# W2 Wi-Fi modules are external and must be built from the same kernel tree
# and Module.symvers as the image they ship in.
bash "$integration_root/scripts/builder/build-w2.sh" "$workspace"

# The Himax touch module is external for the same reason and with the same
# constraint: it has to carry this kernel's vermagic and resolve against this
# kernel's Module.symvers.
bash "$integration_root/scripts/builder/build-himax.sh" "$workspace"

# The two audio codecs common_drivers does not carry, same constraint again.
bash "$integration_root/scripts/builder/build-audio.sh" "$workspace"

# The accelerometer and the light sensor, likewise external.
bash "$integration_root/scripts/builder/build-sensors.sh" "$workspace"

# The LED ring's four controllers, likewise external.
bash "$integration_root/scripts/builder/build-ledring.sh" "$workspace"

# The camera: the loopback module against this kernel, and the two userspace
# helpers. The ISP runtime itself has no source and ships as pinned binaries.
bash "$integration_root/scripts/builder/build-camera.sh" "$workspace"

# The W2 Bluetooth firmware loader: userspace, but cross-built here for the
# same reason -- one toolchain, one ABI in the image.
bash "$integration_root/scripts/builder/build-bt.sh" "$workspace"

# The panfrost drain library, preloaded into every process of the graphical
# image. Userspace, cross-built for the same reason as the loader above.
bash "$integration_root/scripts/builder/build-panfrost-drain.sh" "$workspace"

# The OP-TEE normal-world userspace, likewise.
bash "$integration_root/scripts/builder/build-tee.sh" "$workspace"

# The ADLA NPU driver: in common_drivers, but with no Kconfig symbol of its own.
bash "$integration_root/scripts/builder/build-npu.sh" "$workspace"

# The Fenix rootfs cache tar is a plain debootstrap of the distro release for
# the architecture. It carries no kernel, no patch queue and none of our
# packages: assemble-output installs the profile package lists into
# rootfs-<profile> afterwards. Debootstrap and its apt run are the slowest step
# of a fresh workspace, so keep the tar beside the source mirrors and let a new
# workspace start from it. Fenix names the tar after distro, release, arch and
# type, so a base change lands on a different name instead of reusing this one.
# Set CHIRON_FRESH_ROOTFS_CACHE=1 to force debootstrap to run again.
rootfs_cache="$mirror_root/rootfs-cache"
mkdir -p "$rootfs_cache" "$BUILD_IMAGES/cache/rootfs"
# A builder can retain caches for several Fenix base types.  The graphical
# Chiron profile deliberately uses the minimal Fenix base and adds Wayfire in
# assemble-output, so feeding it a gnome tar creates a mixed desktop.  Keep the
# workspace cache directory unambiguous and select only the configured base.
rootfs_cache_prefix="${DISTRIB_RELEASE}-${DISTRIB_TYPE}-${DISTRIB_ARCH}."
rm -f "$BUILD_IMAGES"/cache/rootfs/*.tar.lz4
if [ "${CHIRON_FRESH_ROOTFS_CACHE:-0}" = 1 ]; then
    rm -f "$rootfs_cache"/"$rootfs_cache_prefix"*.tar.lz4
else
    mapfile -t cached_rootfs < <(
        find "$rootfs_cache" -maxdepth 1 -type f \
            -name "${rootfs_cache_prefix}*.tar.lz4" -print | sort
    )
    if [ "${#cached_rootfs[@]}" -gt 1 ]; then
        echo "multiple matching rootfs caches under $rootfs_cache" >&2
        printf '  %s\n' "${cached_rootfs[@]}" >&2
        exit 1
    elif [ "${#cached_rootfs[@]}" -eq 1 ]; then
        cp "${cached_rootfs[0]}" "$BUILD_IMAGES/cache/rootfs/"
        echo "reusing the base rootfs cache: ${cached_rootfs[0]}"
    fi
fi
# CREATE_ROOTFS_CACHE_ONLY recreates the cache unconditionally, so only run it
# when no cache tar exists yet; assemble-output unpacks the tar either way.
if ! ls "$BUILD_IMAGES"/cache/rootfs/*.tar.lz4 >/dev/null 2>&1; then
    CREATE_ROOTFS_CACHE_ONLY=yes ./scripts/rootfs_stage.sh
    mapfile -t generated_rootfs < <(
        find "$BUILD_IMAGES/cache/rootfs" -maxdepth 1 -type f \
            -name "${rootfs_cache_prefix}*.tar.lz4" -print | sort
    )
    test "${#generated_rootfs[@]}" -eq 1 || {
        echo "Fenix produced no unique $rootfs_cache_prefix rootfs cache" >&2
        exit 1
    }
    cp "${generated_rootfs[0]}" "$rootfs_cache/"
fi

# The session tray indicators. Last of the component builds, because this
# is the only one that needs the Fenix rootfs cache: it compiles in a small
# arm64 chroot rather than with the cross toolchain, and the cache tar is
# what that chroot is made from.
bash "$integration_root/scripts/builder/build-privacy.sh" "$workspace"

bash "$integration_root/scripts/image/assemble-output.sh" "$workspace" "$profile"
set -u
