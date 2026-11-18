/* keyvault.c - a PIN-encrypted file (see keyvault.h).
 * HMAC: RFC 2104. PBKDF2: RFC 8018. ChaCha20: RFC 8439. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "keyvault.h"
#include "sha256.h"

#define ROUNDS   30000             /* about 1 s on the console: slow for guessers, OK for us */
#define MAX_FILE 16384

void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, inner[32];
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    uint8_t *buf = malloc(64 + (mlen > 32 ? mlen : 32));
    if (!buf) { memset(out, 0, 32); return; }
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;            /* inner: (key ^ ipad) + message */
    memcpy(buf + 64, msg, mlen);
    sha256(buf, 64 + mlen, inner);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5c;            /* outer: (key ^ opad) + inner hash */
    memcpy(buf + 64, inner, 32);
    sha256(buf, 64 + 32, out);
    free(buf);
}

void pbkdf2_sha256(const char *pass, const uint8_t *salt, size_t slen, unsigned iters, uint8_t *out, size_t olen)
{
    size_t plen = strlen(pass);
    uint8_t *s = malloc(slen + 4);
    if (!s) return;
    memcpy(s, salt, slen);
    for (uint32_t block = 1; olen; block++) {
        uint8_t u[32], t[32];
        s[slen] = block >> 24; s[slen + 1] = block >> 16; s[slen + 2] = block >> 8; s[slen + 3] = (uint8_t)block;
        hmac_sha256((const uint8_t *)pass, plen, s, slen + 4, u);
        memcpy(t, u, 32);
        for (unsigned i = 1; i < iters; i++) {                    /* the slow part: on purpose */
            hmac_sha256((const uint8_t *)pass, plen, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = olen < 32 ? olen : 32;
        memcpy(out, t, n);
        out += n;
        olen -= n;
    }
    free(s);
}

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
                       a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *data, size_t len)
{
    uint32_t st[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };   /* "expand 32-byte k" */
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + i * 4);
    for (int i = 0; i < 3; i++) st[13 + i] = le32(nonce + i * 4);
    while (len) {
        uint32_t x[16];
        st[12] = counter++;
        memcpy(x, st, sizeof x);
        for (int r = 0; r < 10; r++) {                           /* 20 rounds: columns, then diagonals */
            QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
        }
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) {
            uint32_t v = x[i] + st[i];
            ks[i * 4] = (uint8_t)v; ks[i * 4 + 1] = v >> 8; ks[i * 4 + 2] = v >> 16; ks[i * 4 + 3] = v >> 24;
        }
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        data += n;
        len -= n;
    }
}

/* ---------- files ---------- */
static long read_all(const char *path, uint8_t *buf, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, max, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return more ? -1 : (long)n;
}

static int write_private(const char *path, const uint8_t *buf, size_t n)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);       /* only the owner may read it */
    if (fd < 0) return -1;
    int ok = write(fd, buf, n) == (ssize_t)n;
    ok = (close(fd) == 0) && ok;
    return ok ? 0 : -1;
}

static int random_bytes(uint8_t *b, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    int ok = read(fd, b, n) == (ssize_t)n;
    close(fd);
    return ok ? 0 : -1;
}

static void keys_from_pin(const char *pin, const uint8_t salt[16], uint8_t enc[32], uint8_t mac[32])
{
    uint8_t k[64];
    pbkdf2_sha256(pin, salt, 16, ROUNDS, k, 64);
    memcpy(enc, k, 32);
    memcpy(mac, k + 32, 32);
    memset(k, 0, sizeof k);
}

/* The work buffer (16 KB) is borrowed only while a file is locked/unlocked,
 * then wiped (it held a key) and given back - not kept all the time. */
#define VAULT_BUF (4 + 16 + 12 + MAX_FILE + 32)
static void wipe_free(uint8_t *buf)
{
    volatile uint8_t *v = buf;                               /* (volatile: the wipe can't be skipped) */
    for (size_t i = 0; i < VAULT_BUF; i++) v[i] = 0;
    free(buf);
}

int vault_encrypt_file(const char *plain_path, const char *vault_path, const char *pin)
{
    uint8_t *buf = malloc(VAULT_BUF);
    if (!buf) return -1;
    long n = read_all(plain_path, buf + 32, MAX_FILE);
    if (n < 0) { wipe_free(buf); return -1; }
    uint8_t enc[32], mac[32];
    memcpy(buf, "NZK1", 4);
    if (random_bytes(buf + 4, 28) != 0) { wipe_free(buf); return -1; }   /* salt + nonce */
    keys_from_pin(pin, buf + 4, enc, mac);
    chacha20_xor(enc, buf + 20, 1, buf + 32, (size_t)n);
    hmac_sha256(mac, 32, buf, 32 + (size_t)n, buf + 32 + n);
    memset(enc, 0, 32); memset(mac, 0, 32);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", vault_path);
    int rc = write_private(tmp, buf, 32 + (size_t)n + 32);
    wipe_free(buf);
    if (rc != 0) return -1;
    return rename(tmp, vault_path) == 0 ? 0 : -1;
}

int vault_decrypt_file(const char *vault_path, const char *plain_path, const char *pin)
{
    uint8_t *buf = malloc(VAULT_BUF);
    if (!buf) return -2;
    long n = read_all(vault_path, buf, VAULT_BUF);
    if (n < 0) { wipe_free(buf); return -2; }
    if (n < 64 || memcmp(buf, "NZK1", 4)) { wipe_free(buf); return -1; }
    size_t len = (size_t)n - 64;
    uint8_t enc[32], mac[32], tag[32];
    keys_from_pin(pin, buf + 4, enc, mac);
    hmac_sha256(mac, 32, buf, 32 + len, tag);
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= tag[i] ^ buf[32 + len + i];   /* (no early exit) */
    if (diff) { memset(enc, 0, 32); memset(mac, 0, 32); wipe_free(buf); return -1; }  /* wrong PIN or damaged */
    chacha20_xor(enc, buf + 20, 1, buf + 32, len);
    memset(enc, 0, 32); memset(mac, 0, 32);
    int rc = write_private(plain_path, buf + 32, len);
    wipe_free(buf);                                          /* (the key was in it) */
    return rc;
}
