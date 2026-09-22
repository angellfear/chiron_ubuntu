#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

workspace=${1:-}
test -d "$workspace/sources" || {
    echo "usage: $0 /root/ubuntu26_temp/<run>" >&2
    exit 2
}
case "$(readlink -f "$workspace")" in
    "$(readlink -f "$temp_root")"/*) ;;
    *) echo "workspace must be below $temp_root" >&2; exit 2 ;;
esac

apply_queue() {
    local tree=$1 queue=$2 checkout=$3 line patch
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%%#*}
        line=$(printf '%s' "$line" | xargs)
        [ -n "$line" ] || continue
        case "$line" in
            /*|*..*) echo "unsafe patch path in $queue: $line" >&2; exit 1 ;;
        esac
        patch="$integration_root/patches/$tree/$line"
        test -f "$patch" || { echo "missing patch: $patch" >&2; exit 1; }
        git -C "$checkout" am --3way "$patch"
    done < "$queue"
}

apply_queue fenix "$integration_root/patches/fenix/series" "$workspace/sources/fenix"
apply_queue linux "$integration_root/patches/linux/series" "$workspace/sources/linux"
apply_queue common_drivers "$integration_root/patches/common_drivers/series" \
    "$workspace/sources/common_drivers"
apply_queue w2 "$integration_root/patches/w2/series" "$workspace/sources/w2"
apply_queue himax "$integration_root/patches/himax/series" "$workspace/sources/himax"
apply_queue uboot "$integration_root/patches/uboot/series" "$workspace/sources/u-boot"
apply_queue v4l2loopback "$integration_root/patches/v4l2loopback/series" "$workspace/sources/v4l2loopback"

echo "production patch queues applied"
