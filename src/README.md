# Userspace sources built for the board

C programs that are built for the image from this checkout, in the temporary
workspace, the same way the out-of-tree kernel modules are.

| Source | Built by | Installed as |
|---|---|---|
| `chiron-bt-init` | `scripts/builder/build-bt.sh` | `/usr/local/sbin/chiron-bt-init` |
| `chiron-privacy` | `scripts/builder/build-privacy.sh` | `/usr/local/bin/chiron-privacy` |
| `panfrost-drain` | `scripts/builder/build-panfrost-drain.sh` | `/usr/local/lib/chiron/libpanfrost-drain.so`, via `/etc/ld.so.preload` |

Most of it is cross-built with the Fenix toolchain, which is enough for a
program that links libc and nothing else. `chiron-privacy` is the exception: it
links GTK and libayatana-appindicator, and the headers for those exist only on
the target side of the ABI, so it compiles in a small cached arm64 chroot
instead. Its build script says why at length.

Nothing here is a rebuild of a distribution package. Where the archive ships a
working package, the image uses it.
