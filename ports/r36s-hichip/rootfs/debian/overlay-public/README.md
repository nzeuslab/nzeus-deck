# overlay-public: the copy-and-go image's differences

`build-debian.sh WORK_DIR --reuse --public-fatroot` copies these files over
`overlay/` for the public image (docs/specs/2026-11-18-copy-and-go-installer-design.md).
There, the boot RAM disk (ports/r36s-hichip/fatroot) has already loop-mounted
`nzeus/nzroot.img` as `/` (loop1, after e2fsck), `nzeus/nzdata.img` at `/data`
(loop2) and the card's FAT32 at `/sdcard` (loop0, read-write on purpose).
