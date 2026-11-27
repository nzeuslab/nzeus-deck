#!/bin/sh
# usbnet-confirm.sh - send this with rescue-command.py AFTER usbnet-up.sh switched
# the console to "serial + network". It runs on the NEW serial port, so it working
# at all proves the lifeline still works; it then tells usbnet-up.sh to stay in
# network mode (otherwise it switches back to the plain rescue port by itself).
STATE=${NZ_USBNET_STATE:-/tmp/nzeus-usbnet}   # (NZ_*: PC test only)
test -f "$STATE/usbnet-up.sh" || { echo 'USBNET_NOT_STARTED: run usbnet-up.sh first.'; exit 1; }
sh "$STATE/usbnet-up.sh" --confirm || exit 1
sh "$STATE/usbnet-up.sh" --status
