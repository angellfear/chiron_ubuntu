# Chiron display and privacy controller

Native C/GTK session controller for Chiron. It contains the privacy-switch
indicator, ambient-light brightness policy, touch-oriented display settings,
editable lux-to-brightness profiles and the fullscreen clock overlay.

One tray item carries both hardware switches. It is always the same pair of
glyphs -- a webcam and a microphone -- each drawn in the panel's ink while its
line is live and struck through in red once it is cut, which covers all four
combinations without the icon ever changing shape. It is the only item this
process exports, so it lives on the main bus connection and needs no child
processes. Activating it opens the settings window.

The settings window has two tabs. Brightness holds the sensor switch, the
profile, the editable curve and the limits. Screen and clock holds the idle
timeout and action, the system timezone as a single selector of one entry per
hour of UTC offset, and the fullscreen clock: font and date language on one
strip, digit size, and night mode. Night mode is a band rather than a point --
the clock turns red below the first level and white again only above the
second, which keeps it from flickering on the room's own noise. It ships at 30
and 50 lx.

Every selector opens a modal list rather than a drop-down: a combo popup is the
width of its button and the height of its contents, which for twenty-four
timezones is a column of mouse-sized rows stranded in empty space. The list
window sizes itself to what it holds, centres on the settings window, and keeps
a scrollbar in view rather than one that only appears once a drag is already
under way.

The source package builds only this application. It does not build or modify
the kernel, root filesystem or operating-system image.

## Build on the ARM64 device

Install the compiler and headers once:

```sh
sudo apt-get install build-essential pkg-config \
  libayatana-appindicator3-dev libgtk-3-dev libgtk-layer-shell-dev
```

Then build:

```sh
make
```

Runtime packages are `libayatana-appindicator3-1`, `libgtk-3-0t64`,
`libgtk-layer-shell0`, `swayidle` and `wlopm`.

The executable is installed as `/usr/local/bin/chiron-privacy`. The companion
`chiron-idle` script from the source archive is installed as
`/usr/local/bin/chiron-idle` and is started by the Wayfire idle plugin.

User settings are stored in `~/.config/chiron/display.ini` and
`~/.config/chiron/power.conf`. No settings are written into the system image.
