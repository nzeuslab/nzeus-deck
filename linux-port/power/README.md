# Modern Linux: battery, charger, power button, power-off

## Measured (read-only, 2026-10-10)

- Battery/charger: see `../battery/README.md` (SAR ADC left running by the
  bootloader; level = 241*raw[ch1]/raw[ch0]; charger ch5 0 / ~138).
- Power button (usb-command-20261009T144012.txt, `input/key-watch`): three
  short taps pulled **L14** from 1 to 0 for about 0.2 s each; the console stayed
  on. L14 is an input (DIR bit 14 = 0), pin-mux GPIO.
- Power-off (stock side, `apps/power/nzeus-poweroff.c` and the stock
  `cubegm/powergpio`, disassembled): drive L14 low (OUT bit 14 cleared, then DIR
  bit 14 set), wait 4 s. Works on stock - but see below: on stock something
  else (presumably the AVP, which owns the `standby` service) reacts to it.

## Power-off: the real pin is L01 (hardware, 2026-10-10)

- **L14 only senses the button.** Modern Linux, AVP-free, power driver unbound,
  `pin-drive 15 14`: L14 driven low and read back 0 for all 15 s - the console
  stayed on. The kernel's earlier L14 power-off (3 s, then 8 s) also failed.
- Physical button alone (Linux dead): the power chip switches off after ~7 s.
- **L01 = the stock device tree's `power-pin`** (`panel { power-button = <0xe>;
  power-pin = <0x1>; }`). `pin-drive 5 1` (`l01-drive.sh`): USB link lost
  right after the pin went low, device gone from Windows, console OFF (I
  watched). L01 is otherwise an input reading high, pin-mux GPIO, unused.
- Logs: `../purelinux/session/logs/pure-update-20261010-172338.txt`.
- **Kernel fix proven 2026-10-10 19:30** (pure `e7105b8d…`): `poweroff-test.sh` switched the
  console fully off by itself (`../purelinux/session/logs/l01-fix-20261010-193004.txt`).

## Driver `nzeus-r36s-power.c` (build `gma-native-power`)

- Maps only the 8 ADC sample bytes at 0x188184d0, reads them, never writes.
  `/sys/class/power_supply/battery` (status, capacity as a rough straight line
  128..175, capacity_level, present, technology) and `.../charger` (online).
  Raw stock-scale levels in `/sys/bus/platform/devices/188184d0.power/stock_levels`.
- `power-gpios` = L14 active low: polled every 50 ms as KEY_POWER
  ("r36s-power-button") by the driver's own always-running work item (NOT the
  input poller, which only runs while a program has the device open - so
  hold-to-off also works with nothing running); held 3 s -> orderly power-off (forced, there is no
  /sbin/poweroff). L14 always stays an input.
- `poweroff-gpios` = L01 active low, requested AS-IS (probe never touches it):
  the power-off handler drives it low; if the power is still on 2 s later it
  lets go and logs it.
- `nz-poweroff --yes` (static tool): sync + reboot(RB_POWER_OFF).
- Tests: `power-test.sh` (read-only: sysfs values + key events),
  `poweroff-test.sh` (switches off via nz-poweroff).
- `../display/r36s-gma-avp-power.dts` = keys tree + one node;
  `verify-avprpc.py --variant power` checks it (mutation-tested) and that the
  GPIO users are exactly the keys + this node. Cloud-compiled at the pinned
  commit (symbols r36s_power_probe, r36s_power_off, r36s_button_poll).

## Brightness (in `../avp-rpc/nzeus-avp-rpc.c`, same build)
Standard backlight device `/sys/class/backlight/r36s-avp` (type firmware,
0..255; its start value is read from the PWM0 counter the AVP left, measured
23 on this console, without sending anything). A write sends
exactly what stock avp-proxy sends: RPC_OPEN "/dev/backlight", RPC_WRITE of
the 4-byte little-endian level, RPC_CLOSE. Not tied to fb blanking
(`controls_device` returns false) because our fb blank hands the screen back
to the firmware. 0 may switch the light off. Test: `bl-set.sh`.

## zram and SysRq (same build)
`CONFIG_ZRAM` (lzo-rle) + `ZSMALLOC`: compressed swap in RAM, switched on at
run time by `nz-zram SIZE_MB` (static tool: disksize, version-1 swap header on
/dev/zram0, swapon prio 100; the RAM BusyBox has no mkswap/swapon). Test:
`zram-test.sh` (32 MiB). `CONFIG_MAGIC_SYSRQ` only through
/proc/sysrq-trigger: serial SysRq disabled (a floating UART RX could otherwise
fire random commands) and `MAGIC_SYSRQ_DEFAULT_ENABLE=0x0` (no key combos).
The verifier pins the exact config differences (mutation-tested: serial SysRq
on is rejected).

## Battery calibration logger
`battlog-start.sh` starts a background loop (sleep 60, no busy wait) that
appends "uptime ref=.. battery=.. charger=.. cap=NN status" to /tmp/battlog.txt
(RAM, ~60 bytes/min). Unplug the charger and let it run down; `battlog-show.sh`
prints it. The stock-scale battery values over time give the real discharge
curve to replace the straight-line percentage (128..175) in the driver.
