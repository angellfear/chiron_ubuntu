# Chiron OP-TEE userspace

The secure world is reached through the Amlogic OP-TEE backend
(`CONFIG_AMLOGIC_TEE=m`, patched by queue entries 0012 and 0013) and the
`/optee` node the board file extends with `amlogic,prefer-reserved-shm`.

Android's `tee-supplicant` and `libteec` are bionic-linked and cannot run here,
so `scripts/builder/build-tee.sh` builds the same protocol against glibc from
the pinned `optee_client`, and cross-builds the Chiron client below.

| Program | What it does |
|---|---|
| `chiron_preload_fw` | pushes the Amlogic video firmware package into the secure world; a glibc replacement for Android's `/vendor/bin/tee_preload_fw` |


`chiron_preload_fw` is what the hardware video decoder waits on: until it has
run, the secure world has no microcode to hand out and `tee_load_video_fw()`
answers `TEEC_ERROR_BAD_STATE`, leaving the T7 decoder's processor unstarted.
