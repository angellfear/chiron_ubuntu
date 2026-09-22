#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Load the Amlogic W2 (fullmac) Wi-Fi driver.
#
# Initialization order is the one proven on this board: the vendor providers,
# then w2_comm with bus_type=sdio, then the vendor power ABI
# (/sys/class/aml_wifi/power) which performs power down, 200 ms, power up and
# sdio_reinit(). Without that write the card is missed on a cold boot and only
# a much later rescan finds it. Only then does w2 load, which needs the
# completed function 7 probe.
set -euo pipefail

MODDIR="/lib/modules/$(uname -r)"
MCOMM="$MODDIR/extra/w2_comm.ko"
MW2="$MODDIR/extra/w2.ko"
MGKI="$MODDIR/kernel/common_drivers/drivers/gki_tool/amlogic-gkitool.ko"
MDEBUG="$MODDIR/kernel/common_drivers/drivers/debug/amlogic-debug.ko"
MPOWER="$MODDIR/kernel/common_drivers/drivers/power/amlogic-power.ko"
MPM="$MODDIR/kernel/common_drivers/drivers/pm/amlogic-pm.ko"
MWIRELESS="$MODDIR/kernel/common_drivers/drivers/wireless/amlogic-wireless.ko"

wait_for_path() {
    path=$1
    label=$2
    timeout=$3
    waited=0

    while [ ! -e "$path" ]; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[w2] timeout waiting for ${label}: ${path}" >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "[w2] ${label} ready after ${waited}s"
}

# The SDIO host index follows the VIM4N mmc aliases, so function 7 appears
# under mmc1 or mmc2 depending on which controllers are enabled. Match any
# host: the semantics are "wait for SDIO function 7".
sdio_func7_path() {
    set -- /sys/bus/sdio/devices/mmc*:0000:7
    [ -e "$1" ] && printf '%s\n' "$1"
}

wait_for_sdio_func7() {
    timeout=$1
    waited=0

    while [ -z "$(sdio_func7_path)" ]; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[w2] timeout waiting for SDIO function 7" >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "[w2] SDIO function 7 ready after ${waited}s: $(sdio_func7_path)"
}

load_module() {
    module=$1
    path=$2
    shift 2

    if grep -q "^${module} " /proc/modules 2>/dev/null; then
        echo "[w2] ${module} already loaded"
        return 0
    fi
    if [ ! -r "$path" ]; then
        echo "[w2] missing module: $path" >&2
        return 1
    fi

    echo "[w2] loading ${module}"
    if output=$(insmod "$path" "$@" 2>&1); then
        :
    else
        rc=$?
        printf '%s\n' "$output" | sed "s/^/[w2] ${module}: /" >&2
        return "$rc"
    fi
    if [ -n "$output" ]; then
        printf '%s\n' "$output" | sed "s/^/[w2] ${module}: /"
    fi
}

echo "[w2] loading W2 firmware path (bus_type=sdio)"
modprobe -q rfkill 2>/dev/null || true
rm -f /usr/lib/firmware/aml_w2_rf_0640_0000.txt
load_module amlogic_gkitool "$MGKI"
load_module amlogic_debug "$MDEBUG"
load_module amlogic_power "$MPOWER"
load_module amlogic_pm "$MPM"
load_module amlogic_wireless "$MWIRELESS"
load_module w2_comm "$MCOMM" bus_type=sdio
wait_for_path /sys/class/aml_wifi/power "aml_wifi power control" 10
echo 1 > /sys/class/aml_wifi/power
echo "[w2] requested Wi-Fi power-cycle and immediate SDIO rescan"
wait_for_sdio_func7 5
load_module w2 "$MW2"
wait_for_path /sys/class/net/wlan0 wlan0 15
ip link set wlan0 up 2>/dev/null || true
echo "[w2] wlan0 is up"
