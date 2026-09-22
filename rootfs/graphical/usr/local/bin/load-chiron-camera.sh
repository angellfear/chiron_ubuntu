#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Bring up the OV13B10 media graph and the loopback node the bridge publishes.
#
# An earlier version of this script insmod'ed amlsens.ko and amlcam.ko by
# absolute path out of the Android /cache partition. Here both are part of the
# kernel and bind from the board device tree the moment i2c4 and the CSI phy
# probe, so there is nothing to load: this waits for the graph the DT builds
# instead of creating it. v4l2loopback is still an out-of-tree module and is
# still loaded here, with the same parameters as before -- the node number
# and card label are not cosmetic, udev matches ChironCamera by name and the
# bridge opens /dev/video35 by number.
set -euo pipefail

wait_for() {
    local what=$1 path=$2 i
    for i in $(seq 1 100); do
        [ -e "$path" ] && return 0
        sleep 0.1
    done
    echo "[chiron-camera] timeout waiting for $what ($path)" >&2
    return 1
}

# The sensor binds at address 0x36; without it the ISP graph comes up empty and
# ispService starts against nothing.
#
# The adapter number is not part of the board description -- it is handed out
# when the controller probes, and it is not the same here as it was under
# Android. A wait on 4-0036 was carried over at first, which
# on this kernel is 3-0036: the wait always timed out, the unit failed and the
# whole chain behind it never started, even though the sensor had probed
# cleanly. So match on the address and let the bus be whatever it is.
wait_for_sensor()
{
    local i path
    for i in $(seq 1 100); do
        for path in /sys/bus/i2c/drivers/amlsens/*-0036; do
            [ -e "$path" ] && return 0
        done
        sleep 0.1
    done
    echo "[chiron-camera] timeout waiting for the OV13B10 sensor to bind" >&2
    return 1
}

wait_for_sensor

wait_for "the ISP media graph" /dev/media0
wait_for "the ISP capture node" /dev/video30

if ! grep -q '^v4l2loopback ' /proc/modules 2>/dev/null; then
    echo "[chiron-camera] loading v4l2loopback"
    modprobe v4l2loopback \
        video_nr=35 card_label=ChironCamera exclusive_caps=1 max_buffers=4
fi
wait_for "the loopback node" /dev/video35

# v4l2loopback creates the node before the seat ACL worker necessarily runs.
# Apply the public-device ownership explicitly at the point where the module
# is ready, so a late uaccess event cannot leave the browser-facing node at
# mode 000.  Raw ISP nodes remain protected by 72-chiron-camera.rules.
chown root:video /dev/video35
chmod 0660 /dev/video35

echo "[chiron-camera] media graph and loopback node are ready"
