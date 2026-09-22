#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail

integration_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
. "$integration_root/sources.lock"

case "${WSL_INTEROP:-}:$(cat /proc/sys/kernel/osrelease 2>/dev/null || true)" in
    *WSL*|*wsl*|*Microsoft*|*microsoft*)
        echo "WSL builds are forbidden; use the native Ubuntu builder" >&2
        exit 2
        ;;
esac

test "$(id -u)" = 0 || { echo "run on the builder as root" >&2; exit 2; }
grep -qx 'ID=ubuntu' /etc/os-release || {
    echo "the native Ubuntu builder is required" >&2
    exit 2
}

temp_root=${UBUNTU26_TEMP_ROOT:-/root/ubuntu26_temp}
mirror_root=${UBUNTU26_MIRROR_ROOT:-/root/ubuntu26_sources}

valid_run_name() {
    case "$1" in
        ''|*[!A-Za-z0-9._-]*) return 1 ;;
        *) return 0 ;;
    esac
}
