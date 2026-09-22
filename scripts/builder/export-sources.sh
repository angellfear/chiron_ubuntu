#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Pack the complete corresponding source of a built workspace, for a release.
#
# GPL-2.0 section 3 obliges whoever hands out an image to hand out the source
# it was built from. sources.lock and patches/ describe that source exactly,
# but they point at other people's repositories, and a branch that is later
# rewritten upstream takes the pinned commit with it. So a release carries the
# source itself: every tree of the workspace as it was built -- the pinned
# commit with this repository's patch queue applied on top -- plus this
# repository at the commit that built it.
#
# The ISP userspace tree (sources/ispaml-0.8) is left out on purpose: it is a
# proprietary binary package with no source, it is not in the image, and it is
# not this project's to hand out.
#
#   usage: export-sources.sh /root/ubuntu26_temp/<run>
#   output: <run>/artifacts/source/*.tar.xz and SHA256SUMS
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

out="$workspace/artifacts/source"
rm -rf "$out"
mkdir -p "$out"

# Source, not blobs. Two of the trees carry vendor binaries beside their
# source, and an archive of the whole tree would hand out exactly what the
# image was built not to carry: Fenix bundles WLAN and Amlogic video firmware
# and prebuilt U-Boot/ATF/Rockchip images, and the W2 repository bundles its
# firmware for three buses, RF tables and a diagnostic executable. None of it
# is source of anything in the image; the W2 driver needs only the headers in
# common/. The image's own copy of the W2 SDIO firmware is covered in
# THIRD_PARTY.md, not here.
excludes() {
    case "$1" in
        fenix)
            printf '%s\n' ':(exclude)archives' \
                ':(exclude)packages/u-boot-mainline/fip' \
                ':(exclude)packages/u-boot-mainline/atf' \
                ':(exclude)packages/rkbin/files'
            ;;
        w2)
            printf '%s\n' ':(exclude)common/*.bin' ':(exclude)common/*.txt' \
                ':(exclude)common/aml_comm_diag' ':(exclude)common/w2_rf'
            ;;
    esac
}

for name in linux common_drivers w2 himax v4l2loopback optee_client fenix; do
    tree="$workspace/sources/$name"
    test -d "$tree/.git" || { echo "no $name tree in $workspace/sources" >&2; exit 1; }
    test -z "$(git -C "$tree" status --short --untracked-files=no)" || {
        echo "$name has uncommitted changes; the archive would not be what was built" >&2
        exit 1
    }
    # Untracked files are build output (and, in linux, the common_drivers
    # link the kernel build expects); git archive leaves them out anyway.
    head=$(git -C "$tree" rev-parse --short=12 HEAD)
    echo "packing $name at $head"
    mapfile -t skip < <(excludes "$name")
    git -C "$tree" archive --format=tar --prefix="$name/" HEAD -- . "${skip[@]}" |
        xz -T0 -6 >"$out/$name-$head.tar.xz"
    # The safety net: whatever the exclusions miss, a firmware-shaped file
    # does not leave in a source archive.
    if blobs=$(xz -dc "$out/$name-$head.tar.xz" | tar -t |
            grep -i -E '\.(bin|fw|ta|ucode|img|elf|so|ko|o|a)$'); then
        echo "$name: firmware-shaped files would be packed:" >&2
        printf '  %s\n' $blobs >&2
        exit 1
    fi
done

integration=$(git -C "$integration_root" rev-parse --short=12 HEAD)
test -z "$(git -C "$integration_root" status --short)" || {
    echo "the integration checkout has uncommitted changes" >&2
    exit 1
}
git -C "$integration_root" archive --format=tar --prefix=chiron_ubuntu/ HEAD |
    xz -T0 -6 >"$out/chiron_ubuntu-$integration.tar.xz"

(cd "$out" && sha256sum -- *.tar.xz >SHA256SUMS)
echo "corresponding source ready: $out"
ls -l "$out"
