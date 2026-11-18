/* store.c - remembers paired devices on the SD card.
 *
 * One small text file per device:
 *     <dir>/devices/AA-BB-CC-DD-EE-FF.conf      (dir = /mnt/sdcard/nzeus/bluetooth)
 * with lines like "name=Logitech K380". It holds the PAIRING KEY, which is
 * what lets the keyboard reconnect without pairing again after a restart.
 * (Anyone who copies this file could pretend to be the console to that
 * keyboard - keep the SD card private.)
 *
 * Files are written to a .tmp file first and then renamed, so a power cut
 * never leaves a half-written file behind. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "bt.h"

const char *store_dir(void)
{
    const char *d = getenv("NZEUS_BT_DIR");
    return (d && *d) ? d : "/mnt/sdcard/nzeus/bluetooth";
}

static void device_path(const bdaddr_t *a, char *out, size_t n, const char *ext)
{
    char f[18];
    addr_file(a, f);
    snprintf(out, n, "%s/devices/%s%s", store_dir(), f, ext);
}

/* Turns a device into the text of its file. Returns the length, -1 if too big. */
int store_format(const struct bt_device *d, char *out, size_t outsz)
{
    char a[18], hex[2 * HID_DESC_MAX + 1];
    size_t n = 0;
    int i;
    /* ADD(...) = printf onto the end of out; gives up (-1) if out is full */
#define ADD(...) do {                                             \
        int r_ = snprintf(out + n, outsz - n, __VA_ARGS__);       \
        if (r_ < 0 || (size_t)r_ >= outsz - n)                    \
            return -1;                                            \
        n += r_;                                                  \
    } while (0)
    addr_str(&d->addr, a);
    ADD("# NZEUS Bluetooth device, written by nzeus-bt. Holds the pairing key: keep it private.\n");
    ADD("address=%s\n", a);
    ADD("type=%s\n", addr_type_str(d->addr_type));
    ADD("name=%s\n", d->name);
    ADD("kind=%s\n", d->kind[0] ? d->kind : "other");
    ADD("class=0x%06x\n", (unsigned)d->cod);
    ADD("appearance=0x%04x\n", d->appearance);
    if (d->has_linkkey) {
        hex_encode(d->linkkey, 16, hex, sizeof(hex));
        ADD("linkkey=%s\n", hex);
        ADD("linkkey_type=%u\n", d->linkkey_type);
        ADD("pin_len=%u\n", d->pin_len);
    }
    for (i = 0; i < d->n_ltk && i < 2; i++) {
        const struct bt_ltk *k = &d->ltk[i];
        hex_encode(k->val, 16, hex, sizeof(hex));
        /* master,type,enc_size,ediv,rand,key */
        ADD("ltk=%u,%u,%u,%u,%016llx,%s\n", k->master, k->type, k->enc_size, k->ediv,
            (unsigned long long)k->rand, hex);
    }
    if (d->has_irk) {
        hex_encode(d->irk, 16, hex, sizeof(hex));
        ADD("irk=%s\n", hex);
    }
    if (d->has_hid) {
        ADD("hid_vendor=0x%04x\n", d->hid_vendor);
        ADD("hid_product=0x%04x\n", d->hid_product);
        ADD("hid_version=0x%04x\n", d->hid_version);
        ADD("hid_vendor_src=%u\n", d->hid_vendor_src);
        ADD("hid_parser=0x%04x\n", d->hid_parser);
        ADD("hid_country=%u\n", d->hid_country);
        ADD("hid_subclass=0x%02x\n", d->hid_subclass);
        hex_encode(d->rd, d->rd_size, hex, sizeof(hex));
        ADD("hid_descriptor=%s\n", hex);
    }
#undef ADD
    return (int)n;
}

static unsigned long num(const char *v) { return strtoul(v, NULL, 0); }

/* Reads the text of a device file. Unknown lines are ignored; Windows line
 * endings are fine. Returns 0 if it had a valid address. */
int store_parse(const char *text, struct bt_device *d)
{
    const char *p = text;
    int have_addr = 0;
    memset(d, 0, sizeof(*d));
    while (*p) {
        char line[2 * HID_DESC_MAX + 64], *eq, *v, *e;
        size_t len = strcspn(p, "\n");
        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = 0;
            e = line + strlen(line);
            while (e > line && (e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            eq = strchr(line, '=');
            if (line[0] != '#' && eq) {
                *eq = 0; v = eq + 1;
                if (!strcmp(line, "address"))      have_addr = addr_parse(v, &d->addr) == 0;
                else if (!strcmp(line, "type"))    { int t = addr_type_parse(v); d->addr_type = t < 0 ? 0 : t; }
                else if (!strcmp(line, "name"))    snprintf(d->name, sizeof(d->name), "%s", v);
                else if (!strcmp(line, "kind"))    snprintf(d->kind, sizeof(d->kind), "%s", v);
                else if (!strcmp(line, "class"))   d->cod = num(v);
                else if (!strcmp(line, "appearance")) d->appearance = num(v);
                else if (!strcmp(line, "linkkey")) d->has_linkkey = hex_decode(v, d->linkkey, 16) == 16;
                else if (!strcmp(line, "linkkey_type")) d->linkkey_type = num(v);
                else if (!strcmp(line, "pin_len")) d->pin_len = num(v);
                else if (!strcmp(line, "irk"))     d->has_irk = hex_decode(v, d->irk, 16) == 16;
                else if (!strcmp(line, "ltk") && d->n_ltk < 2) {
                    struct bt_ltk *k = &d->ltk[d->n_ltk];
                    unsigned m, t, es, ed; unsigned long long r; char key[64];
                    if (sscanf(v, "%u,%u,%u,%u,%llx,%63s", &m, &t, &es, &ed, &r, key) == 6 &&
                        hex_decode(key, k->val, 16) == 16) {
                        k->master = m; k->type = t; k->enc_size = es; k->ediv = ed; k->rand = r;
                        d->n_ltk++;
                    }
                }
                else if (!strcmp(line, "hid_vendor"))    { d->hid_vendor = num(v); d->has_hid = 1; }
                else if (!strcmp(line, "hid_product"))   d->hid_product = num(v);
                else if (!strcmp(line, "hid_version"))   d->hid_version = num(v);
                else if (!strcmp(line, "hid_vendor_src")) d->hid_vendor_src = num(v);
                else if (!strcmp(line, "hid_parser"))    d->hid_parser = num(v);
                else if (!strcmp(line, "hid_country"))   d->hid_country = num(v);
                else if (!strcmp(line, "hid_subclass"))  d->hid_subclass = num(v);
                else if (!strcmp(line, "hid_descriptor")) {
                    int r = hex_decode(v, d->rd, sizeof(d->rd));
                    d->rd_size = r > 0 ? r : 0;
                    d->has_hid = 1;
                }
            }
        }
        p += len;
        if (*p == '\n') p++;
    }
    return have_addr ? 0 : -1;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64 * 1024) { fclose(f); return NULL; }
    buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

int store_load(const bdaddr_t *a, struct bt_device *d)
{
    char path[512], *text;
    int r;
    device_path(a, path, sizeof(path), ".conf");
    text = read_file(path);
    if (!text) return -1;
    r = store_parse(text, d);
    free(text);
    if (r == 0 && !addr_eq(&d->addr, a)) r = -1;   /* file name and contents disagree */
    return r;
}

static void make_dirs(void)
{
    char p[512];
    mkdir(store_dir(), 0700);
    snprintf(p, sizeof(p), "%s/devices", store_dir());
    mkdir(p, 0700);
}

int store_save(const struct bt_device *d)
{
    static char text[2 * HID_DESC_MAX + 4096];
    char path[512], tmp[520];
    int fd, n;
    n = store_format(d, text, sizeof(text));
    if (n < 0) return -1;
    make_dirs();
    device_path(&d->addr, path, sizeof(path), ".conf");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, text, n) != n) { close(fd); unlink(tmp); return -1; }
    fsync(fd);                     /* really on the card before we rename */
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int store_remove(const bdaddr_t *a)
{
    char path[512];
    device_path(a, path, sizeof(path), ".conf");
    return unlink(path) == 0 ? 0 : -1;
}

/* All paired devices. *out is malloc'd (free it); returns how many. */
int store_list(struct bt_device **out)
{
    char path[512];
    DIR *dir;
    struct dirent *e;
    int n = 0, cap = 0;
    struct bt_device *list = NULL;
    *out = NULL;
    snprintf(path, sizeof(path), "%s/devices", store_dir());
    dir = opendir(path);
    if (!dir) return 0;
    while ((e = readdir(dir)) != NULL) {
        bdaddr_t a;
        char name[18];
        size_t len = strlen(e->d_name);
        /* only "AA-BB-CC-DD-EE-FF.conf" */
        if (len != 22 || strcmp(e->d_name + 17, ".conf") != 0) continue;
        memcpy(name, e->d_name, 17);
        name[17] = 0;
        if (addr_parse(name, &a) != 0) continue;
        if (n == cap) {
            struct bt_device *bigger = realloc(list, (cap + 4) * sizeof(*list));
            if (!bigger) break;
            list = bigger;
            cap += 4;
        }
        if (store_load(&a, &list[n]) == 0) n++;
    }
    closedir(dir);
    *out = list;
    return n;
}

/* Only one nzeus-bt at a time may change the files (the daemon and a "pair"
 * command can both receive a new key). Returns an fd; close() it to unlock. */
int store_lock(void)
{
    char path[512];
    int fd;
    make_dirs();
    snprintf(path, sizeof(path), "%s/.lock", store_dir());
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}
