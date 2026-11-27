#!/bin/sh
# flag-now.sh FLAGFILE [SECONDS] - stands in for fn-hold.py when NZEUS Deck
# switches to Devices mode: the owner already said "the PC is out" by choosing
# it in Settings > USB, so the flag is written at once.
: > "$1"
