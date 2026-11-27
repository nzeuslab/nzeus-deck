/* settings.c - reads and writes nzeus/settings.ini (see settings.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "settings.h"

#define MAX_KEYS 48
#define KEY_LEN  32
#define VAL_LEN  96

static struct { char key[KEY_LEN], val[VAL_LEN]; } items[MAX_KEYS];
static int count, loaded;

void settings_reload(void)
{
    char line[KEY_LEN + VAL_LEN + 8];
    count = 0;
    loaded = 1;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;                                   /* no file yet: all defaults */
    while (fgets(line, sizeof line, f) && count < MAX_KEYS) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        snprintf(items[count].key, KEY_LEN, "%.*s", KEY_LEN - 1, line);
        snprintf(items[count].val, VAL_LEN, "%.*s", VAL_LEN - 1, eq + 1);
        count++;
    }
    fclose(f);
}

static int find(const char *key)
{
    if (!loaded) settings_reload();
    for (int i = 0; i < count; i++) if (!strcmp(items[i].key, key)) return i;
    return -1;
}

const char *settings_get(const char *key, const char *fallback)
{
    int i = find(key);
    return i >= 0 ? items[i].val : fallback;
}

int settings_get_int(const char *key, int fallback)
{
    const char *v = settings_get(key, NULL);
    return v ? atoi(v) : fallback;
}

/* Write all settings to a new file, then swap it in, so a power cut while
 * saving can't leave a half-written settings file. */
static void save(void)
{
    mkdir(NZ_DIR, 0755);
    FILE *f = fopen(SETTINGS_FILE ".new", "w");
    if (!f) return;
    fprintf(f, "# NZEUS Deck settings (changed by the Settings app)\n");
    for (int i = 0; i < count; i++) fprintf(f, "%s=%s\n", items[i].key, items[i].val);
    fclose(f);
    rename(SETTINGS_FILE ".new", SETTINGS_FILE);
    sync();
}

void settings_set(const char *key, const char *value)
{
    int i = find(key);
    if (i < 0) {
        if (count >= MAX_KEYS) return;
        i = count++;
        snprintf(items[i].key, KEY_LEN, "%s", key);
    } else if (!strcmp(items[i].val, value)) {
        return;                                       /* unchanged: no write */
    }
    snprintf(items[i].val, VAL_LEN, "%s", value);
    save();
}

void settings_set_int(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    settings_set(key, v);
}

int settings_accept_is_b(void)
{
    return !strcmp(settings_get("accept_button", "a"), "b");
}
