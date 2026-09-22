# Out-of-tree audio codec drivers

`common_drivers` carries the AUGE fabric and the Amlogic codecs, but not the
two codecs fitted on this board, so both are built as external modules against
the same kernel and `Module.symvers` (`scripts/builder/build-audio.sh`).

| Driver | Part | Role |
|---|---|---|
| `es7210` | Everest ES7210 | four-channel ADC carrying the amplifier hardware feedback back to the SoC, on TDM-C |
| `fs2105` | FourSemi FS2105 ×2 | the 2.1 speaker amplifiers on TDM-B; `fsm,dev-index` decides which is the woofer, not the I2C address |

Both are carried unchanged. The FS2105 driver
consumes the two factory register and DSP configuration files captured from
Android; they are firmware, not source, and are installed with the image.
