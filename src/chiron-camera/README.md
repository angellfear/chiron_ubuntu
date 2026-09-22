# Chiron OV13B10 camera

The board carries one OV13B10 on i2c4, four MIPI lanes into `csiphy0`, through
the adapter into the T7 ISP.

Everything is built from pinned sources. This directory holds the C sources and
the calibration payload; the ISP runtime comes out of a pinned package.

## What is in the image

* the board description -- the sensor node, its mclk chain and the endpoint
  link to `csiphy0`, in `patches/common_drivers/0001-*`;
* the ordered kernel patches that register the sensor, reserve camera CMA and
  keep the offline adapter queue balanced;
* `etc/udev/rules.d/72-chiron-camera.rules` and `99-chiron-camera-private.rules`,
  which keep every raw ISP node root-only and expose only the loopback node to
  the desktop account;
* `scripts/builder/build-camera.sh`, which builds `v4l2loopback.ko` against
  this kernel, compiles `libchiron-ov13b10.so` against the ispaml 0.8 headers
  and cross-builds the two helpers;
* the camera module, ISP, bridge and privacy units plus
  `/usr/local/bin/load-chiron-camera.sh`.

## Where the ISP userspace comes from

`ispaml` 0.8-202512~1.5. Khadas' own pool stops at 0.6 and has no resolute
suite, but 0.8 is published in `numbqq/multimedia_debs_aml`, and `sources.lock`
pins it twice -- the archive by its own SHA-256 and the `.deb` inside it by
another:

```
ISPAML_URL=https://github.com/numbqq/multimedia_debs_aml/archive/0bb677edc748670b1dc4eb14951430c6fffbeec9.tar.gz
ISPAML_DEB=focal/arm64/ispaml_0.8-202512~1.5_arm64.deb
```

`prepare-workspace.sh` fetches and verifies it and unpacks it to
`sources/ispaml-0.8`. That tree carries both halves the camera needs: the four
headers `chiron_ov13b10.c` compiles against -- `sensor_config.h`,
`staticPipe.h`, `v4l2subdev.h`, `aml_isp_adapt.h` -- and the runtime itself,
`ispService` plus `libispaml.so`, `libmediaAPI.so`, `libsns.so` and
`liblens.so`.

Only the runtime is a binary, because it has no source anywhere. Everything
that can be compiled, is.

The build uses the headers and links the adapter against the libraries, but
the runtime itself is not in the image: it is proprietary and published with
no licence text. `chiron-vendor-fetch` downloads the same pinned package on
the board once a network is up and installs `ispService` and the four
libraries into `/usr/lib/chiron-camera`. Until then the camera units are
skipped by their `ConditionPathExists` on `ispService`, and the fetcher starts
them once it has installed it.

## The calibration adapter

`chiron_ov13b10.c` feeds the factory Android SDR calibration to this ISP, whose
calibration enum gained four core and three optional slots since those tables
were captured. The index remap is the entire point of the file and was verified
against the 0.1 and 0.8 headers; the wrong mapping feeds a table such as FPNR
to an unrelated block. It is built with `-Wl,-z,defs` so a missing symbol fails
the build rather than surfacing as an `LD_PRELOAD` failure at service start.

`ov13b10-sdr.calib` is that calibration, serialized from
`Ov13b10SdrCalibration::dynamic_sdr_calibrations_init_ov13b10()` in the 32-bit
`/vendor/lib/hw/camera.amlogic.so`. `chiron-camera.service` checksums it before
starting.

## Why the pipeline works the way it does

The public v4l2loopback producer remains registered for the whole session so
browsers can always discover `/dev/video35`. The private ISP node and sensor
stream are opened only while a client is actively capturing. When the last
client closes, the bridge performs `STREAMOFF`, closes `/dev/video30` and stops
`chiron-camera.service`; before the next capture it rebuilds the userspace ISP
context and opens a fresh four-buffer stream.

The hardware node exposes only its validated mode, NV12 1920x1080. Accepting a
1280x720 allocation while the ISP still wrote a 1920x1080 frame corrupted the
right-hand quarter and could overwrite adjacent memory.

`ispService` is started as `-m /dev/media0 -M 2`. The mode matters: `-M 4`
selects the wrong internal mode.

v4l2loopback is pinned at 0.12.7 and loaded with
`video_nr=35 card_label=ChironCamera exclusive_caps=1 max_buffers=4`. Neither
the number nor the label is cosmetic: udev matches `ChironCamera` by name and
the bridge opens `/dev/video35` by number.

### Idling when nobody is looking

`patches/v4l2loopback/0001-*` adds a read-only `capture_active` attribute to the
loopback device: the number of openers that have `STREAMON`'d a capture and not
yet stopped or closed it. The bridge polls it every 250 ms and owns the entire
private-camera lifecycle. A read that fails is treated as active so failure of
the optional attribute cannot make the camera silently publish nothing.

`patches/common_drivers/0047-*` fixes the corresponding kernel lifecycle:

* a RAW buffer removed from `done_list` is returned when the userspace ISP is
  closed, preventing permanent depletion of `free_list`;
* the asynchronous offline path has a four-frame queue instead of two;
* OV13B10 changes its software stream state only after a successful hardware
  transition and does not take a duplicate MCLK reference.

`chiron-camera.service` is deliberately disabled as a boot unit. The image
assembler also removes a stale enable symlink from reused rootfs trees; only
the bridge starts and stops the ISP. While idle, `/dev/video30` has no holder
and the sensor/adapter pipeline produces no frame interrupts.

## Acceptance

Run on the clean assembled image on 2026-09-01 with no camera files in the
writable overlay:

| What | Result |
|---|---|
| system state after a captured cold boot | running, 0 failed units |
| camera services while idle | modules, bridge and privacy active; ISP inactive |
| what a client sees on `/dev/video35` | `Video Capture`, caps `0x85200001` |
| repeated browser capture | 6 Chromium + 4 Firefox sessions passed |
| clean-image confirmation | 3 Chromium + 2 Firefox sessions passed |
| frames checked visually | clean, no tearing or corruption |
| state after every close | ISP inactive, `/dev/video30` free |
| `ADAP0:Error free list empty` | 0 during capture and idle |
| dequeue errors, timeouts and bridge restarts | 0 |

## Files here

| File | What it is |
|---|---|
| `chiron_ov13b10.c` | the Chiron sensor callbacks and calibration adapter, compiled by `build-camera.sh` |
| `ov13b10-sdr.calib` | the factory SDR IQ calibration |
| `ov13b10-sdr.calib.sha256` | its checksum; the ISP unit verifies it before starting |
| `chiron-camera-bridge.c` | copies NV12 frames from the private ISP node to the public loopback node while something is capturing |
| `chiron-camera-bridge` | the wrapper that waits for both nodes and execs the binary |
| `chiron-camera-privacy.c` | watches `SW_CAMERA_LENS_COVER` and takes access to the public node away when the shutter closes |
| `units/` | source copies of the camera units; installed copies are in `rootfs/graphical/etc/systemd/system/` |
