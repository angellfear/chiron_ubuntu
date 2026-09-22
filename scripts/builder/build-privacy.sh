#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Build chiron-privacy, the native session controller for the two hardware
# privacy switches, ambient-light policy, display settings and clock overlay.
#
# Everything else this builder compiles for the target links against libc and
# nothing more, so the bare Fenix cross toolchain is enough and build-camera.sh
# and friends just call ${cross}gcc. This one links GTK, GIO and
# gtk-layer-shell, whose headers do not exist on the builder side of the ABI.
# Cross-compiling them would mean an arm64 sysroot
# assembled by hand out of a hundred -dev packages and a pkg-config wrapper
# pointed at it; the ISP tree is done that way because that package has no
# other distribution, and this one does.
#
# So the compiler runs where the headers already are: a small arm64 chroot with
# build-essential, pkg-config and libgtk-layer-shell-dev, built from the same
# Fenix rootfs cache the image starts from and against the same apt
# configuration. It is deliberately NOT the image tree -- nothing here is
# installed into what ships, and the image never sees a -dev package.
#
# The chroot is cached beside the package layers and keyed the same way: change
# the base tarball, the apt configuration or the package list below and the key
# changes and it is rebuilt. So a builder pays ~30 MB of -dev packages and one
# apt run once, not once per workspace. The compile itself is one file under
# qemu-user and costs seconds.
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

cache=$(ls "$workspace"/artifacts/cache/rootfs/*.tar.lz4 2>/dev/null | head -n 1)
test -n "$cache" || {
    echo "no Fenix rootfs cache under $workspace/artifacts/cache/rootfs" >&2
    exit 1
}

# The build dependencies, and only these: apt resolves the rest.
build_packages="build-essential pkg-config libgtk-layer-shell-dev"

out="$workspace/artifacts/privacy"
mkdir -p "$out"

layer_root="$mirror_root/rootfs-layers"
mkdir -p "$layer_root"

hash_apt_config() {
    local dir
    for dir in "$integration_root/rootfs/common/etc/apt" \
               "$integration_root/rootfs/common/usr/share/keyrings"; do
        [ -d "$dir" ] || continue
        (cd "$dir" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
    done | sha256sum | cut -d' ' -f1
}

key=$(printf '%s\n' \
    v1 \
    "$UBUNTU_RELEASE" "$UBUNTU_ARCH" \
    "$(sha256sum "$cache" | cut -d' ' -f1)" \
    "$(hash_apt_config)" \
    "$build_packages" \
    | sha256sum | cut -d' ' -f1)
layer="$layer_root/privacy-build-$key.tar.lz4"

chroot_dir="$workspace/build-privacy"

# Deepest first, for the same reason assemble-output does it: the chroot can
# create submounts we never made, and a leftover bind of /dev turns a cleanup
# into deleting the builder's own device nodes.
umount_build() {
    awk -v r="$chroot_dir/" 'index($2, r) == 1 {print $2}' /proc/mounts \
        | sort -r | while IFS= read -r m; do umount "$m"; done
}
mount_build() {
    local m
    for m in proc sys dev; do
        mount --bind "/$m" "$chroot_dir/$m"
    done
    cp --remove-destination /etc/resolv.conf "$chroot_dir/etc/resolv.conf"
}
trap 'set +e; umount_build' EXIT

umount_build
rm -rf "$chroot_dir"
mkdir -p "$chroot_dir"

if [ -f "$layer" ]; then
    echo "reusing the privacy build chroot: $(basename "$layer")"
    lz4 -dc <"$layer" | tar xp --numeric-owner --xattrs-include='*' \
        -C "$chroot_dir"
    mount_build
else
    echo "building the privacy build chroot: $(basename "$layer")"
    lz4 -dc <"$cache" | tar xp --numeric-owner --xattrs-include='*' \
        -C "$chroot_dir"
    mount_build
    # The same extra apt sources the image resolves against, so the compiler
    # and the image agree on which archive a library came from.
    for rel in /etc/apt /usr/share/keyrings; do
        src="$integration_root/rootfs/common$rel"
        [ -d "$src" ] || continue
        install -d "$chroot_dir$rel"
        cp -a "$src/." "$chroot_dir$rel/"
    done
    chroot "$chroot_dir" apt-get update -qq
    # shellcheck disable=SC2086
    chroot "$chroot_dir" apt-get install -y -qq --no-install-recommends \
        $build_packages
    tmp="$layer.incomplete.$$"
    umount_build
    tar -C "$chroot_dir" -cp --numeric-owner --xattrs \
        --xattrs-include='*' . | lz4 -1 >"$tmp"
    mv -f "$tmp" "$layer"
    mount_build
fi

# Copied in rather than built in place: the integration checkout is edited on
# another host and must never receive arm64 objects.
install -d "$chroot_dir/build"
install -m 0644 "$integration_root/src/chiron-privacy/chiron-privacy.c" \
    "$chroot_dir/build/chiron-privacy.c"
install -m 0644 "$integration_root/src/chiron-privacy/chiron-display.c" \
    "$chroot_dir/build/chiron-display.c"
install -m 0644 "$integration_root/src/chiron-privacy/chiron-display.h" \
    "$chroot_dir/build/chiron-display.h"

# -Werror on purpose. This is a small file with no generated code in it, and
# the warnings a GTK program earns are the ones worth stopping for.
chroot "$chroot_dir" sh -c '
    set -e
    cd /build
    gcc -O2 -pipe -Wall -Wextra -Werror -o chiron-privacy \
        chiron-privacy.c chiron-display.c \
        $(pkg-config --cflags --libs gtk+-3.0 gio-2.0 gtk-layer-shell-0) \
        -lm
'

install -m 0755 "$chroot_dir/build/chiron-privacy" "$out/chiron-privacy"
umount_build
rm -rf "$chroot_dir"

echo "built: $out/chiron-privacy"
