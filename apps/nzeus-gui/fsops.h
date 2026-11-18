#ifndef NZEUS_FSOPS_H
#define NZEUS_FSOPS_H
#include <stddef.h>

/* File operations for the Files app (NZEUS's own).
 * Changes are only allowed on the SD card and on USB drives, so a wrong click
 * can never damage the system folders. */

#ifndef NZ_SDCARD                       /* NZEUS's own folder (see settings.h) */
#define NZ_SDCARD "/tmp/nzeus-data"
#endif
#define FS_PATH_MAX 1024

void fs_join(char *out, size_t n, const char *dir, const char *name);   /* dir + "/" + name */
int  fs_is_allowed(const char *path);         /* inside the SD card or a USB drive */
int  fs_is_usb(const char *path);             /* inside a USB drive (/media/...) */
int  fs_path_contains(const char *parent, const char *child);   /* child is parent or inside it */

int  fs_copy_tree(const char *src, const char *dst);    /* file or whole folder; 0 = ok */
int  fs_remove_tree(const char *path);                  /* file or whole folder; 0 = ok */
unsigned long long fs_tree_size(const char *path);      /* bytes, folders added up */
void fs_size_label(unsigned long long bytes, char *out, size_t n);   /* "1.5 MB" */
void fs_unique_name(const char *wanted, char *out, size_t n);        /* "name (2).txt" if taken */

int  fs_usb_mount_point(char *out, size_t n);  /* a mounted USB drive (mounts it if needed); 1 = found */
int  fs_is_text_file(const char *name);        /* .txt .py .sh .c ... */
int  fs_is_image_file(const char *name);       /* .png .bmp .jpg .jpeg .gif */
int  fs_is_audio_file(const char *name);       /* .wav .mp3 */
int  fs_file_icon(const char *name, int is_dir);   /* ICON_* (icons.h) for a list row */

#endif
