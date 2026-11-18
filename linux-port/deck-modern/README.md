# NZEUS Deck helpers for SD Debian

The programs NZEUS Deck runs from `NZ_DIR/bin` (installed to
`/usr/local/lib/nzeus/deck-bin`; `nzeus-deck` links `NZ_DIR/bin` there), built
on Debian's own programs. `required-files.txt` is the explicit inventory used
by both the system image builder and `tools/publish_update.py`. A missing
helper is an error. The image records helper hashes in `deck-bin/SHA256SUMS`.

Also kept here (each goes to the image and can go through the signed updater):

| Folder / file | What | On the console |
| --- | --- | --- |
| `wifi-join-core.sh` | the Wi-Fi joiner (wpa_supplicant); `wifi-join.sh` runs it with Debian's programs | `deck-bin/wifi-join-core.sh` |
| `bt/` | `nzeus-bt`, the Bluetooth program: source, tests, the checked static build (`bt/README.md`) | `deck-bin/nzeus-bt.bin` |
| `python-examples/` | the Python app's examples | `/usr/local/lib/nzeus/python-examples/` |
| `build/emoji/` (not in git) | emoji pictures, made by `python tools/make_emoji.py` (Twemoji, CC-BY 4.0) | image: `/usr/local/lib/nzeus/emoji` + link `NZ_DIR/emoji`; updater (`--emoji`): `NZ_DIR/emoji` |

The Python app uses full Python 3. The Web downloader uses curl with certificate
checks and a bounded download. USB serial drivers belong to the kernel; the
helper does not load stock modules. Arduino/STM32 upload tools are invoked only
when the owner explicitly starts an upload; PC tests never contact a board.

Claude/WhatsApp connections use ordinary SSH to the owner's configured PC/phone.
No account keys, addresses or login sessions belong in this folder or GitHub.
The gated **Claude access** reverse tunnel is not ported: the Deck's Claude
app keeps that switch off and says so (no helper script is involved, nothing
replaces it with a root service). Direct SSH to the console is a separate,
owner-configured service.

## Reading PDF reports

In Files, open `nzeus/car`, select a PDF and press A. The PDF Reader shows one
page at a time; allow a few seconds for rendering. L/R changes page, A cycles
page fit / width fit / enlarged width, the D-pad scrolls, X rotates the page, and B returns to Files.
English and Arabic preserve the layout in the saved PDF. Damaged, encrypted or
excessively complex files show an error; password entry is not implemented.

The SD Debian needs `poppler-utils` (pdfinfo/pdftoppm) and `timeout`. The Debian
image's packages.txt includes Poppler for future builds. On an existing card,
install that dependency separately through Debian apt; the signed app updater
only manages the app and its helper inventory, not Debian packages. The reader
uses a private temporary page cache and removes it when closed. It never
rewrites the source PDF. See `../../reports/continuation/pdf-reader-20261012.md`
for current PC and hardware evidence.

## Vehicle discovery and support guides

In Car, press X and choose **Features / vehicle packs**. Press A for vehicle
selection, guide downloads, installed support or saving vehicle details. A
manual make/model/year/market selection is a search filter, not proof of equipment
or compatibility. The current supported pack kind is **owner-guide** only: it
explains settings in the car's own menus. No app-controlled feature toggle,
coding definition or write/restore controller is present.

Completed non-demo scans automatically save private `Vehicle-discovery-*.json`
files and `Last-vehicle-discovery.json` in `nzeus/car` on the persistent SD data
area. They contain VIN, adapter/protocol, responding module addresses/bus and
optional positive F187/F188/F189/F191 identifiers. Failed identifier reads remain
unknown. This is identification evidence, not an original-configuration backup.
It is not uploaded. Native car identifier capture still needs a field test.

`nzeus-car-support` needs Python 3, OpenSSL and timeout; the future image inventory
includes the helper and dependencies. CLI actions are `view`, `refresh`,
`select MAKE MODEL YEAR MARKET` and `import BUNDLE`, with optional `--root PATH`.
The default store is `/data/nzeus/deck/nzeus/car/support`. A configured `server`
file must contain an HTTPS base URL; normal CA validation and same-origin
redirect checks apply. No production server is configured: Download guide packs
reports that honestly and retains offline guides. VIN is never sent in requests.

Catalogs use a dedicated RSA/SHA-256 public key embedded in the helper, separately
from app-update authentication. The manager verifies signatures, hashes, sizes,
expiry, schema and version history before atomically switching generations.
It rejects executable/coding content. `tools/publish_car_support.py` builds the
signed bundle using an external private key; the key must not enter Git or the
console. The installed offline guide covers conditional **2016 US Silverado**
owner-menu settings only. GCC/other years are not covered by that guide.

Run `python3 linux-port/deck-modern/tests/test_car_support.py` on Linux/WSL for
trust, transport and installation tests. See
`../../reports/continuation/car-support-20261012.md` for installed evidence and
remaining work.

## Updates and recovery

`tools/publish_update.py` produces an HMAC-signed `system modern` manifest
(`--modern` is still accepted and changes nothing). The installer accepts only
`nzeus-modern`, `deck-bin/NAME` (mode 755), `python-examples/NAME.py` and
`emoji/NAME` (mode 644; emoji only when published with `--emoji`, about 1400
small files, so a check takes longer then). It verifies downloads before
replacing managed files, keeps originals, and marks the transaction pending.
It restarts the Deck application, not the kernel. A failed rollback retains
its pending marker and backups for recovery.

An atomic `/run/nzeus-update.lock` excludes simultaneous checks, installs,
rollbacks and restart watchers. Normal exit releases it; a full reboot clears
the volatile lock. After a forced process kill, verify the owner recorded in
`pid` has stopped before removing an abandoned lock. This is separate from the
persistent pending marker: there is no automatic power-loss recovery service
yet. Hardware update/restart validation is still required.
Do not deploy by replacing the whole Linux area on the existing card:
that would erase `/data`.

## PC validation

Run `python3 linux-port/deck-modern/tests/test_helpers.py` on Linux/WSL. Tests use
temporary fake targets, public fixture keys and a loopback HTTP server. They
cover helper syntax/inventory, signed install/rollback, bad signatures/paths,
wrong architecture, corrupted downloads, concurrent commands, failed restore state, missing helpers,
download size limits, examples/emoji (modes, names, rollback), what the publisher
lists (Bluetooth program hash, emoji only with `--emoji`) and a published update
installed end to end. They do not prove hardware SSH/board/WhatsApp behavior.

WhatsApp transport regression: `python3 linux-port/deck-modern/tests/test_whatsapp_transport.py`
uses a fake SSH executable with the real shell helper. It checks specific key,
host-identity, network and unknown-handshake messages, private temporary error
cleanup, UTF-8/photo preservation and no replay of sends. The helper does not
show raw SSH stderr or alter known-host trust. With the same local Dropbear and
OpenSSH dependencies as `brain/test_modern_chain.sh`, run
`python3 brain/test_whatsapp_chain.py` for real loopback authentication,
wrong-key and changed-host checks. Neither test uses a real WhatsApp account.
