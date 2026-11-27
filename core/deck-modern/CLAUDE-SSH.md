# Claude connections on SD Debian

Claude Code runs on the owner's PC or Android/Termux. The console's Claude app
opens an SSH session to that machine. Full Python and native TCP/IP do not make
Claude Code run locally on this MIPS console.

Use the existing brain setup in `brain/pc/setup_pc_ssh.ps1` or
`brain/android/setup.sh`. In the console app, configure that machine's address,
SSH port and user, then choose a PIN. Copy the console's **public** key
`/data/nzeus/deck/nzeus/claude/console_key.pub` into the brain's authorized keys
using its existing restricted setup. Keep the private key on the console.
Test on the home network first; setting a public address alone does not make
remote access work. This helper does not configure a router or firewall.

The modern connection helper uses the supplied unlocked key for one connection.
Its manual `--make-key` mode is for setup and troubleshooting. Host-key behavior
comes from Dropbear: first-use acceptance is requested; changed-key errors need
investigation before removing a stored key. The modern setup still needs a
hardware connection test with the owner's actual brain/account.

The **Claude access** reverse tunnel and command gate are not ported.
The Deck's Claude app has no access switch on the modern system.
The owner's existing root SSH service on the console is separate; do not
silently give the brain an unrestricted replacement login.

WhatsApp uses its own key and the existing `brain/whatsapp` command gate.
Testing its helper does not authorize sending messages or changing accounts.

## One PC at home and away

The PC Address row stores `claude_pc_host`: use the PC's **LAN address** here,
with port and user in their separate fields. The optional `claude_pc_public`
setting stores the home's public address for a configured router forward. The
helpers choose the LAN address first when the console's gateway is on that
network, and the public address first on a different network. Read-only
requests can try the other address; sends are never automatically replayed.

Do not replace the LAN address with the public address for both cases. Some
routers do not route home clients back through their own public address.
Internet access working on the console does not establish that this path works.
The current Address menu edits only host/port/user; the optional public setting
requires provisioning in settings.ini. Back up and edit it with the app stopped
normally, then restart so an older in-memory copy cannot overwrite the edit.
Do not remove known-host entries to fix an address problem.

## Modern connection regression

Run `bash brain/test_modern_chain.sh` in WSL from this repository. It compiles the real
Claude GUI driver, uses a standard x86 Dropbear client built from the pinned
release source, and starts an isolated OpenSSH fixture on 127.0.0.1. If needed,
official Ubuntu server packages are downloaded/extracted into the WSL test build
folder; no system SSH service is installed or started. GCC/make, ssh-keygen,
apt and dpkg-deb must already be present. `NZ_TEST_DROPBEAR` and `NZ_TEST_SSHD`
can select existing test tools.

The test exercises the real app, encrypted PIN vault, modern helper, real SSH
authentication, local/remote PTYs, terminal dimensions, typed request/reply,
disconnect cleanup, wrong-key rejection and changed-host-key rejection. The
remote Claude response is a controlled fixture, with no subscription/API call.
Temporary keys and known_hosts stay in test folders. It never edits the owner's
authorized_keys or contacts the console. Run GUI suites serially because their
compiled fake SD and RAM-key paths are shared. PC passing does not prove the
owner's network, brain setup or live Claude account.
