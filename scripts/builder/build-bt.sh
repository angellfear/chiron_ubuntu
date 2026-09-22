#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Cross-build chiron-bt-init, the W2 Bluetooth firmware loader.
#
# Not a kernel module: it is an ordinary static-ish userspace program that
# speaks Amlogic's vendor HCI commands over /dev/ttyS1 before btattach takes
# the UART over. It is built with the same Fenix toolchain as everything else
# so the image carries one aarch64 ABI and one libc expectation.
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

out="$workspace/artifacts/bt"
build="$workspace/build-bt"
rm -rf "$build"
mkdir -p "$out" "$build"

# Copied out of the integration checkout rather than built in place: the
# checkout is edited on another host and must never receive arm64 objects.
cp -r "$integration_root/src/chiron-bt-init" "$build/"
PATH="$toolchain:$PATH" make -C "$build/chiron-bt-init"

file "$build/chiron-bt-init/chiron-bt-init" | grep -q "ARM aarch64" || {
    echo "chiron-bt-init is not an aarch64 binary" >&2
    exit 1
}
cp "$build/chiron-bt-init/chiron-bt-init" "$out/chiron-bt-init"

rm -rf "$build"
echo "Bluetooth loader ready: $out"
ls -l "$out"
