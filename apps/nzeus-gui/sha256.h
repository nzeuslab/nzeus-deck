#ifndef NZEUS_SHA256_H
#define NZEUS_SHA256_H
#include <stddef.h>
#include <stdint.h>

void sha256(const void *data, size_t len, unsigned char out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);   /* 64 hex digits + end */

#endif
