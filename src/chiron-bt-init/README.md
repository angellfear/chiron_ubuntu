# chiron-bt-init

The factory Android image uses the W2 controller as an H4 UART device on
`/dev/ttyS1`. Its vendor HAL first downloads `w2_bt_fw_uart.bin` with Amlogic
vendor HCI commands, then leaves the UART at 4 Mbit/s. It does **not** expose
Bluetooth through `w2_bt.ko`, `/dev/stpbt`, or a `/dev/vhci` bridge.

`chiron-bt-init` implements only that board-specific firmware sequence,
including the two-antenna RF register and the special `0xfef2` CPU-start write.
`chiron-bluetooth.service` runs a Bluetooth-only rfkill power cycle first,
because a SoC reset does not reset the separately powered W2 core, and then
hands a raw 4 Mbit/s UART to `btattach -N`. The standard Linux `btattach` owns
the UART afterwards and registers `hci0`; BlueZ is otherwise unmodified.

Carried unchanged. The firmware itself is not in this repository: the board
imports it from its own Android volumes on first boot, into
`/usr/lib/firmware/amlbt/`. Its first two little-endian words describe a
524288-byte ICCM region and a 131072-byte DCCM region.

## Headset call audio does not work, and is parked

A2DP plays. Handsfree does not. The profile registers and a call negotiates an
eSCO link -- status success, 60-byte packets both ways, CVSD, transmission
interval `0x0c` -- but not one synchronous packet ever arrives from the
controller, and its command path stalls the moment the link comes up. Measured
on a live call: 107 commands answered before the link and none of the ten sent
after it, while receive on the ACL link kept working for a further ten seconds.
The host wrote 6265 outbound synchronous packets of 48 bytes each into that
60-byte link, and read nothing back.

The controller offers no way to put synchronous data on the HCI transport. Its
supported-command bitmap ends at octet 28, so Enhanced Setup Synchronous
Connection and Enhanced Accept Synchronous Connection Request -- the only
standard commands carrying a data-path field -- are both absent, and the kernel
consequently accepts with the legacy form, which cannot express the choice.
There is no path around HCI either: the factory Android image declares
Bluetooth SCO device ports in its audio policy but has no matching mixer path,
no dai-link for the controller's PCM port, and a `hw_set_audio_state` that is an
eight-byte stub, and TDM-A is not clocked during a call. `transparent SCO data`
is missing from the LMP features, so mSBC is out and only CVSD is on the table.

The mainline firmware changes none of this. Loaded through the parked `-m` path
with reset `0x00000700` it attaches cleanly from a cold start and reports the
same features and the same supported commands, and a call reproduces the same
picture packet for packet; the mainline `hci_aml` driver does not touch
synchronous routing either. Headset call audio has never worked on this
board, so there is nothing here to carry.

What is left is the vendor register-write mechanism this loader already uses,
with the routing register unknown. Finding it means reversing the firmware, so
the question is parked rather than answered.
