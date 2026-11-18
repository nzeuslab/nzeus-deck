#!/bin/sh
# usb-host-off.sh - leave USB host mode now: Wi-Fi down, the USB port back to
# device mode, the USB rescue serial port (NZEUS-RAM-RESCUE-01, COM9) plugged in
# again - and, if /etc/nzeus/usbnet-at-boot exists, the USB network mode as at
# power-on (nzeus-usbnet-boot: serial + network once the PC sends packets, else
# COM9 stays). Both done by usb-host-trial.sh's revert. Runs in the background,
# so it finishes even when this runs over SSH on Wi-Fi (which goes away).
# Unplug the OTG adapter, then plug the PC in.
# Log: /data/nzeus/usbhost.log and the kernel log.
STATE=${NZ_USBHOST_STATE:-/run/nzeus-usbhost}   # (NZ_*: PC test only)
LIB=${NZ_USBHOST_LIB:-/usr/local/lib/nzeus/usbhost}
mkdir -p "$STATE" || exit 1
if test -f "$STATE/usb-host-trial.sh"; then
    sh "$STATE/usb-host-trial.sh" --off
else
    # host mode set by hand (no trial copy yet): use the installed script
    cp "$LIB/usb-host-trial.sh" "$STATE/usb-host-trial.sh" || exit 1
    sh "$STATE/usb-host-trial.sh" --off
fi
