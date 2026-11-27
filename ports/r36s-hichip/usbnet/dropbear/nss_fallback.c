/* nss_fallback.c - only used in the static console (MIPS) build.
 *
 * Why: in a statically linked glibc program, user lookups (getpwnam etc.)
 * still try to load glibc's "NSS" plug-in libraries (libnss_files.so.2)
 * from the console's /lib at run time. If they are missing or a different
 * glibc version, Dropbear would say "Login attempt for nonexistent user".
 *
 * How: build_dropbear.sh links with  -Wl,--wrap=getpwnam,...  which makes
 * every call to getpwnam() land in __wrap_getpwnam() below. We first ask
 * glibc normally (__real_getpwnam); only if that finds nothing do we read
 * /etc/passwd ourselves with fgetpwent(), which needs no plug-ins. */
#define _DEFAULT_SOURCE
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* the original glibc functions (the linker provides these names) */
struct passwd *__real_getpwnam(const char *name);
struct passwd *__real_getpwuid(uid_t uid);
int __real_initgroups(const char *user, gid_t group);

/* Look through /etc/passwd line by line. Match by name, or by uid if name is NULL. */
static struct passwd *scan_passwd(const char *name, uid_t uid)
{
    FILE *f = fopen("/etc/passwd", "r");
    struct passwd *pw = NULL;
    if (f == NULL)
        return NULL;
    while ((pw = fgetpwent(f)) != NULL) {
        if (name ? strcmp(pw->pw_name, name) == 0 : pw->pw_uid == uid)
            break;          /* found it */
    }
    fclose(f);              /* pw points into glibc's own buffer, still valid */
    return pw;
}

struct passwd *__wrap_getpwnam(const char *name)
{
    struct passwd *pw = __real_getpwnam(name);
    return pw ? pw : scan_passwd(name, 0);
}

struct passwd *__wrap_getpwuid(uid_t uid)
{
    struct passwd *pw = __real_getpwuid(uid);
    return pw ? pw : scan_passwd(NULL, uid);
}

/* initgroups() sets the extra groups of a user. If glibc can't read the
 * group list, fall back to "only the user's main group". */
int __wrap_initgroups(const char *user, gid_t group)
{
    if (__real_initgroups(user, group) == 0)
        return 0;
    return setgroups(1, &group);
}
