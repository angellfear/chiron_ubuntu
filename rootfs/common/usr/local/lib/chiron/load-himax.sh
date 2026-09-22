#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Load the Amlogic SPI controller and Himax HX83102E touchscreen driver.
#
# Two adaptations from the Android arrangement, both mechanical:
#
#   * insmod by absolute path out of the Android /cache partition becomes
#     modprobe: here these are ordinary modules of the kernel this image ships.
#   * the Android side waits for /sys/bus/spi/devices/spi3.0, where the 3
#     comes from the spi aliases block in its DTB. Our board tree carries no
#     spi aliases and neither does mesont7c.dtsi, so the SPI core numbers the
#     bus dynamically and the name is not spi3 by construction. The
#     controller has one child at
#     chip select 0, so the glob below means exactly what that fixed path
#     meant -- the same treatment load-w2.sh already gives the SDIO function.
#
# himax_mmi carries no module alias of any kind, so nothing autoloads it and
# this unit is the only thing that ever will. The orientation parameters live
# in /etc/modprobe.d/chiron-touch.conf, where the reasoning behind them is
# written out; modprobe applies them.
set -euo pipefail

load_module() {
    module=$1
    if grep -q "^${module//-/_} " /proc/modules 2>/dev/null; then
        echo "[himax] ${module} already loaded"
        return 0
    fi
    echo "[himax] loading ${module}"
    modprobe "$module"
}

spi_child_path() {
    set -- /sys/bus/spi/devices/spi*.0
    [ -e "$1" ] && printf '%s\n' "$1"
}

wait_for_spi_child() {
    timeout=$1
    waited=0
    while [ -z "$(spi_child_path)" ]; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[himax] timeout waiting for the SPI child device" >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "[himax] SPI child ready after ${waited}s: $(spi_child_path)"
}

wait_for_input() {
    timeout=$1
    waited=0
    while ! grep -lqx 'himax-touchscreen-mt' /sys/class/input/input*/name >/dev/null 2>&1; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[himax] timeout waiting for input device" >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "[himax] input device ready after ${waited}s"
}

# The image does not carry this firmware; chiron-firmware-import copies it off
# the board's own Android volumes on the first boot and reboots. So an absence
# here is the expected state of that one boot, not a broken image.
if ! test -r /usr/lib/firmware/Himax_firmware.bin; then
    echo "[himax] /usr/lib/firmware/Himax_firmware.bin is absent;" \
         "waiting for chiron-firmware-import to bring it in" >&2
    exit 1
fi
load_module amlogic-spi
wait_for_spi_child 5
load_module himax_mmi
wait_for_input 10
