#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Load the Amlogic USB driver.
#
# This SoC's USB is not the mainline dwc3 the kernel config advertises. The DT
# describes Amlogic CRG controllers ("amlogic, crg-drd", "amlogic, crg-otg",
# "amlogic, amlogic-crg-drd-usb2/3"), and the only driver that binds them lives
# in common_drivers and links into a single amlogic-usb.ko. Without it the
# platform devices sit unbound and the board has no USB at all -- no host
# controller, no /sys/bus/usb/devices, nothing plugged in ever appears.
#
# The USB-C port is host-only by device tree (controller-type = <1>); the OTG
# role machine cannot work on this board.
#
# Carried unchanged, with insmod from the cache partition replaced by
# modprobe: here the module is a proper part of the kernel package and
# depmod has resolved amlogic-gkitool for us.
set -uo pipefail

if grep -q '^amlogic_usb ' /proc/modules 2>/dev/null; then
    echo "[usb] amlogic-usb already loaded"
    exit 0
fi
if ! modprobe amlogic-usb; then
    echo "[usb] amlogic-usb failed to load" >&2
    exit 1
fi

# The root hubs appear synchronously with the xHCI registration, so their
# absence here means the controller did not come up rather than that a device
# is slow to enumerate.
for _ in 1 2 3 4 5; do
    [ -e /sys/bus/usb/devices/usb1 ] && break
    sleep 1
done
if [ ! -e /sys/bus/usb/devices/usb1 ]; then
    echo "[usb] amlogic-usb loaded but no USB bus was registered" >&2
    exit 1
fi
echo "[usb] USB host up: $(ls /sys/bus/usb/devices | grep -c '^usb') bus(es)"
