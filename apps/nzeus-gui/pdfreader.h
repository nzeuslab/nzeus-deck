#ifndef NZEUS_PDFREADER_H
#define NZEUS_PDFREADER_H
#include "bgjob.h"
#include "fsops.h"

/* Poppler runs outside the GUI. Only one bounded page image is cached. */
typedef struct {
    BgJob job;
    char path[FS_PATH_MAX], dir[80], bitmap[96], error[128];
    int pages, page, state;             /* 1 info, 2 rendering, 3 ready, -1 error */
} PdfReader;
int pdfreader_open(PdfReader *p, const char *path);
int pdfreader_page(PdfReader *p, int page);
int pdfreader_poll(PdfReader *p);        /* 1 new bitmap, -1 new error, 0 waiting */
void pdfreader_close(PdfReader *p);
#endif
