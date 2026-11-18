#!/bin/sh
# usb-host-confirm.sh - "host mode works, keep it". Run it during a
# usb-host-trial.sh trial (on the console's own keyboard/terminal, or over SSH
# on Wi-Fi). Without it the trial switches back to device mode by itself.
# Host mode then stays until usb-host-off.sh or power-off (every start is
# device mode with the USB rescue port again).
STATE=${NZ_USBHOST_STATE:-/run/nzeus-usbhost}   # (NZ_*: PC test only)
test -f "$STATE/usb-host-trial.sh" || { echo 'USBHOST_NOT_STARTED: run usb-host-trial.sh first.'; exit 1; }
sh "$STATE/usb-host-trial.sh" --confirm || exit 1
sh "$STATE/usb-host-trial.sh" --status
