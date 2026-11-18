#!/bin/sh
# usbnet-down.sh - send this with rescue-command.py to leave network mode: the
# console swaps back to the plain rescue gadget (NZEUS-RAM-RESCUE-01, COM9).
# The serial port this runs on disappears 3 s after the reply.
STATE=${NZ_USBNET_STATE:-/tmp/nzeus-usbnet}   # (NZ_*: PC test only)
test -f "$STATE/usbnet-up.sh" || { echo 'USBNET_NOT_STARTED: nothing to switch off.'; exit 0; }
sh "$STATE/usbnet-up.sh" --down
