#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"

check_ref() {
    local name=$1 url=$2 ref=$3 locked=$4 current
    current=$(git ls-remote "$url" "$ref" | awk 'NR == 1 {print $1}')
    test -n "$current" || { echo "$name: unable to resolve $ref" >&2; return 1; }
    if [ "$current" = "$locked" ]; then
        echo "$name: unchanged $locked"
    else
        echo "$name: update available $locked -> $current"
    fi
}

check_ref fenix "$FENIX_URL" "$FENIX_REF" "$FENIX_COMMIT"
check_ref linux "$LINUX_URL" "$LINUX_REF" "$LINUX_COMMIT"
check_ref common_drivers "$COMMON_DRIVERS_URL" "$COMMON_DRIVERS_REF" \
    "$COMMON_DRIVERS_COMMIT"
# u-boot is neither checked out nor built; see prepare-workspace.sh.
