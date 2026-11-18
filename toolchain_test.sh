#!/bin/bash
# Checks the MIPS toolchain and builds a tiny "hello" program for the console.
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
echo "== toolchain folder"; ls $T
[ -x $T/relocate-sdk.sh ] && (cd $T && ./relocate-sdk.sh > /dev/null && echo "relocated OK")
BIN=$(find $T -type f -name '*-gcc' -path '*bin*' 2>/dev/null | head -5)
echo "== compilers found:"; echo "$BIN"
