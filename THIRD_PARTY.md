# Licensing and third-party material

This file records what this repository contains that is not its own work, where
each piece came from, and under what terms it is here. It is written in English
because its audience is the upstreams and rightsholders named in it.

## This project

Everything authored here — `scripts/`, `rootfs/`, `src/`, `tools/`,
`profiles/`, `config/`, the documentation — is under **0BSD**
(`LICENSES/0BSD.txt`), which asks nothing of anyone: no attribution, no notice
to preserve, no condition of any kind. It is released as a gift.

`patches/` and `drivers/` are the exception, and not by choice: they are
modifications to and loadable modules for the Linux kernel, so they are
derivative works of GPL-2.0 code and are under **GPL-2.0**
(`LICENSES/GPL-2.0.txt`). Every file carries an SPDX header stating which of
the two applies to it, and `LICENSE` holds the map.

One file in `src/` must stay permissive rather than follow `drivers/`:
`src/chiron-camera/chiron_ov13b10.c` is built into `libchiron-ov13b10.so`,
which links Amlogic's proprietary `libispaml` and `libmediaAPI` —
`scripts/builder/build-camera.sh` verifies that it does. GPL-2.0 does not
permit redistributing a work linked against proprietary libraries, so
relicensing that one file GPL-2.0 would hand every redistributor of this image
a violation they did not create. Nothing else in `src/` links anything
proprietary: `chiron-privacy` links GTK, gio and gtk-layer-shell (LGPL/MIT),
and the TEE client links only `libteec` (BSD-2-Clause).

Images built from this repository contain a modified Linux kernel. GPL-2.0 §3
obliges whoever distributes such an image to provide the corresponding source.
`sources.lock` pins every upstream tree by commit, and `patches/` carries every
change applied to them as a `git am` queue. Because those trees live in other
people's repositories, a release should not rely on them staying there:
`scripts/builder/export-sources.sh` packs every tree of the build workspace,
patched as built, plus this repository, into archives to ship beside the
image. It packs source only: the proprietary ISP package is not among them,
and the vendor binaries that Fenix and the W2 repository bundle beside their
source (WLAN and video firmware, prebuilt boot images, the W2 firmware and RF
tables, a diagnostic executable) are left out, with a check that refuses to
pack anything firmware-shaped.

## Upstream sources — not vendored

No upstream tree is copied into this repository. Each is pinned by URL and
commit in [`sources.lock`](sources.lock) and checked out by the build:

| Component | Upstream | Licence |
|---|---|---|
| Fenix build system | khadas/fenix | per that repository |
| Linux kernel | khadas/linux (5.15) | GPL-2.0 |
| Amlogic common_drivers | khadas/common_drivers | GPL-2.0 |
| W2 Wi-Fi/BT fullmac driver | khadas/…_wifi_bt_wifi_amlogic_w2 | per that repository |
| Himax touch module | android.googlesource.com google-modules/touch/himax_touch | GPL-2.0 |
| v4l2loopback 0.12.7 | umlaeute/v4l2loopback | GPL-2.0 |
| OP-TEE client | OP-TEE/optee_client | BSD-2-Clause |

## Amlogic userspace — installed on the board, not in the image

The Amlogic binary userspace for the hardware video, ISP and NPU paths is in
neither this repository nor the image: `libion`, `libge2d`, `libjpegenc`,
`libamvenc`, `libmultienc`, `gst-aml-drmbufferpool-plugins`, `gstreamer-aml`,
`libadla`, and the ISP runtime from `ispaml` (`ispService`, `libispaml.so`,
`libmediaAPI.so`, `libsns.so`, `liblens.so`).

These are proprietary Amlogic packages that Khadas publishes in its apt pool
and in `numbqq/multimedia_debs_aml`, with no licence text inside. The build
only compiles against the ISP headers and writes the URLs and SHA-256 pins
from `sources.lock` into the image. On the board, `chiron-vendor-fetch`
downloads them from those publishers once a network is up and refuses
anything whose checksum does not match. The copy on a board is downloaded by
that board, from Khadas; nobody who hands out the image hands out these.

## Out-of-tree drivers in `drivers/`

| Driver | Origin | Licence |
|---|---|---|
| `audio/es7210` | Everest Semiconductor's own ASoC driver, carried unchanged | GPL-2.0, © 2018 Everest Semiconductor Co., Ltd, David Yang. Attribution and `MODULE_LICENSE("GPL v2")` preserved |
| `audio/fs2105` | written here | GPL-2.0 |
| `led/aw21036` | written here against the AW21036 datasheet and confirmed on hardware | GPL-2.0 |
| `sensor/sc7a20e` | written here | GPL-2.0 |
| `sensor/stk3b6x` | written here | GPL-2.0 |

## Firmware — imported from the board on first boot

This image is shipped without the vendor firmware below; the W2 Wi-Fi firmware
is the one exception, explained after the table. There is no redistribution
grant for a single one of these files, so the repository names them and the
board supplies the bytes:

| File | Rightsholder |
|---|---|
| `/usr/lib/firmware/Himax_firmware.bin` | Himax Technologies |
| `/usr/lib/firmware/Himax_mpfw.bin` | Himax Technologies |
| `/usr/lib/optee_armtz/*.ta` (7 files) | Amlogic Co., Inc. (signed trusted applications) |
| `/usr/lib/firmware/video/video_ucode.bin` | Amlogic Co., Inc. (signed secure-world video package) |
| `/usr/lib/firmware/video/h264_enc.bin` | Amlogic Co., Inc. |
| `/usr/lib/firmware/amlbt/w2_bt_fw_uart.bin` | Amlogic Co., Inc. |
| `/usr/lib/firmware/chiron-fs2105-{woofer,tweeters}.cfg` | the appliance vendor (speaker tuning), in FourSemi's tool format |

Every board this image targets already carries them in its own Android
volumes. On first boot `chiron-firmware-import.service` copies them from those
read-only volumes into place and reboots. One of the seven TAs is marked
optional in the manifest: nothing in the image uses it and older firmware
revisions do not have it. Matching is deliberately not pinned
to a checksum: the vendor reissues these files, and a rebuild with a minor
change is the same firmware under a new hash. A recorded checksum is only a
fast path, and the recorded size exists to tell same-named files apart --
`usr/local/lib/chiron/firmware-import.manifest` explains both. What this
project distributes is that list, which is not anyone's work; what ends up in
`/usr/lib` is a copy the owner of the board made from their own device.

The amplifier tuning files are plain-text I2C and DSP-coefficient sequences
emitted by FourSemi's tuning tool; the files still carry that tool's header.
They sit in the device's `/vendor` volume under the same names, so they are
imported like the firmware rather than carried.

### The one exception: W2 Wi-Fi firmware

`/usr/lib/firmware/wifi_w2_fw_sdio.bin` and `/usr/lib/firmware/aml_wifi_rf.txt`
are in the image. They come from Khadas' public W2 driver repository, the
same pinned tree the `w2` and `w2_comm` modules are built from, which
publishes them beside the GPL driver source but carries no licence file.

They are not imported from the board because that does not work. The
board's own copies are a different firmware build, and with the Khadas
driver they fail under load. Tested 2026-09-24 on a board with the October
2023 Android firmware: during a download the driver kept resizing its RX
buffer, the link dropped, and after reassociation no traffic passed at all.
Moving the pinned Khadas copy back restored the link, and 38 MB then
downloaded at 3.8 MB/s with no RX-buffer resets. They cannot be downloaded on
the board either, because Wi-Fi is the board's only network and this is the
firmware that brings it up.

Rightsholder: Amlogic Co., Inc. If Amlogic or Khadas object, these files
will be removed; the image would then ship without working Wi-Fi.

The Zigbee coordinator's own firmware is not in this list and not in the
image. It lives on the Telink coordinator chip; the host only talks to it
over a serial line and never loads or reflashes it, so there is nothing for
this project to carry or import.

## Unresolved

| Item | Status |
|---|---|
| `rootfs/graphical/usr/local/share/chiron/wallpaper-{landscape,portrait}.png` | Made by this project's author; no third party's file was copied. Noted for completeness: the images depict a green falling-code wall and a white rabbit, which are recognisable visual motifs of *The Matrix* (Warner Bros.). That is a question about what is depicted, not about who made the file. |
| `src/chiron-camera/ov13b10-sdr.calib` | Retained. It is the factory SDR IQ calibration for the OV13B10 module, serialised from the tables `Ov13b10SdrCalibration::dynamic_sdr_calibrations_init_ov13b10()` builds inside the 32-bit `/vendor/lib/hw/camera.amlogic.so`. It is sensor measurement data rather than program code. It is not a standalone file on the device, and extracting it on the board was tried and rejected: the HAL of the July 2026 Android firmware yields exactly this file, but the HAL of the October 2023 firmware lays its tables out differently and three of them hold other values, so extraction would break the camera on those boards. Raised here rather than left unmentioned. |

## Reporting

If you hold rights in anything in this repository -- whether or not it is
listed above -- and object to how it is used here, it will be taken down.
Open an issue, or, if you would rather not do so in public, use the hosting
platform's own report/takedown channel. You do not need to establish the
claim in court first: a credible request from the rightsholder or an
authorised representative is enough to act on.

On such a request the material is removed, or moved to the first-boot import
-- where the board supplies the file and the repository keeps only its
checksum -- as promptly as the maintainer can act. This project is
non-commercial and earns nothing, and takedown is the remedy it offers;
nothing here is meant to be kept over a rightsholder's objection.
