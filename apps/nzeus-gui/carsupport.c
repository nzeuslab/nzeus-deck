/* Durable discovery evidence, independent from report rendering and vehicle commands. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "carsupport.h"

static void string(FILE *f, const char *value)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 32 || *p == 127) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void snapshot(FILE *f, const CarScan *s)
{
    fprintf(f, "{\"schema\":1,\"when\":%lld,\"read_only\":true,\"vin\":", (long long)s->when);
    string(f, s->vin);
    fprintf(f, ",\"year\":%d,\"make\":", s->model_year); string(f, s->make_en);
    fputs(",\"model_hint\":", f); string(f, s->make_model_en);
    fputs(",\"adapter\":", f); string(f, s->adapter);
    fputs(",\"protocol\":", f); string(f, s->protocol);
    fprintf(f, ",\"battery_v\":%.2f,\"modules\":[", s->battery_v);
    for (int i = 0; i < s->module_count; i++) {
        const ScanModule *m = &s->modules[i];
        fprintf(f, "%s{\"address\":%d,\"bus\":%d,\"answered\":%s,\"codes_read\":%s,\"name\":",
                i ? "," : "", m->can_id, m->bus, m->answered ? "true" : "false", m->codes_read ? "true" : "false");
        string(f, m->name_en);
        /* GM modules speak GM's own language (GMLAN) and answer on their address + 0x400;
         * their identifiers are GM's (GM90, GMB0...), not UDS F187... */
        fprintf(f, ",\"protocol\":\"%s\",\"identifiers\":{", m->protocol == SCAN_PROTO_GMLAN ? "GMLAN" : "UDS");
        for (int k = 0; k < 4; k++) {
            fprintf(f, "%s\"%s\":{\"status\":%d,\"hex\":", k ? "," : "", carscan_identity_name(m, k), m->identity_status[k]);
            string(f, m->identity_status[k] == 1 ? m->identity[k] : "");
            fputc('}', f);
        }
        fputs("}}", f);
    }
    fputs("],\"limitations\":\"Reachable modules only. Missing identifiers remain unknown. No configuration was read or changed. VIN does not establish model, variant, market or coding compatibility.\",\"notes\":[", f);
    for (int i = 0; i < s->note_count; i++) { if (i) fputc(',', f); string(f, s->notes_en[i]); }
    fputs("]}\n", f);
}

static int write_snapshot(const CarScan *s, const char *path)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(path); return -1; }
    snapshot(f, s);
    int bad = ferror(f) || fflush(f) || fsync(fd);
    if (fclose(f)) bad = 1;
    if (bad) unlink(path);
    return bad ? -1 : 0;
}

int carsupport_save(const CarScan *s, const char *folder)
{
    if (!s || s->demo || s->module_count < 0 || s->module_count > SCAN_MAX_MODULES ||
        s->note_count < 0 || s->note_count > SCAN_MAX_NOTES) return -1;
    char temp[640], final[640], latest_temp[640], latest[640];
    if (strlen(folder) > 450) return -1;
    mkdir(folder, 0700);
    snprintf(temp, sizeof temp, "%s/.discovery-XXXXXX", folder);
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    close(fd);
    /* mkstemp reserves a unique name; the actual writer uses exclusive creation. */
    unlink(temp);
    if (write_snapshot(s, temp)) return -1;
    snprintf(final, sizeof final, "%s/Vehicle-discovery-%lld-%s.json", folder,
             (long long)s->when, strrchr(temp, '-') + 1);
    if (rename(temp, final)) { unlink(temp); return -1; }
    snprintf(latest_temp, sizeof latest_temp, "%s/.latest-XXXXXX", folder);
    fd = mkstemp(latest_temp);
    if (fd < 0) return -1;
    close(fd); unlink(latest_temp);
    if (write_snapshot(s, latest_temp)) return -1;
    snprintf(latest, sizeof latest, "%s/Last-vehicle-discovery.json", folder);
    if (rename(latest_temp, latest)) { unlink(latest_temp); return -1; }
    fd = open(folder, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { int bad = fsync(fd); close(fd); return bad ? -1 : 0; }
    return -1;
}
