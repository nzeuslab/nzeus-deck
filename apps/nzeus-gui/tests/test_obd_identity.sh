#!/bin/bash
# PC-only state-machine regression; no console access or external dependencies.
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
build="$HOME/r36s/build/obd-identity"
mkdir -p "$build"
gcc -O2 -Wall -Wextra -Werror -I"$src" -o "$build/test_obd_identity" \
    "$src/tests/test_obd_identity.c" "$src/obd.c" "$src/dtc_db.c" -lm
"$build/test_obd_identity"
