#!/bin/sh
# nzeus-usbserial.sh - NZEUS Deck's USB-serial "plug and play" on the SD Debian.
# Stock (apps/usbserial): the old kernel had no udev and the adapter drivers
# were add-on modules, so this script loaded cdc-acm / ftdi_sio / ch341 /
# cp210x / pl2303 with insmod whenever a board was plugged in (the Arduino,
# Car and Probe apps run it every 3 s while they are open).
# The new kernel has those drivers BUILT IN, and devtmpfs makes /dev/ttyACM*
# and /dev/ttyUSB* by itself the moment a board is plugged in - so there is
# nothing to load. It prints nothing (= "nothing changed") and succeeds.
exit 0
