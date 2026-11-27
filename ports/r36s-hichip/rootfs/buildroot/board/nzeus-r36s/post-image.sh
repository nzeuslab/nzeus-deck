#!/bin/sh
# Buildroot post-image: write a small manifest next to the images
# (package list with installed sizes, totals, SHA-256 of rootfs.tar).
# Only this manifest is meant to be committed, never the images.
set -e
OUT="$BINARIES_DIR/nzeus-rootfs-manifest.txt"
LIST="$BUILD_DIR/packages-file-list.txt"
{
    echo "NZEUS R36S rootfs manifest"
    echo "buildroot: $BR2_VERSION"
    echo "rootfs.tar: $(stat -c %s "$BINARIES_DIR/rootfs.tar") bytes"
    echo "rootfs.tar sha256: $(sha256sum "$BINARIES_DIR/rootfs.tar" | cut -d' ' -f1)"
    echo "target dir: $(du -s --apparent-size -B1 "$TARGET_DIR" | cut -f1) bytes (apparent)"
    echo
    echo "installed bytes per package (files in target/, apparent size):"
    if test -r "$LIST"; then
        while IFS=, read -r pkg path; do
            f="$TARGET_DIR/${path#./}"
            if test -f "$f" && ! test -L "$f"; then
                echo "$pkg $(stat -c %s "$f")"
            fi
        done < "$LIST" | awk '{ s[$1] += $2 } END { for (p in s) printf "%10d  %s\n", s[p], p }' | sort -rn
    fi
    echo
    echo "package versions (from the build directory):"
    for d in "$BUILD_DIR"/*/; do basename "$d"; done | grep -v '^host-\|^buildroot-\|^toolchain\|^skeleton' | sort
} > "$OUT"
echo "Manifest: $OUT"
