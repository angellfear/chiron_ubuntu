#!/bin/bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Load the Amlogic hardware video decoders and encoders.
#
# Order matters and is not discoverable at runtime: decoder_common registers
# the vdec platform driver every decoder attaches to, stream_input brings up
# /dev/amstream_*, and amvdec_ports is the V4L2 M2M front end that binds
# vcodec_dec and creates the /dev/video node userspace actually opens.
#
# These used to be insmod'ed by absolute path out of the Android /cache
# partition, because the root overlay had no room for them. Here they are
# ordinary modules from the kernel this image ships, so this is modprobe and
# the symbol dependencies resolve themselves; the order below is still the
# same one, because it is about driver registration and not about symbols.
#
# The secure firmware must already be in the secure world before any decoder
# streams. chiron-codec-load.service orders that; running this by hand after
# boot is safe because the firmware is loaded once per boot and this is
# idempotent.
set -uo pipefail

# decoder_common needs ionvideo_assign_map out of aml_media, which only exists
# when that module was built with CONFIG_AMLOGIC_IONVIDEO=y.
CORE="videobuf-core aml_media media_clock firmware decoder_common
      video_framerate_adapter pts_server stream_input"

# The V4L2 back ends. Each one registers the codec it handles with
# amvdec_ports; a missing one silently narrows what /dev/video can decode.
DECODERS="amvdec_mh264_v4l amvdec_h265_v4l amvdec_vp9_v4l amvdec_av1_v4l
          amvdec_mmpeg12_v4l amvdec_mmpeg4_v4l amvdec_mmjpeg_v4l
          amvdec_avs2_v4l amvdec_mavs_v4l"

failed=0

load() {
    module=$1
    if grep -q "^${module//-/_} " /proc/modules 2>/dev/null; then
        return 0
    fi
    modprobe "$module" || {
        echo "[codecs] failed to load ${module}" >&2
        return 1
    }
}

for m in $CORE; do
    if ! load "$m"; then
        echo "[codecs] core module ${m} failed; no hardware decode" >&2
        exit 1
    fi
done

# amvdec_ports carries two parameters that are not defaults and are not
# optional; they are set in /etc/modprobe.d/chiron-codec.conf so that a
# modprobe from anywhere gets them. See that file for what each one fixes.
if ! load amvdec_ports; then
    echo "[codecs] amvdec_ports failed; no hardware decode" >&2
    exit 1
fi

# A back end that fails costs one codec, not the whole stack, so keep going.
for m in $DECODERS; do
    load "$m" || failed=$((failed + 1))
done

# T7C production Android uses the CNM multi-encoder and the JPEG encoder. The
# older encoder.ko/vpu.ko pair is rejected by this SoC and must not be loaded.
# encoder_common is their reporting provider; decoder_common and aml_media are
# already active above. A working drm_stack aml_media also registers /dev/ge2d.
for m in encoder_common amvenc_multi jpegenc; do
    if ! load "$m"; then
        echo "[codecs] encoder module ${m} failed" >&2
        exit 1
    fi
done

node=$(grep -l aml-vcodec-dec /sys/class/video4linux/*/name 2>/dev/null | head -1)
if [ -n "$node" ]; then
    echo "[codecs] hardware decoder at /dev/$(basename "$(dirname "$node")")"
else
    echo "[codecs] no aml-vcodec-dec node appeared" >&2
    exit 1
fi

for device in /dev/ge2d /dev/amvenc_multi /dev/jpegenc; do
    if [ ! -c "$device" ]; then
        echo "[codecs] encoder device did not appear: $device" >&2
        exit 1
    fi
done
echo "[codecs] hardware encoders at /dev/amvenc_multi and /dev/jpegenc"

if [ "$failed" -gt 0 ]; then
    echo "[codecs] ${failed} decoder back end(s) did not load" >&2
fi

exit 0
