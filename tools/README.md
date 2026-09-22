# Bench tools

Tools that are run by hand on the board or on a build host and are not part of
any image.

| Tool | What it is for |
|---|---|
| `sc7a20e-calibrate` | measures the accelerometer's `mount-matrix` from three reference positions |

`sc7a20e-calibrate` is the record of how the accelerometer's placement was
established. The answer it produced on this board is the identity, which is what
the DT node carries; keep the tool so the measurement can be repeated rather
than re-derived if the sensor is ever moved. Copy it to the board and run it
there -- it reads `/sys/bus/iio/devices` directly and needs nothing installed.
