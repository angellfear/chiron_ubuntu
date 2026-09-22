#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../builder/common.sh
. "$here/../builder/common.sh"

workspace=${1:-}
profile=${2:-}
test -d "$workspace/sources/fenix" && test -n "$profile" || {
    echo "usage: $0 /root/ubuntu26_temp/<run> graphical" >&2
    exit 2
}
case "$profile" in graphical) ;; *) echo "invalid profile: $profile" >&2; exit 2 ;; esac

for tool in lz4 mksquashfs mkimage; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

cache=$(ls "$workspace"/artifacts/cache/rootfs/*.tar.lz4 2>/dev/null | head -n 1)
test -n "$cache" || { echo "no Fenix rootfs cache under $workspace/artifacts/cache/rootfs" >&2; exit 1; }

rootfs="$workspace/rootfs-$profile"
out="$workspace/artifacts/$profile"
mkdir -p "$out"

kdebs=$(ls "$workspace"/artifacts/debs/*linux-image*.deb 2>/dev/null | grep -v -- '-dbg' || true)
test -n "$kdebs" || { echo "no linux-image deb under $workspace/artifacts/debs" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Layered package cache.
#
# Installing the profile packages is by far the longest part of an image build:
# every dpkg maintainer script runs under qemu-user, so a graphical profile
# costs hours. It is also the part that changes least. A clean workspace has
# neither the tree nor the stamp, though, so the old in-workspace reuse never
# helped the case that matters -- the clean production rebuild -- and every
# candidate paid the full apt run again.
#
# So the expensive prefix is cached outside the workspace, as two layers:
#
#   Fenix base rootfs
#     -> stage apt sources/preferences/keyrings, apt-get update
#     -> base packages               -> base-<key>.tar.lz4
#     -> profile packages on top     -> <profile>-<key>.tar.lz4
#
# A layer is addressed by the hash of everything that can change what is inside
# it: the Fenix base tarball, the package lists that were installed into it,
# the apt configuration those lists were resolved against, and the Ubuntu
# release and architecture. Change any of them and the key changes, the layer
# misses, and the packages are installed again -- which is the only thing that
# makes reusing a prebuilt tree safe. The profile layer chains on the base
# key, so a base change necessarily invalidates the profile layer too. The
# base list is not a profile of its own; it is split out only because it
# changes far less often than the desktop list above it.
#
# What is deliberately NOT in a layer: the candidate kernel and its modules
# and the rootfs overlay. Those are what iterates, they are applied after the
# layer is restored, and keeping them out means a kernel or overlay change
# never invalidates hours of apt. (The Amlogic vendor packages are in neither:
# the board downloads them, see chiron-vendor-fetch.)
#
# A clean production rebuild stays clean: the layer is a cache of the same apt
# operations this script would perform, keyed by their inputs, not a
# hand-maintained artefact.
layer_root="$mirror_root/rootfs-layers"
mkdir -p "$layer_root"

# Content and path hash of the apt configuration the packages resolve against.
hash_apt_config() {
    local dir
    for dir in "$integration_root/rootfs/common/etc/apt" \
               "$integration_root/rootfs/common/usr/share/keyrings"; do
        [ -d "$dir" ] || continue
        (cd "$dir" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
    done | sha256sum | cut -d' ' -f1
}

file_hash() { sha256sum "$1" | cut -d' ' -f1; }

# v2 is the layer format. Bump it if the layer contents stop being produced by
# the same steps, so old tarballs cannot be mistaken for new ones. (v2: the
# base is upgraded to the updates and security pockets before any package
# list goes in.)
base_key=$(printf '%s\n' \
    v2 \
    "$UBUNTU_RELEASE" "$UBUNTU_ARCH" \
    "$(file_hash "$cache")" \
    "$(hash_apt_config)" \
    "$(file_hash "$integration_root/profiles/base/packages.list")" \
    | sha256sum | cut -d' ' -f1)
base_layer="$layer_root/base-$base_key.tar.lz4"

profile_key=$(printf '%s\n' \
    v2 "$base_key" \
    "$(file_hash "$integration_root/profiles/$profile/packages.list")" \
    | sha256sum | cut -d' ' -f1)
profile_layer="$layer_root/$profile-$profile_key.tar.lz4"

target_key=$profile_key
target_layer=$profile_layer

# Third layer: the development scratch layer.
#
# Iterating on a package set in a temporary workspace used to cost a full apt
# run per attempt, because any edit to the profile list changes the profile key
# and misses its layer -- twenty attempts, twenty multi-hour runs under
# qemu-user. CHIRON_DEV_LAYER=1 adds profiles/dev/packages.list as a layer of
# its own, chained on the profile layer: the profile layer underneath is
# untouched and keeps hitting its cache, so only the dev delta is reinstalled.
#
# It is a package-cache layer and nothing else. It does not create a profile,
# does not change the image name, and the image still comes out as
# <profile>.sqfs. A clean production build simply leaves the variable unset and
# resolves to the profile layer -- which is the same tarball the dev builds were
# stacking on, so the final build reuses it instead of starting cold.
dev_list="$integration_root/profiles/dev/packages.list"
use_dev_layer=0
if [ "${CHIRON_DEV_LAYER:-0}" = 1 ]; then
    [ -f "$dev_list" ] || {
        echo "CHIRON_DEV_LAYER=1 but $dev_list does not exist" >&2
        exit 2
    }
    use_dev_layer=1
    target_key=$(printf '%s\n' \
        v2 "$profile_key" "$(file_hash "$dev_list")" \
        | sha256sum | cut -d' ' -f1)
    target_layer="$layer_root/$profile-dev-$target_key.tar.lz4"
    echo "dev package layer enabled: $(basename "$target_layer")"
fi

# Unmount EVERYTHING below the rootfs tree, deepest first: the chroot can
# create submounts we never mounted ourselves (dev/pts, shm, ...), and a
# leftover bind of /dev turned an rm -rf of the rootfs into deleting the
# builder's own device nodes once.
umount_chroot() {
    awk -v r="$rootfs/" 'index($2, r) == 1 {print $2}' /proc/mounts \
        | sort -r | while IFS= read -r m; do umount "$m"; done
}
mount_chroot() {
    local m
    for m in proc sys dev; do
        mount --bind "/$m" "$rootfs/$m"
    done
    cp --remove-destination /etc/resolv.conf "$rootfs/etc/resolv.conf"
}
cleanup() {
    set +e
    umount_chroot
}
trap cleanup EXIT

unpack_tree() {
    echo "unpacking ${2:-rootfs}: $(basename "$1")"
    lz4 -dc <"$1" | tar xp --numeric-owner --xattrs-include='*' -C "$rootfs"
}

# Written to a temporary name and renamed, so a build interrupted mid-write
# cannot leave a truncated tarball behind under a key that says it is complete.
# The bind mounts come off first: with /proc and /dev bound, tar would archive
# the builder's own kernel and device state into the layer.
save_layer() {
    local dest=$1 tmp
    tmp="$dest.incomplete.$$"
    umount_chroot
    echo "saving rootfs layer: $(basename "$dest")"
    tar -C "$rootfs" -cp --numeric-owner --xattrs --xattrs-include='*' . \
        | lz4 -1 >"$tmp"
    mv -f "$tmp" "$dest"
    mount_chroot
}

stage_apt_sources() {
    # Extra apt sources have to be in place before the first update, not at
    # overlay time far below: the browser debs come from the Armbian
    # resolute-desktop suite, and with no sources file for it apt resolves
    # `firefox` and `chromium` to the Ubuntu snap transition packages and fails
    # outright on `chromium-sandbox`, which has no Ubuntu counterpart at all.
    # The same files are copied again with the rest of the overlay later; the
    # content is identical, and it is hashed into the layer key above.
    local apt_rel apt_src
    for apt_rel in /etc/apt /usr/share/keyrings; do
        apt_src="$integration_root/rootfs/common$apt_rel"
        [ -d "$apt_src" ] || continue
        install -d "$rootfs$apt_rel"
        cp -a "$apt_src/." "$rootfs$apt_rel/"
        echo "staged $apt_rel for apt"
    done
}

# A plain upgrade, never a full one: it installs no new packages and removes
# none, so a package whose update needs a new dependency is held back rather
# than widening the image behind the package lists' back.
upgrade_rootfs() {
    chroot "$rootfs" env DEBIAN_FRONTEND=noninteractive \
        apt-get upgrade -y -qq --no-install-recommends \
        -o Dpkg::Options::=--force-confold
}

install_package_list() {
    local packages
    packages=$(sed 's/#.*//' "$@" | xargs)
    echo "installing packages from: $*"
    # shellcheck disable=SC2086
    chroot "$rootfs" apt-get install -y -qq --no-install-recommends $packages
}

# The stamp now records the layer key rather than the base tarball name, so a
# tree left in the workspace is reused only when it holds exactly the packages
# this build asks for. The tree itself is part of the condition, not just the
# stamp: a stamp left behind by a hand-removed rootfs made this print "reusing
# rootfs tree" and then fail on the first bind mount, with no rootfs anywhere.
stamp="$workspace/.rootfs-$profile.stamp"
if [ -f "$stamp" ] && [ -d "$rootfs" ] && grep -qx "$target_key" "$stamp"; then
    echo "reusing rootfs tree: $rootfs"
    mount_chroot
else
    umount_chroot
    rm -rf "$rootfs"
    mkdir -p "$rootfs"
    if [ -f "$target_layer" ]; then
        unpack_tree "$target_layer" "$profile package layer"
        mount_chroot
    elif [ "$use_dev_layer" = 1 ] && [ -f "$profile_layer" ]; then
        # The dev delta missed but the profile layer underneath is present:
        # restore it and install only the dev list on top.
        unpack_tree "$profile_layer" "$profile package layer"
        mount_chroot
        chroot "$rootfs" apt-get update -qq
        install_package_list "$dev_list"
        save_layer "$target_layer"
    else
        if [ -f "$base_layer" ]; then
            unpack_tree "$base_layer" "base package layer"
            mount_chroot
        else
            unpack_tree "$cache" "Fenix base rootfs"
            mount_chroot
            stage_apt_sources
            chroot "$rootfs" apt-get update -qq
            # The debootstrap base comes from the release pocket only; bring it
            # up to the updates and security pockets staged just above.
            upgrade_rootfs
            install_package_list "$integration_root/profiles/base/packages.list"
            # initramfs-tools and zstd are in no profile list: they are what
            # this script itself needs further down to produce the initrd.
            chroot "$rootfs" apt-get install -y -qq --no-install-recommends \
                initramfs-tools zstd
            save_layer "$base_layer"
        fi
        # The profile lists only its additions; the base set
        # is already in the layer underneath. Refresh the indexes first,
        # because that layer may have been built weeks ago.
        chroot "$rootfs" apt-get update -qq
        upgrade_rootfs
        install_package_list "$integration_root/profiles/$profile/packages.list"
        save_layer "$profile_layer"
        if [ "$use_dev_layer" = 1 ]; then
            # Saved separately from the profile layer above, so the profile
            # layer stays exactly what a production build would restore.
            chroot "$rootfs" apt-get update -qq
            install_package_list "$dev_list"
            save_layer "$target_layer"
        fi
    fi
    printf '%s\n' "$target_key" >"$stamp"
fi

# After the layer, never inside it: this is the candidate being tested. The
# package hook that would build an initramfs here is left to run, but the
# image's own initrd is regenerated from scratch further down, so whatever it
# produces is discarded rather than trusted.
echo "installing kernel deb"
cp $kdebs "$rootfs/tmp/"
chroot "$rootfs" /bin/bash -e <<'KERNELDEB'
for deb in /tmp/*linux-image*.deb; do dpkg -i "$deb" || apt-get -f install -y -qq; done
KERNELDEB
rm -f "$rootfs"/tmp/*linux-image*.deb

# The Amlogic vendor userspace: libion, libge2d, the two encoder libraries, the
# JPEG encoder and the GStreamer plugins that use them, the ADLA user-mode
# driver for the NPU, and the ISP runtime of the camera. Binary vendor
# packages published by Khadas with no licence text, so the image does not
# carry them: it carries this list, built from the same sources.lock pins
# prepare-workspace has just verified, and chiron-vendor-fetch installs them
# on the board from where Khadas publishes them.
echo "writing the Amlogic vendor package list"
vendor_manifest="$rootfs/usr/local/lib/chiron/vendor-packages.manifest"
install -d "$(dirname "$vendor_manifest")"
{
    echo "# Written by assemble-output.sh from sources.lock. See chiron-vendor-fetch."
    for pin in ION GE2D JPEGENC AMVENC MULTIENC DRMBUFFERPOOL GSTREAMER_AML ADLA; do
        path_var=VENDOR_DEB_$pin
        sha_var=VENDOR_DEB_${pin}_SHA256
        echo "deb $VENDOR_DEB_BASE/${!path_var} ${!sha_var}"
    done
    echo "ispaml $ISPAML_URL $ISPAML_SHA256 $ISPAML_DEB $ISPAML_DEB_SHA256"
} >"$vendor_manifest"

kver=$(ls "$rootfs/lib/modules" | head -n 1)
test -n "$kver" || { echo "no kernel modules installed in rootfs" >&2; exit 1; }

# External W2 modules and their firmware, built from the locked W2 source
# against this exact kernel. A module set from another build fails at insmod
# with a vermagic or CRC mismatch.
w2_out="$workspace/artifacts/w2"
if [ -d "$w2_out" ]; then
    echo "installing W2 modules and firmware"
    install -D -m 0644 "$w2_out/w2_comm.ko" "$rootfs/lib/modules/$kver/extra/w2_comm.ko"
    install -D -m 0644 "$w2_out/w2.ko" "$rootfs/lib/modules/$kver/extra/w2.ko"
    install -D -m 0644 "$w2_out/wifi_w2_fw_sdio.bin" "$rootfs/usr/lib/firmware/wifi_w2_fw_sdio.bin"
    install -D -m 0644 "$w2_out/aml_wifi_rf.txt" "$rootfs/usr/lib/firmware/aml_wifi_rf.txt"
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no W2 modules in $w2_out; the image will have no Wi-Fi" >&2
fi

# The Himax touch module, built from the pinned Google source against this
# exact kernel. The digitiser is the only input device on this board, so a
# module that fails to load leaves the session with nothing to drive it.
himax_out="$workspace/artifacts/himax"
if [ -f "$himax_out/himax_mmi.ko" ]; then
    echo "installing the Himax touch module"
    install -D -m 0644 "$himax_out/himax_mmi.ko" \
        "$rootfs/lib/modules/$kver/extra/himax_mmi.ko"
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no Himax module in $himax_out; the image will have no touch" >&2
fi

# The out-of-tree audio codecs. Installed beside the Himax module and picked
# up by the same depmod below.
audio_out="$workspace/artifacts/audio"
if [ -d "$audio_out" ]; then
    echo "installing the audio codec modules"
    for module in snd-soc-es7210.ko snd-soc-fs2105.ko; do
        install -D -m 0644 "$audio_out/$module" \
            "$rootfs/lib/modules/$kver/extra/$module"
    done
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no audio modules in $audio_out; the image will have no sound" >&2
fi

# The accelerometer and the light sensor, installed the same way.
sensors_out="$workspace/artifacts/sensors"
if [ -d "$sensors_out" ]; then
    echo "installing the sensor modules"
    for module in sc7a20e.ko stk3b6x.ko; do
        install -D -m 0644 "$sensors_out/$module" \
            "$rootfs/lib/modules/$kver/extra/$module"
    done
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no sensor modules in $sensors_out; the panel will not rotate" >&2
fi

# The LED ring driver, installed the same way. The boot animation starts in
# probe, so nothing has to load it: the four controllers are in the board DT.
ledring_out="$workspace/artifacts/ledring"
if [ -d "$ledring_out" ]; then
    echo "installing the LED ring module"
    install -D -m 0644 "$ledring_out/aw21036.ko" \
        "$rootfs/lib/modules/$kver/extra/aw21036.ko"
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no LED ring module in $ledring_out; the ring will stay dark" >&2
fi

# The NPU driver, installed the same way. It autoloads from the adla node.
npu_out="$workspace/artifacts/npu"
if [ -d "$npu_out" ]; then
    echo "installing the NPU module"
    install -D -m 0644 "$npu_out/adla_core.ko" \
        "$rootfs/lib/modules/$kver/extra/adla_core.ko"
    chroot "$rootfs" depmod -a "$kver"
else
    echo "warning: no NPU module in $npu_out; there will be no /dev/adla0" >&2
fi

# The camera. The loopback module goes in beside the other out-of-tree modules;
# the two helpers go next to the ISP runtime, which is not built here and not
# shipped: it has no source, and chiron-vendor-fetch installs it on the board.
camera_out="$workspace/artifacts/camera"
if [ -f "$camera_out/v4l2loopback.ko" ]; then
    echo "installing the camera runtime, module and helpers"
    install -D -m 0644 "$camera_out/v4l2loopback.ko" \
        "$rootfs/lib/modules/$kver/extra/v4l2loopback.ko"
    chroot "$rootfs" depmod -a "$kver"
    # ispService and its four libraries are not installed here: they are
    # the vendor ISP runtime, which chiron-vendor-fetch puts on the board.
    for binary in chiron-camera-bridge-bin \
                  chiron-camera-privacy chiron-camera-bridge; do
        install -D -m 0755 "$camera_out/$binary" \
            "$rootfs/usr/lib/chiron-camera/$binary"
    done
    for payload in libchiron-ov13b10.so ov13b10-sdr.calib \
                   ov13b10-sdr.calib.sha256; do
        install -D -m 0644 "$camera_out/$payload" \
            "$rootfs/usr/lib/chiron-camera/$payload"
    done
else
    echo "warning: no camera artefacts in $camera_out; there will be no camera" >&2
fi

# The session tray indicators for the two privacy switches and the light
# sensor. In
# /usr/local/bin beside the other session programs rather than in
# /usr/lib/chiron-camera: this is a thing a person runs, not a helper the
# camera units start.
privacy_out="$workspace/artifacts/privacy"
if [ -f "$privacy_out/chiron-privacy" ]; then
    echo "installing the privacy and light indicators"
    install -D -m 0755 "$privacy_out/chiron-privacy" \
        "$rootfs/usr/local/bin/chiron-privacy"
else
    echo "warning: no chiron-privacy in $privacy_out; the tray will have no privacy indicators" >&2
fi

# The Bluetooth firmware loader. Graphical only: it is the profile that carries
# BlueZ, and without btattach the loader has nothing to hand the UART to.
bt_out="$workspace/artifacts/bt"
if [ -x "$bt_out/chiron-bt-init" ]; then
    echo "installing the Bluetooth firmware loader"
    install -D -m 0755 "$bt_out/chiron-bt-init" \
        "$rootfs/usr/local/sbin/chiron-bt-init"
else
    echo "warning: no chiron-bt-init in $bt_out; there will be no Bluetooth" >&2
fi

# The panfrost drain library, named in rootfs/graphical/etc/ld.so.preload.
# Installed here, before the overlays: once that file is in the tree every
# process started in the chroot loads the library, and a missing one makes the
# loader complain on each of them. So its absence is fatal, not a warning.
drain_out="$workspace/artifacts/panfrost-drain"
test -f "$drain_out/libpanfrost-drain.so" || {
    echo "no libpanfrost-drain.so in $drain_out" >&2
    exit 1
}
echo "installing the panfrost drain library"
install -D -m 0644 "$drain_out/libpanfrost-drain.so" \
    "$rootfs/usr/local/lib/chiron/libpanfrost-drain.so"

# The OP-TEE normal-world userspace. Graphical only, like the Bluetooth loader:
# what needs the secure world here is the video firmware the decoder waits on,
# and that decoder belongs to the desktop image.
tee_out="$workspace/artifacts/tee"
if [ -x "$tee_out/tee-supplicant" ]; then
    echo "installing the OP-TEE userspace"
    install -D -m 0755 "$tee_out/tee-supplicant" \
        "$rootfs/usr/local/bin/tee-supplicant"
    install -D -m 0755 "$tee_out/libteec.so.2.0.0" \
        "$rootfs/usr/local/lib/libteec.so.2.0.0"
    # The soname the clients were linked against, and the development name.
    ln -sfn libteec.so.2.0.0 "$rootfs/usr/local/lib/libteec.so.2"
    ln -sfn libteec.so.2 "$rootfs/usr/local/lib/libteec.so"
    for client in chiron_preload_fw; do
        install -D -m 0755 "$tee_out/$client" \
            "$rootfs/usr/local/bin/$client"
    done
    # /usr/local/lib is not on the default search path in a Debian-style
    # image; the clients carry an rpath, but tee-supplicant's own dlopen of
    # a TA plugin and anything else linked later needs the loader to know.
    install -D -m 0644 /dev/null "$rootfs/etc/ld.so.conf.d/chiron-tee.conf"
    echo "/usr/local/lib" > "$rootfs/etc/ld.so.conf.d/chiron-tee.conf"
    chroot "$rootfs" ldconfig
else
    echo "warning: no tee-supplicant in $tee_out; the secure world is unreachable" >&2
fi

# Public board files (loader, units). Device provisioning is never here: it is
# applied to the device overlay at deploy time.
# Two trees: rootfs/common is the board -- drivers, loaders, network --
# and rootfs/<profile> is the desktop on top of it: greetd, the Wayfire
# configuration, the first-boot account and everything the session drives.
overlays="$integration_root/rootfs/common"
if [ -d "$integration_root/rootfs/$profile" ]; then
    overlays="$overlays $integration_root/rootfs/$profile"
fi
if [ -d "$integration_root/rootfs/common" ]; then
    for overlay in $overlays; do
        echo "applying rootfs overlay: ${overlay#"$integration_root/"}"
        # Through tar with an explicit owner: the integration checkout can
        # carry foreign uid/gid (it is edited on another host) and those must
        # never end up in the image.
        tar -C "$overlay" --owner=0 --group=0 -cf - . | tar -C "$rootfs" -xf -
    done
    # The overlay drops icons into hicolor, and hicolor already carries a cache
    # built when the packages were unpacked. GTK trusts that cache over the
    # directory, so an icon added afterwards is simply never found: the tray
    # asks for a name and gets nothing back. Rebuild it once, here, after every
    # overlay has landed.
    if [ -x "$rootfs/usr/bin/gtk-update-icon-cache" ]; then
        chroot "$rootfs" gtk-update-icon-cache -q -f -t /usr/share/icons/hicolor
    fi
    # The board is a clock on a wall. Shipping it on UTC means the first thing
    # anyone sees is the wrong time by three hours, and the timezone list then
    # has to carry an Etc/UTC row that is none of the offsets it offers.
    chroot "$rootfs" ln -sf /usr/share/zoneinfo/Europe/Moscow /etc/localtime
    printf 'Europe/Moscow\n' >"$rootfs/etc/timezone"

    # Two identities that must not be the same on every board, and would be if
    # they were left as the rootfs build left them.
    #
    # The host keys first. They come from the base rootfs, so every copy of a
    # published image would answer SSH with the same key: anyone holding the
    # image could impersonate any board on the network, and the warning that
    # normally catches that never fires, because the key really does match.
    # Removed here; chiron-firstboot writes a fresh set on the first boot.
    # Not left to openssh's own sshd-keygen.service, which never runs on this
    # image: its ConditionFirstBoot=yes is never met, because systemd flags a
    # first boot only when it initialises the machine id itself and on this
    # squashfs root under an overlay it leaves /run/systemd/first-boot absent.
    rm -f "$rootfs"/etc/ssh/ssh_host_*
    # Then the machine id, which is two files. Emptying /etc/machine-id alone
    # is not enough and fails quietly in a way that matters: when it is empty,
    # systemd adopts /var/lib/dbus/machine-id if that still holds a value, so
    # every board kept the build's id -- and, because systemd had not had to
    # generate one, it did not consider the boot a first boot. That switched
    # off sshd-keygen.service, whose ConditionFirstBoot=yes is the only thing
    # that writes the host keys removed above, so sshd had no keys, ssh.service
    # failed its config test and ssh.socket went down with it: the board came
    # up refusing connections on port 22. The dbus copy becomes the symlink
    # Ubuntu ships, so there is one file, it is empty, and the first boot is a
    # first boot.
    : >"$rootfs/etc/machine-id"
    ln -sfn ../../../etc/machine-id "$rootfs/var/lib/dbus/machine-id"
    # And the resolver. mount_chroot copies the builder's own /etc/resolv.conf
    # in so apt can resolve names inside the chroot, and nothing put it back:
    # the image then shipped a plain file carrying the build network's search
    # domain, applied on every board that boots it. systemd-resolved is enabled
    # here, so the file belongs where Ubuntu keeps it -- a symlink to the stub
    # resolved writes at runtime. It dangles until resolved starts, which is
    # what stock Ubuntu does too.
    ln -sfn ../run/systemd/resolve/stub-resolv.conf "$rootfs/etc/resolv.conf"

    # USB. amlogic-usb.ko is what binds the CRG controllers the board
    # describes; it is loaded from its own unit, and without it there is no
    # /sys/bus/usb/devices at all.
    units="w2-load.service chiron-wifi-watchdog.service"
    units="$units chiron-usb-load.service"
    # The CPU frequency policies boot on schedutil; this puts them on
    # performance. The packaged cpupower-gui.service cannot do it -- see
    # /usr/local/sbin/chiron-cpufreq -- and is turned off further down.
    units="$units chiron-cpufreq.service"
    # Swap in compressed RAM. The root is a squashfs read through a loop
    # device, so evicting a file page costs a re-read and a decompression to
    # get it back; with no swap at all the kernel had nowhere to put anonymous
    # memory but the file cache, and the board went into page-cache thrashing
    # under a browser -- processes queueing in squashfs_decompress() with PID 1
    # and NetworkManager among them, so the session and SSH stalled together.
    units="$units chiron-swap.service"
    # The other half of the same problem: 1624 MiB of this 3 GiB board sits in
    # reusable CMA pools that the vendor aml_cma driver refuses to lend to the
    # page cache until swap is already 30% used, which on this appliance never
    # happens. See /usr/local/sbin/chiron-cma for the measurement and for why
    # the reserved-memory layout makes it safe to lift.
    units="$units chiron-cma.service"
    # Crash evidence: after every boot, pack what the previous boot's panic
    # left in ramoops onto the desktop as crash_dump_<date>_<time>.tar.gz.
    # In common, not graphical: on a profile with no desktop account the
    # archive lands on /root/Desktop instead.
    units="$units chiron-evidence.service"
    # The session: the account is created on the first boot rather than
    # baked into the image, and greetd opens the logind session that gives
    # the compositor its seat and DRM master.
    units="$units chiron-firstboot.service greetd.service"
    # Panel rotation follows the hinge, not the login: it is a system unit
    # that reaches the session's compositor through its Wayland socket, and
    # chiron-firstboot points it at the runtime directory of the account it
    # has just created.
    units="$units chiron-autorotate.service"
    # The ring's handoff is triggered by the session's readiness marker,
    # so what is enabled is the path unit, not the service.
    units="$units chiron-ledring-ready.path"
    # Bluetooth: the loader runs before bluetoothd, which the drop-in in
    # bluetooth.service.d orders against it.
    units="$units chiron-bluetooth.service"
    # The secure world: the supplicant has to be up before anything asks
    # the TEE for the video firmware.
    units="$units chiron-tee-supplicant.service"
    # The panel backlight: the udev rule sets the group on the device,
    # this fixes up the attribute itself, which udev does not reach.
    units="$units chiron-backlight-permissions.service"
    # The read-only Android volumes the file manager offers.
    units="$units chiron-android-volumes.service"
    # The one-time import of the vendor firmware this image does not carry.
    # It reboots the board once it has the files, and is a no-op after that.
    units="$units chiron-firmware-import.service"
    # The Amlogic userspace, fetched from its publisher once a network is
    # up; the NetworkManager dispatcher starts it again on every new
    # connection until it has succeeded.
    units="$units chiron-vendor-fetch.service"
    # The touchscreen. himax_mmi has no module alias at all, so nothing
    # autoloads it; it is loaded from its own late unit, after the SPI
    # controller has instantiated the child device.
    # Its firmware is one of the files chiron-firmware-import brings in.
    units="$units himax-load.service"
    # The Zigbee coordinator's power-on sequence.
    units="$units zbhci-host.service"
    # The hardware video path: the secure microcode first, then the
    # decoder and encoder modules that cannot stream without it.
    units="$units chiron-video-firmware.service chiron-codec-load.service"
    # The audio stack. Explicit and ordered because the dummy codec the
    # first dai-link needs has no module alias for udev to match.
    units="$units chiron-audio-load.service"
    # The camera. Keep the modules, public bridge and privacy watcher
    # resident, but let the bridge start/stop the vendor ISP only while a
    # client is actually capturing.
    units="$units chiron-camera-modules.service"
    units="$units chiron-camera-bridge.service chiron-camera-privacy.service"
    # Reused rootfs trees may retain this link from an older build where
    # the ISP was permanently enabled. Remove it explicitly every time.
    chroot "$rootfs" systemctl disable chiron-camera.service >/dev/null 2>&1 || {
        echo "could not disable chiron-camera.service" >&2
        exit 1
    }
    for unit in $units; do
        chroot "$rootfs" systemctl enable "$unit" >/dev/null 2>&1 || {
            echo "could not enable $unit" >&2
            exit 1
        }
    done
    # cpupower-gui ships this enabled by preset and it fails on every boot:
    # `cpupower-gui config` builds a GTK window, so it dies on "cannot open
    # display" from multi-user.target, and the system stays degraded. The
    # window application itself is untouched and stays in the menu.
    for unit in cpupower-gui.service; do
        chroot "$rootfs" systemctl disable "$unit" >/dev/null 2>&1 || {
            echo "could not disable $unit" >&2
            exit 1
        }
    done

    # SSH is installed, configured and off. This image publishes an account
    # and its password, so a board answering port 22 the moment it is
    # plugged in is one that anyone on that network can log into before its
    # owner has touched it. It is turned on by a desktop shortcut, once,
    # and stays on from then; see usr/local/bin/chiron-ssh-enable.
    #
    # ssh.socket is the whole switch on this release: ssh.service is not in
    # multi-user.target and is started by the socket. Disabled rather than
    # masked, so that turning it on is an ordinary enable.
    for unit in ssh.socket; do
        chroot "$rootfs" systemctl disable "$unit" >/dev/null 2>&1 || {
            echo "could not disable $unit" >&2
            exit 1
        }
    done

    # The sudo rule that shortcut relies on. git records only the execute
    # bit, so the mode it lands with is whatever the checkout had; sudo
    # ignores a file in sudoers.d that anyone but its owner can write, and
    # a rule that is silently ignored means a password prompt on a
    # touchscreen. Checked as well as fixed, because a syntax error in
    # sudoers.d does not disable one rule -- it disables sudo.
    sudo_rule="$rootfs/etc/sudoers.d/50-chiron-ssh"
    if [ ! -f "$sudo_rule" ]; then
        echo "the ssh sudo rule is missing: $sudo_rule" >&2
        exit 1
    fi
    chown 0:0 "$sudo_rule"
    chmod 0440 "$sudo_rule"
    chroot "$rootfs" visudo -cqf /etc/sudoers.d/50-chiron-ssh || {
        echo "the ssh sudo rule does not parse" >&2
        exit 1
    }

    # The evidence shortcut's rule, same file discipline: mode fixed and
    # syntax checked, because a sudoers.d file sudo declines to read does
    # not disable one rule -- it disables sudo.
    sudo_rule="$rootfs/etc/sudoers.d/50-chiron-evidence"
    if [ ! -f "$sudo_rule" ]; then
        echo "the evidence sudo rule is missing: $sudo_rule" >&2
        exit 1
    fi
    chown 0:0 "$sudo_rule"
    chmod 0440 "$sudo_rule"
    chroot "$rootfs" visudo -cqf /etc/sudoers.d/50-chiron-evidence || {
        echo "the evidence sudo rule does not parse" >&2
        exit 1
    }
    # Unattended apt is wrong for an appliance whose root is read-only
    # squashfs: nothing it downloads can be installed into the image, and the
    # attempt is expensive in exactly the way this board cannot afford -- an
    # apt run pulled the squashfs page cache out from under everything else and
    # stalled the whole system. Packages come from the image build, never from
    # the running device. Masked, not disabled: the timers are pulled in by
    # apt's own units, so disabling alone does not keep them away.
    apt_units="apt-daily.timer apt-daily-upgrade.timer"
    apt_units="$apt_units apt-daily.service apt-daily-upgrade.service"
    for unit in $apt_units; do
        chroot "$rootfs" systemctl mask "$unit" >/dev/null 2>&1 || {
            echo "could not mask $unit" >&2
            exit 1
        }
    done
    # User units. The account does not exist yet -- chiron-firstboot
    # creates it on the first boot -- so these cannot be enabled per user
    # here; --global writes into /etc/systemd/user/default.target.wants,
    # which every user session picks up whenever it is first started.
    for unit in chiron-audio-firstboot.service; do
        chroot "$rootfs" systemctl --global enable "$unit" >/dev/null 2>&1 || {
            echo "could not enable the user unit $unit" >&2
            exit 1
        }
    done
    # The Android volumes are published to the desktop through fstab, so
    # that an ordinary GVfs volume item mounts and unmounts them. Kept in
    # its own delimited block and rewritten from scratch, so that a rebuild
    # over a reused rootfs tree cannot accumulate copies of it.
    android_fstab="$rootfs/etc/chiron/android-volumes.fstab"
    if [ -f "$android_fstab" ]; then
        echo "publishing the Android volumes through fstab"
        install -d -m 755 "$rootfs"/media/Android-system \
            "$rootfs"/media/Android-system_ext \
            "$rootfs"/media/Android-vendor \
            "$rootfs"/media/Android-product \
            "$rootfs"/media/Android-odm
        sed -i '/^# BEGIN CHIRON ANDROID VOLUMES$/,/^# END CHIRON ANDROID VOLUMES$/d' \
            "$rootfs/etc/fstab"
        {
            printf '\n# BEGIN CHIRON ANDROID VOLUMES\n'
            sed '/^[[:space:]]*#/d;/^[[:space:]]*$/d' "$android_fstab"
            printf '# END CHIRON ANDROID VOLUMES\n'
        } >> "$rootfs/etc/fstab"
    fi

    # Polkit 127 prefers a socket-activated helper using pidfd. The board's
    # vendor kernel has no pidfd support, so every graphical authorization
    # otherwise opens a dialog, loses its helper and immediately opens a
    # replacement. Polkit retains its setuid-helper fallback: make that
    # fallback explicit and keep the incompatible socket off.
    polkit_helper="$rootfs/usr/lib/polkit-1/polkit-agent-helper-1"
    if [ ! -f "$polkit_helper" ]; then
        echo "polkit agent helper is missing: $polkit_helper" >&2
        exit 1
    fi
    chown 0:0 "$polkit_helper"
    chmod 4755 "$polkit_helper"
    chroot "$rootfs" systemctl disable polkit-agent-helper.socket >/dev/null 2>&1 || {
        echo "could not disable polkit-agent-helper.socket" >&2
        exit 1
    }
    chroot "$rootfs" systemctl mask polkit-agent-helper.socket >/dev/null 2>&1 || {
        echo "could not mask polkit-agent-helper.socket" >&2
        exit 1
    }

    chroot "$rootfs" systemctl set-default graphical.target >/dev/null 2>&1 || {
        echo "could not set the graphical default target" >&2
        exit 1
    }
fi

# One image name per profile, so several can sit side by side in <chiron_dir>
# and the boot menu picks between them with chiron_image=. The initrd still
# accepts the old rootfs.sqfs, so a board deployed before the split keeps
# booting.
sqfs_name="$profile.sqfs"

echo "building squashfs (zstd)"
mksquashfs "$rootfs" "$out/$sqfs_name" \
    -comp zstd -Xcompression-level 19 -b 1M \
    -processors "$(nproc)" \
    -noappend -no-progress \
    -e boot -e proc -e sys -e dev -e run -e tmp -e mnt -e media

echo "building initrd with overlay mountroot"
# Created from scratch rather than updated. The kernel package is now installed
# into a tree that already has initramfs-tools, so its hook leaves an initrd
# behind; regenerating unconditionally keeps the result a function of this
# workspace alone, and not of whatever a restored package layer happened to
# carry.
rm -f "$rootfs/boot/initrd.img-$kver"
chroot "$rootfs" update-initramfs -c -k "$kver" >/dev/null
initrd_raw="$rootfs/boot/initrd.img-$kver"
test -f "$initrd_raw" || { echo "initramfs-tools produced no initrd" >&2; exit 1; }

initrd_work="$workspace/initrd-$profile"
rm -rf "$initrd_work"
mkdir -p "$initrd_work/initramfs" "$initrd_work/initramfs/lower"     "$initrd_work/initramfs/mnt"
# The overlay mountroot mounts the boot partition on /mnt and the squashfs on
# /lower. Neither is guaranteed to exist in the generated initramfs, and a
# missing /mnt makes every mount in the partition scan fail, which looks
# exactly like a missing rootfs.sqfs. Create both in the repacked archive.
# initramfs-tools picks zstd only when the binary exists at generation time,
# so the produced initrd may be gzip or zstd depending on when it ran.
decompress_initrd() {
    case "$(od -An -tx1 -N4 "$1" | tr -d ' \n')" in
        28b52ffd) zstd -dc "$1" ;;
        *) gzip -dc "$1" ;;
    esac
}
(cd "$initrd_work/initramfs" && decompress_initrd "$initrd_raw" | cpio -idm --quiet)
mkdir -p "$initrd_work/initramfs/scripts"
cat "$here/mountroot-local" >>"$initrd_work/initramfs/scripts/local"
(cd "$initrd_work/initramfs" \
    && find . -print0 | cpio --null -o -H newc --quiet \
    | zstd -T0 -19 -q >"$initrd_work/initrd.cpio.zst")
mkimage -A arm64 -O linux -T ramdisk -C none -n "Chiron initrd" \
    -d "$initrd_work/initrd.cpio.zst" "$out/initrd.img" >/dev/null

# Both come out of the rootfs, not out of the build tree. They are then the
# same build as the modules installed beside them, which is the invariant that
# matters: a kernel from one build with modules from another loses every
# vendor module to a vermagic mismatch, and it has cost a cycle here already.
# The build tree is not a reliable source either -- bindeb-pkg cleans it, and
# the common_drivers dts output directory is gone by the time this runs.
echo "collecting boot files"
cp "$rootfs/boot/vmlinuz-$kver" "$out/Image"
dtb=$(ls "$rootfs"/usr/lib/linux-image-*/chiron.dtb 2>/dev/null | head -n 1)
test -n "$dtb" || { echo "the kernel package shipped no chiron.dtb" >&2; exit 1; }
cp "$dtb" "$out/chiron.dtb"
test -f "$out/Image" && test -f "$out/chiron.dtb"

printf '%s\n' \
    "PROFILE=$profile" \
    "INTEGRATION_COMMIT=$(git -C "$integration_root" rev-parse HEAD)" \
    "KERNEL_VERSION=$kver" \
    "ROOTFS_CACHE=$(basename "$cache")" \
    "ROOTFS_LAYER=$(basename "$target_layer")" \
    "SQFS_NAME=$sqfs_name" \
    "SQFS=$(sha256sum "$out/$sqfs_name" | cut -d' ' -f1)" \
    "INITRD=$(sha256sum "$out/initrd.img" | cut -d' ' -f1)" \
    "IMAGE=$(sha256sum "$out/Image" | cut -d' ' -f1)" \
    "DTB=$(sha256sum "$out/chiron.dtb" | cut -d' ' -f1)" \
    >"$out/manifest.env"

cleanup
trap - EXIT
echo "Chiron $profile output ready: $out"
ls -lh "$out"
