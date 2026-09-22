# Out-of-tree sensor drivers

Two parts on this board have no in-tree driver that will bind to them, so both
are built as external modules against the same kernel and `Module.symvers`
(`scripts/builder/build-sensors.sh`).

| Driver | Part | Bus | Role |
|---|---|---|---|
| `sc7a20e` | Silan SC7A20E | i2c1 0x19 | 3-axis accelerometer; the panel rotation is decided from it |
| `stk3b6x` | Sensortek STK3B6X | i2c4 0x48 | ambient light |

Both are carried unchanged.

The accelerometer is register-compatible with ST's LIS3DH but answers WHO_AM_I
with 0x11, which `st_accel` rejects; giving it an entry there would mean
duplicating the LIS3DH settings struct and building four more external modules.
The driver here is self-contained against the built-in IIO core and reports
polled x/y/z with a scale, which is all the rotation policy needs. It reads the
standard `mount-matrix` property -- the vendor node describes placement with its
own `orient` index instead, which nothing in mainline consumes.

The light sensor exposes the part the way the Android vendor driver did: an
input device named `als` reporting the 16-bit ALS register through `ABS_X`,
polled at 2 Hz while something has the device open. The vendor's proximity and
colour paths are not implemented; nothing on this board consumes them.
