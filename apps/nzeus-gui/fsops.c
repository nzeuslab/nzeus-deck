/* fsops.c - file operations for the Files app (NZEUS's own). */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fsops.h"
#include "icons.h"

void fs_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    if (len && dir[len - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else snprintf(out, n, "%s/%s", dir, name);
}

int fs_path_contains(const char *parent, const char *child)
{
    size_t n = strlen(parent);
    return !strcmp(parent, child) || (!strncmp(parent, child, n) && child[n] == '/');
}

/* USB drives are mounted at /media/<name> (but /media/mmc is the SD card). */
int fs_is_usb(const char *p)
{
    return !strncmp(p, "/media/", 7) && p[7] && strncmp(p, "/media/mmc", 10) != 0;
}

int fs_is_allowed(const char *p)
{
    return fs_path_contains(NZ_SDCARD, p) || fs_is_usb(p);
}

/* Copy a file, or a folder with everything in it. */
int fs_copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[FS_PATH_MAX], b[FS_PATH_MAX];
            fs_join(a, sizeof a, src, e->d_name);
            fs_join(b, sizeof b, dst, e->d_name);
            if (fs_copy_tree(a, b) != 0) { rc = -1; break; }
        }
        closedir(d);
        return rc;
    }
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        return -1;
    }
    char buf[32768];
    ssize_t got;
    int rc = 0;
    while ((got = read(in, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (got > 0) {                         /* write() may write less than asked */
            ssize_t wr = write(out, p, (size_t)got);
            if (wr <= 0) { rc = -1; break; }
            p += wr;
            got -= wr;
        }
        if (rc) break;
    }
    if (got < 0) rc = -1;
    close(in);
    if (close(out) != 0) rc = -1;
    return rc;
}

/* Delete a file, or a folder with everything in it. */
int fs_remove_tree(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[FS_PATH_MAX];
        fs_join(c, sizeof c, p, e->d_name);
        if (fs_remove_tree(c) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(p) != 0) rc = -1;
    return rc;
}

unsigned long long fs_tree_size(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long total = 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[FS_PATH_MAX];
        fs_join(c, sizeof c, p, e->d_name);
        total += fs_tree_size(c);
    }
    closedir(d);
    return total;
}

void fs_size_label(unsigned long long b, char *out, size_t n)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f GB", (double)b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024)   snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024));
    else if (b >= 1024)             snprintf(out, n, "%.1f KB", (double)b / 1024.0);
    else                            snprintf(out, n, "%llu B", b);
}

/* "photo.jpg" is taken -> "photo (2).jpg", "photo (3).jpg", ... */
void fs_unique_name(const char *wanted, char *out, size_t n)
{
    snprintf(out, n, "%s", wanted);
    if (access(wanted, F_OK) != 0) return;
    const char *slash = strrchr(wanted, '/');
    const char *name = slash ? slash + 1 : wanted;
    int folder_len = slash ? (int)(slash - wanted + 1) : 0;
    const char *dot = strrchr(name, '.');
    int stem_len = (dot && dot != name) ? (int)(dot - name) : (int)strlen(name);
    const char *ext = (dot && dot != name) ? dot : "";
    for (int i = 2; i < 1000; i++) {
        snprintf(out, n, "%.*s%.*s (%d)%s", folder_len, wanted, stem_len, name, i, ext);
        if (access(out, F_OK) != 0) return;
    }
}

/* The first mounted USB drive. If none is mounted yet, try to mount the
 * first USB disk (sda1, or sda without partitions) at /media/hdd. */
static int find_usb_mount(char *out, size_t n)
{
    FILE *f = fopen("/proc/mounts", "r");
    char dev[256], dir[256], rest[512];
    int found = 0;
    if (!f) return 0;
    while (fscanf(f, "%255s %255s %511[^\n]", dev, dir, rest) == 3)
        if (fs_is_usb(dir) && !strchr(dir + 7, '/')) { snprintf(out, n, "%s", dir); found = 1; break; }
    fclose(f);
    return found;
}

int fs_usb_mount_point(char *out, size_t n)
{
    if (find_usb_mount(out, n)) return 1;
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    char dev[96] = "";
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "sd", 2) || strlen(e->d_name) != 3) continue;
        snprintf(dev, sizeof dev, "/dev/%s1", e->d_name);
        if (access(dev, F_OK) != 0) snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
        break;
    }
    closedir(d);
    if (!dev[0]) return 0;
    mkdir("/media/hdd", 0777);
    char cmd[320];   /* FAT first, then exFAT (sticks bigger than 32 GB) */
    snprintf(cmd, sizeof cmd, "mount -t auto -o iocharset=utf8 %s /media/hdd 2>/dev/null || "
                              "mount.exfat -o iocharset=utf8 %s /media/hdd 2>/dev/null", dev, dev);
    if (system(cmd) != 0) { rmdir("/media/hdd"); return 0; }
    return find_usb_mount(out, n);
}

int fs_is_text_file(const char *name)
{
    static const char *ext[] = { "py", "txt", "sh", "c", "h", "cpp", "hpp", "ino", "md", "json", "cfg",
                                 "ini", "conf", "xml", "csv", "log", "lst", "yml", "yaml", "html", "css", "js", NULL };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    for (int i = 0; ext[i]; i++) if (!strcasecmp(dot + 1, ext[i])) return 1;
    return 0;
}

static int has_ext(const char *name, const char *const ext[])
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    for (int i = 0; ext[i]; i++) if (!strcasecmp(dot + 1, ext[i])) return 1;
    return 0;
}

int fs_is_image_file(const char *name)
{
    static const char *const ext[] = { "png", "bmp", "jpg", "jpeg", "gif", NULL };
    return has_ext(name, ext);
}

int fs_is_audio_file(const char *name)
{
    static const char *const ext[] = { "wav", "mp3", NULL };
    return has_ext(name, ext);
}

int fs_file_icon(const char *name, int is_dir)
{
    static const char *const code[] = { "py", "sh", "c", "h", "cpp", "hpp", "ino", "js", "html", "css", "json", NULL };
    if (is_dir) return ICON_FOLDER;
    if (fs_is_image_file(name)) return ICON_FILE_IMAGE;
    if (fs_is_audio_file(name)) return ICON_FILE_AUDIO;
    if (has_ext(name, code)) return ICON_FILE_CODE;
    if (fs_is_text_file(name)) return ICON_FILE_TEXT;
    return ICON_FILE;
}
