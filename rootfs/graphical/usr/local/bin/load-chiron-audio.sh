#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Load the complete Chiron ASoC stack, in order.
#
# This is not optional plumbing and it is not something udev can do. The sound
# card's first dai-link ends at the dummy codec, and
# amlogic-snd-codec-dummy.ko carries no MODULE_DEVICE_TABLE at all -- modinfo
# reports no aliases, so nothing in modules.alias matches the "amlogic,
# aml_dummy_codec" node and no uevent will ever load it. Without it
# aml_card_dai_link_of returns -517 on dai-link 0 for the lifetime of the boot,
# the card never registers, and ALSA reports "no soundcards".
#
# This same list used to be insmod'ed by absolute path out of the Android
# /cache partition. Here they are ordinary modules from the kernel this image
# ships, so this is modprobe; the order and the waits are unchanged, because
# they are about driver registration and not about symbols.
set -euo pipefail

load_module() {
    module=$1
    if grep -q "^${module//-/_} " /proc/modules 2>/dev/null; then
        echo "[chiron-audio] ${module} already loaded"
        return 0
    fi
    echo "[chiron-audio] loading ${module}"
    modprobe "$module"
}

# The AUGE register accessors are exported by aml_media, which the display
# stack brings up. Without it amlogic-snd-soc cannot resolve its symbols.
for attempt in $(seq 1 20); do
    if grep -q '^aml_media ' /proc/modules 2>/dev/null; then
        break
    fi
    if [ "$attempt" -eq 20 ]; then
        echo "[chiron-audio] aml_media did not become ready" >&2
        exit 1
    fi
    sleep 1
done

test -r /usr/lib/firmware/chiron-fs2105-woofer.cfg
test -r /usr/lib/firmware/chiron-fs2105-tweeters.cfg

load_module amlogic-i2c
load_module amlogic-snd-codec-dummy
load_module snd-soc-fs2105
load_module snd-soc-es7210
load_module amlogic-snd-soc

udevadm settle --timeout=10
grep -q 'AML-AUGESOUND' /proc/asound/cards
echo "[chiron-audio] AML-AUGESOUND ready"
