#ifndef NZEUS_KEYVAULT_H
#define NZEUS_KEYVAULT_H
#include <stddef.h>
#include <stdint.h>

/* Keeps a secret file (the console's SSH key for Claude) ENCRYPTED with the
 * user's PIN, because the FAT32 SD card can't lock files: anyone holding the
 * card could otherwise copy the key.
 *
 *   PIN --PBKDF2-HMAC-SHA256 (30 000 rounds, random salt)--> 64-byte key
 *   file --ChaCha20--> encrypted, then HMAC-SHA256 over everything (a wrong
 *   PIN or a changed file is detected before anything is written).
 * File: "NZK1" | salt[16] | nonce[12] | encrypted bytes | tag[32]
 *
 * Honest limit: a short PIN can still be guessed by a computer that has the
 * card (it tries every PIN). It is a strong lock against casual copying; the
 * real protection stays the same: if the card is lost, delete the console's
 * line on the brain (brain/pc/README.md). */

int vault_encrypt_file(const char *plain_path, const char *vault_path, const char *pin);   /* 0 = ok */
int vault_decrypt_file(const char *vault_path, const char *plain_path, const char *pin);   /* 0 ok, -1 wrong PIN / damaged, -2 no file */

/* the building blocks (tests) */
void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]);
void pbkdf2_sha256(const char *pass, const uint8_t *salt, size_t slen, unsigned iters, uint8_t *out, size_t olen);
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *data, size_t len);

#endif
