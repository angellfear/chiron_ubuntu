# Out-of-tree LED driver

| Driver | Part | Bus | Role |
|---|---|---|---|
| `aw21036` | Awinic AW21036 ×4 | i2c3 0x34-0x37 | the 48-pixel RGB ring |

Carried unchanged, built by
`scripts/builder/build-ledring.sh`.

The driver renders one shared animation across all four controllers from the
moment it probes, so the ring is lit before any userspace exists. The first
ordinary LED-class write claims the ring for userspace and stops boot-frame
scheduling on every controller at once; `chiron-ledring.service`, started by
`chiron-ledring-ready.path` when the session marks itself ready, is what makes
that write, clears all 144 channels and gives the `video` group access.

Userspace addresses pixels by name -- `/sys/class/leds/chiron:<colour>:ring<N>`
for N in 0..47 -- and never sees an I2C address. The physical controller order
is 0x37, 0x36, 0x34, 0x35 and the channels on 0x34 are scrambled relative to
the rest; both facts are in the board DT's label/reg pairs, not in the driver.

What is deliberately not carried: `load-chiron-ledring.sh` and
`chiron-ledring-load.service`, which insmod'ed the module by absolute path out
of the Android `/cache` partition. Here it is an ordinary module in
`/lib/modules/<kver>/extra` and the controllers are in the board DT, so it
loads on its own. `boot_animation` defaults to true, so nothing has to pass it.
