# Security

Found a way into a console running NZEUS Deck, or a way to read its owner's
data? Please report it **privately**: on this repository's GitHub page, go to
**Security > Report a vulnerability**. Please don't open a public issue for it.

Past review: [docs/SECURITY-REVIEW-2026-11.md](docs/SECURITY-REVIEW-2026-11.md)
(what was checked, what was found and fixed, and how to re-run the tests).

These are by design:
- **USB shell:** a USB cable to the console gives a root shell without a
  password (`nzeus-usb-shell`, and the RAM-only rescue shell). It's how you
  recover a console. Anyone holding your console and a cable has full control.
- **No secure boot:** the factory bootloader starts any correctly wrapped
  kernel from the SD card (see `docs/WRITEUP.md`). That's what makes this
  project possible.
- **App updates** are only installed when they carry a valid signature from
  your own key (`tools/publish_update.py`).
