#ifndef NZEUS_SYSINFO_H
#define NZEUS_SYSINFO_H
#include <stddef.h>

/* A few lines about the system (kernel, memory, SD card, Wi-Fi, uptime),
 * read live from Linux, for the System window. */
void sysinfo_text(char *buf, size_t size);
long sysinfo_meminfo_kb(const char *key);       /* one /proc/meminfo value in kB, -1 if missing */
void sysinfo_kernel(char *buf, size_t size);    /* "4.4.186-release" */

#endif
