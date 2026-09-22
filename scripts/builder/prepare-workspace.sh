#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

run_name=${1:-}
valid_run_name "$run_name" || {
    echo "usage: $0 <unique-run-name>" >&2
    exit 2
}
workspace="$temp_root/$run_name"
test ! -e "$workspace" || {
    echo "workspace already exists; choose a new run name: $workspace" >&2
    exit 1
}
mkdir -p "$workspace/sources" "$workspace/artifacts" "$mirror_root"

# Full mirrors are cloned once and cached forever: complete history keeps
# `git am --3way` and Fenix's git-derived versioning (git describe/tags) fully
# functional. Each source tree is checked out on its locked upstream branch at
# the locked commit, and the mirror's branch ref is pinned to the same commit.
# Fenix's own update_git_repo (checkout <branch> + git pull origin <branch>)
# then converges exactly to sources.lock during make. NO_GIT_UPDATE=1 stays
# available only as an emergency brake, never as the standard path. Moving to
# a newer upstream means updating sources.lock and preparing a fresh workspace.
prepare_tree() {
    local name=$1 url=$2 ref=$3 commit=$4 destination=$5
    local branch=${ref#refs/heads/}
    local mirror="$mirror_root/$name.git"
    if [ ! -d "$mirror" ]; then
        git clone --mirror "$url" "$mirror"
    else
        git -C "$mirror" fetch --prune origin
    fi
    git -C "$mirror" cat-file -e "$commit^{commit}"
    git -C "$mirror" update-ref "refs/heads/$branch" "$commit"
    git clone --no-checkout "$mirror" "$destination"
    git -C "$destination" checkout -B "$branch" "$commit"
    test -z "$(git -C "$destination" status --short)"
}

# Vendor binary packages. There is no source to build and no commit to pin, so
# they are pinned by URL and SHA-256 and cached beside the git mirrors. A
# checksum that does not match is fatal. They no longer go into the image --
# the board downloads them itself -- but fetching them here proves on every
# build that the pinned URLs still serve the pinned bytes the image will ask
# for.
fetch_vendor_deb() {
    local path=$1 sha=$2
    local file=${path##*/}
    local cache="$mirror_root/vendor-debs"
    mkdir -p "$cache"
    if [ ! -f "$cache/$file" ]; then
        curl -fsS --max-time 300 -o "$cache/$file.part" "$VENDOR_DEB_BASE/$path"
        mv "$cache/$file.part" "$cache/$file"
    fi
    echo "$sha  $cache/$file" | sha256sum -c - >/dev/null || {
        echo "checksum mismatch for $file; refusing to use it" >&2
        rm -f "$cache/$file"
        exit 1
    }
    cp "$cache/$file" "$workspace/vendor-debs/$file"
}

mkdir -p "$workspace/vendor-debs"
fetch_vendor_deb "$VENDOR_DEB_ION" "$VENDOR_DEB_ION_SHA256"
fetch_vendor_deb "$VENDOR_DEB_GE2D" "$VENDOR_DEB_GE2D_SHA256"
fetch_vendor_deb "$VENDOR_DEB_JPEGENC" "$VENDOR_DEB_JPEGENC_SHA256"
fetch_vendor_deb "$VENDOR_DEB_AMVENC" "$VENDOR_DEB_AMVENC_SHA256"
fetch_vendor_deb "$VENDOR_DEB_MULTIENC" "$VENDOR_DEB_MULTIENC_SHA256"
fetch_vendor_deb "$VENDOR_DEB_DRMBUFFERPOOL" "$VENDOR_DEB_DRMBUFFERPOOL_SHA256"
fetch_vendor_deb "$VENDOR_DEB_GSTREAMER_AML" "$VENDOR_DEB_GSTREAMER_AML_SHA256"
fetch_vendor_deb "$VENDOR_DEB_ADLA" "$VENDOR_DEB_ADLA_SHA256"
echo "vendor packages ready: $workspace/vendor-debs"

# The ISP userspace is not in the Khadas pool and not a bare .deb either: it is
# one package inside a source archive, so it is pinned twice -- the archive by
# its own checksum, and the .deb by its checksum once unpacked. Extracted here
# rather than in the build step so that everything the build consumes has been
# fetched and verified before any compiling starts.
fetch_ispaml() {
    local cache="$mirror_root/ispaml"
    local archive="$cache/${ISPAML_URL##*/}"
    mkdir -p "$cache"
    if [ ! -f "$archive" ]; then
        curl -fsSL --max-time 300 -o "$archive.part" "$ISPAML_URL"
        mv "$archive.part" "$archive"
    fi
    echo "$ISPAML_SHA256  $archive" | sha256sum -c - >/dev/null || {
        echo "checksum mismatch for the ispaml archive; refusing to use it" >&2
        rm -f "$archive"
        exit 1
    }

    local staging="$workspace/downloads/ispaml"
    rm -rf "$staging" "$workspace/sources/ispaml-0.8"
    mkdir -p "$staging" "$workspace/sources/ispaml-0.8"
    tar -xzf "$archive" -C "$staging"

    local deb
    deb=$(find "$staging" -path "*/$ISPAML_DEB" -print -quit)
    test -n "$deb" || {
        echo "the ispaml archive has no $ISPAML_DEB" >&2
        exit 1
    }
    echo "$ISPAML_DEB_SHA256  $deb" | sha256sum -c - >/dev/null || {
        echo "checksum mismatch for $ISPAML_DEB; refusing to use it" >&2
        exit 1
    }
    dpkg-deb -x "$deb" "$workspace/sources/ispaml-0.8"
    rm -rf "$staging"
}

mkdir -p "$workspace/downloads"
fetch_ispaml
echo "ISP userspace ready: $workspace/sources/ispaml-0.8"

prepare_tree fenix "$FENIX_URL" "$FENIX_REF" "$FENIX_COMMIT" "$workspace/sources/fenix"
prepare_tree linux "$LINUX_URL" "$LINUX_REF" "$LINUX_COMMIT" "$workspace/sources/linux"
prepare_tree common_drivers "$COMMON_DRIVERS_URL" "$COMMON_DRIVERS_REF" \
    "$COMMON_DRIVERS_COMMIT" "$workspace/sources/common_drivers"
prepare_tree w2 "$W2_URL" "$W2_REF" "$W2_COMMIT" "$workspace/sources/w2"
prepare_tree himax "$HIMAX_URL" "$HIMAX_REF" "$HIMAX_COMMIT" "$workspace/sources/himax"
prepare_tree v4l2loopback "$V4L2LOOPBACK_URL" "$V4L2LOOPBACK_REF" \
    "$V4L2LOOPBACK_COMMIT" "$workspace/sources/v4l2loopback"
prepare_tree optee_client "$OPTEE_CLIENT_URL" "$OPTEE_CLIENT_REF" "$OPTEE_CLIENT_COMMIT" \
    "$workspace/sources/optee_client"
# No u-boot checkout: u-boot is never built or flashed (the stock Amlogic
# U-Boot on the eMMC boots Image/initrd/dtb directly). The lock in sources.lock
# stays as documentation of the tested vendor pair; UBOOT=2019.01 remains a
# plain Fenix environment variable.

# Khadas 5.15 expects common_drivers beside and linked from the Linux tree.
# The link exists only in this disposable workspace, never in integration Git.
test ! -e "$workspace/sources/linux/common_drivers"
ln -s ../common_drivers "$workspace/sources/linux/common_drivers"

# Fenix fetches its cross toolchains and a few host packages from
# build.khadas.com before any compiling starts: about 800 MB, and the least
# reliable part of a clean build. They follow the pinned Fenix commit and
# nothing this repository changes, so keep the downloads beside the source
# mirrors and hand them to each new workspace. Fenix skips a download when the
# archive is already in downloads/<package>/ next to the .url and .sha256 files
# it writes there, so the whole directory is copied across as one unit; it then
# extracts locally, which costs a couple of minutes and no network.
#
# The extracted directories under build/ are deliberately NOT reused. Fenix
# moves a freshly extracted tree onto that path, and mv refuses to replace a
# symlink or to overwrite a directory -- linking them in makes a clean build
# fail outright, which is how this was found.
# Set CHIRON_FRESH_TOOLCHAINS=1 to fetch everything again.
fenix_downloads="$mirror_root/fenix-downloads"
mkdir -p "$fenix_downloads"
if [ "${CHIRON_FRESH_TOOLCHAINS:-0}" = 1 ]; then
    rm -rf "${fenix_downloads:?}"/*
fi
if [ -n "$(ls -A "$fenix_downloads" 2>/dev/null)" ]; then
    mkdir -p "$workspace/sources/fenix/downloads"
    cp -an "$fenix_downloads/." "$workspace/sources/fenix/downloads/" 2>/dev/null || true
    echo "reusing fenix downloads from $fenix_downloads"
fi

printf '%s\n' \
    "INTEGRATION_COMMIT=$(git -C "$integration_root" rev-parse HEAD)" \
    "WORKSPACE=$workspace" \
    "FENIX_COMMIT=$FENIX_COMMIT" \
    "W2_COMMIT=$W2_COMMIT" \
    "HIMAX_COMMIT=$HIMAX_COMMIT" \
    "V4L2LOOPBACK_COMMIT=$V4L2LOOPBACK_COMMIT" \
    "ISPAML_SHA256=$ISPAML_SHA256" \
    "OPTEE_CLIENT_COMMIT=$OPTEE_CLIENT_COMMIT" \
    "LINUX_COMMIT=$LINUX_COMMIT" \
    "COMMON_DRIVERS_COMMIT=$COMMON_DRIVERS_COMMIT" \
    > "$workspace/source-manifest.env"

echo "$workspace"
