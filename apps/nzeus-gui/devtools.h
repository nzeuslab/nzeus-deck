#ifndef NZEUS_DEVTOOLS_H
#define NZEUS_DEVTOOLS_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* The Dev toolbox's workers (app_devtools.c is the screen). All offline.
 * Each one reads text and writes its answer as text into out (n bytes).
 * Return 0 = ok, -1 = the input is not right (out then says why). */

int dt_number(const char *in, char *out, size_t n);       /* 255 / 0xFF / 0b1111 / 0o377 / 'A' -> every base */
int dt_b64_encode(const char *in, char *out, size_t n);
int dt_b64_decode(const char *in, char *out, size_t n);
int dt_hex_encode(const char *in, char *out, size_t n);   /* "Hi" -> "48 69" */
int dt_hex_decode(const char *in, char *out, size_t n);   /* "48 69" or "4869" -> "Hi" */
int dt_url_encode(const char *in, char *out, size_t n);   /* "a b&c" -> "a%20b%26c" */
int dt_url_decode(const char *in, char *out, size_t n);
int dt_hash(const char *in, char *out, size_t n);         /* SHA-256 + CRC-32 of the text */
uint32_t dt_crc32(const void *data, size_t len);
int dt_time(const char *in, time_t now, char *out, size_t n);   /* epoch <-> date (UTC); "" = now */
int dt_colour(const char *in, char *out, size_t n, uint32_t *rgb);   /* #RRGGBB / r,g,b / 565:F800 */
int dt_json(const char *in, char *out, size_t n);         /* checks it, then lays it out neatly */
int dt_regex(const char *pattern, const char *text, char *out, size_t n);   /* what matches where */

#endif
