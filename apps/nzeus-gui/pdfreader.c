/* A real PDF renderer, shared by Files and Images. Poppler handles fonts/RTL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pdfreader.h"
#include "settings.h"
#include "picture.h"

/* Enough space for every character to be a shell quote; never truncate a path. */
static void quote(const char *s, char *out)
{
    *out++ = '\'';
    while (*s) {
        if (*s == '\'') { memcpy(out, "'\\''", 4); out += 4; }
        else *out++ = *s;
        s++;
    }
    *out++ = '\''; *out = 0;
}

static int fail(PdfReader *p, const char *message)
{
    snprintf(p->error, sizeof p->error, "%s", message);
    p->state = -1;
    return -1;
}

static int start(PdfReader *p, const char *command)
{
    char log[96];
    snprintf(log, sizeof log, "%s/job.log", p->dir);
    if (bgjob_start(&p->job, command, log)) return fail(p, "Could not start the PDF reader");
    /* Establish the child's group immediately, so B can cancel even at startup. */
    setpgid(p->job.pid, p->job.pid);
    return 0;
}

int pdfreader_open(PdfReader *p, const char *path)
{
    memset(p, 0, sizeof *p);
    struct stat st;
    if (!path || path[0] != '/' || strlen(path) >= sizeof p->path ||
        stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 40 * 1024 * 1024)
        return fail(p, "Cannot open this PDF (missing, empty, or larger than 40 MB)");
    snprintf(p->path, sizeof p->path, "%s", path);
    snprintf(p->dir, sizeof p->dir, NZ_DIR "/pdf-XXXXXX");
    if (!mkdtemp(p->dir)) { p->dir[0] = 0; return fail(p, "Could not create the PDF page cache"); }
    snprintf(p->bitmap, sizeof p->bitmap, "%s/page.png", p->dir);
    char file[FS_PATH_MAX * 4 + 3], command[FS_PATH_MAX * 4 + 160];
    quote(path, file);
    snprintf(command, sizeof command, "ulimit -v 81920; export LC_ALL=C; exec timeout -k 2 30 pdfinfo %s", file);
    p->state = 1;
    return start(p, command);
}

int pdfreader_page(PdfReader *p, int page)
{
    if (page < 1 || page > p->pages) return -1;
    bgjob_stop(&p->job);
    unlink(p->bitmap);                         /* our own previous cached page only */
    p->page = page; p->state = 2;
    char file[FS_PATH_MAX * 4 + 3], output[sizeof p->dir * 4 + 32];
    char prefix[96], command[FS_PATH_MAX * 4 + 800];
    quote(p->path, file);
    snprintf(prefix, sizeof prefix, "%s/page", p->dir); quote(prefix, output);
    snprintf(command, sizeof command,
             "ulimit -v 81920; export LC_ALL=C; exec timeout -k 2 30 pdftoppm -f %d -l %d "
             "-singlefile -scale-to %d -png %s %s", page, page, PICTURE_PDF_EDGE, file, output);
    return start(p, command);
}

int pdfreader_poll(PdfReader *p)
{
    if (p->state != 1 && p->state != 2) return 0;
    if (bgjob_poll(&p->job)) return 0;
    if (p->job.exit_code) {
        if (p->job.exit_code == 127) return fail(p, "PDF reader unavailable: Poppler is not installed");
        if (p->job.exit_code == 124 || p->job.exit_code == 137) return fail(p, "This PDF page took too long to open");
        return fail(p, "Cannot read this PDF: damaged, password protected, or too complex");
    }
    if (p->state == 1) {
        char info[4096]; bgjob_read(&p->job, info, sizeof info);
        const char *line = !strncmp(info, "Pages:", 6) ? info : strstr(info, "\nPages:");
        if (line && *line == '\n') line++;
        p->pages = line ? atoi(line + 6) : 0;
        if (p->pages < 1 || p->pages > 10000) return fail(p, "Could not read this PDF's page count");
        return pdfreader_page(p, 1);
    }
    struct stat st;
    if (stat(p->bitmap, &st) || st.st_size < 54 || st.st_size > 10 * 1024 * 1024)
        return fail(p, "The PDF page could not be rendered");
    p->state = 3;
    return 1;
}

void pdfreader_close(PdfReader *p)
{
    bgjob_stop(&p->job);
    if (p->dir[0]) {
        char log[96]; snprintf(log, sizeof log, "%s/job.log", p->dir);
        unlink(log); unlink(p->bitmap); rmdir(p->dir);
    }
    memset(p, 0, sizeof *p);
}
