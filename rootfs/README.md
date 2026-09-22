# Board files installed into images

Layered the same way `profiles/*/packages.list` is:

- `common/` is the board. It carries what the hardware needs underneath the
  desktop: the W2 Wi-Fi loader and its unit, the Wi-Fi watchdog, the
  module order the Amlogic DRM stack requires, and the crash-evidence
  collector (`chiron-evidence`) that packs the previous boot's ramoops
  records into a `crash_dump_<date>_<time>.tar.gz` after a panic.
- `graphical/` is the Wayland session on top: greetd, the Wayfire and
  wf-shell configuration, the session wrapper, and the first-boot account.

Public board integration only. Device provisioning — the root serial console,
SSH keys, Wi-Fi credentials — is never here; it is applied to the device
overlay at deploy time and never enters an image.
