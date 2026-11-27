/* localoptions.h - NZEUS OS settings for Dropbear (copied into the source
 * tree by build_dropbear.sh). Anything set here overrides default_options.h.
 * Goal: small, key-only SSH for the R36S console (~42 MB RAM). */

/* --- Logins: public keys only (no passwords, so no libcrypt needed) --- */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1   /* honour command=, restrict, no-pty ... */
#define DROPBEAR_CLI_PUBKEY_AUTH 1
#define DROPBEAR_SK_KEYS 0              /* no U2F/FIDO hardware keys */

/* --- Key types: ed25519 + ecdsa only (RSA/DSS are big and slow) --- */
#define DROPBEAR_ED25519 1
#define DROPBEAR_ECDSA 1
#define DROPBEAR_RSA 0
#define DROPBEAR_DSS 0

/* --- Ciphers: chacha20-poly1305 (fast without AES hardware) + aes-ctr --- */
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_ENABLE_CBC_MODE 0
#define DROPBEAR_ENABLE_GCM_MODE 0
#define DROPBEAR_3DES 0
#define DROPBEAR_SHA2_256_HMAC 1

/* --- Key exchange: curve25519, sntrup761 (post-quantum, small),
 *     NIST ECDH and group14 for older clients. mlkem768 is 34 kB: off. --- */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_SNTRUP761 1
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_ECDH 1
#define DROPBEAR_DH_GROUP14_SHA256 1
#define DROPBEAR_DH_GROUP14_SHA1 0
#define DROPBEAR_DH_GROUP16 0

/* --- Forwarding: keep -L and -R (the reverse tunnel needs -R) --- */
#define DROPBEAR_CLI_LOCALTCPFWD 1
#define DROPBEAR_CLI_REMOTETCPFWD 1
#define DROPBEAR_SVR_LOCALTCPFWD 1
#define DROPBEAR_SVR_REMOTETCPFWD 1
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0

/* -J "program" (connection through a pipe) and netcat mode are kept on
 * purpose: without kernel TCP, a user-space network program can carry
 * the SSH connection through a pipe. */
#define DROPBEAR_CLI_PROXYCMD 1
#define DROPBEAR_CLI_NETCAT 1

/* --- Small extras off --- */
#define DO_MOTD 0
#define DROPBEAR_SFTPSERVER 0           /* no sftp-server on the console */
#define DEBUG_TRACE 0
#define DROPBEAR_SMALL_CODE 1
