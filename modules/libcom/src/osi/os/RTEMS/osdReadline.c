/*
 * Simple line editor for RTEMS POSIX iocsh
 * History recall with up/down arrows
 * No cursor movement - appends at end only
 * Author: FHI Berlin
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include "epicsReadlinePvt.h"

#define MAXLINE  256
#define MAXHIST   50

typedef struct {
    char *lines[MAXHIST];
    int   count;
    int   pos;
} History;

struct osdContext {
    History hist;
    int     fd;
};

static void hist_add(History *h, const char *line)
{
    if (!line || !line[0]) return;
    if (h->count > 0 &&
        strcmp(h->lines[(h->count-1) % MAXHIST], line) == 0) return;
    int idx = h->count % MAXHIST;
    free(h->lines[idx]);
    h->lines[idx] = strdup(line);
    h->count++;
    h->pos = h->count;
}

static void
osdReadlineBegin(struct readlineContext *rc)
{
    if (rc->in != NULL) return;  /* file input - no editing */
    if (!isatty(STDIN_FILENO)) return;
    struct osdContext *ctx = calloc(1, sizeof(struct osdContext));
    if (!ctx) return;
    ctx->fd = STDIN_FILENO;
    rc->osd = (struct osdContext *)ctx;
}

static char *
osdReadline(const char *prompt, struct readlineContext *rc)
{
    struct osdContext *ctx = (struct osdContext *)rc->osd;
    struct termios saved, raw;
    char buf[MAXLINE];
    int len = 0;
    History *h = &ctx->hist;

    /* print prompt before raw mode so console driver flushes it */
    if (prompt && *prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }

    /* switch to raw mode for special key handling */
    tcgetattr(ctx->fd, &saved);
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON);
    raw.c_oflag |= OPOST | ONLCR;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(ctx->fd, TCSAFLUSH, &raw);

    h->pos = h->count;
    memset(buf, 0, sizeof(buf));

    while (1) {
        unsigned char c;
        if (read(ctx->fd, &c, 1) <= 0) {
            tcsetattr(ctx->fd, TCSAFLUSH, &saved);
            return NULL;
        }

        if (c == '\r' || c == '\n') {
            fputc('\n', stdout);
            fflush(stdout);
            break;

        } else if (c == 0x7f || c == '\b') {   /* backspace */
            if (len > 0) {
                len--;
                fputs("\b \b", stdout);
                fflush(stdout);
            }

        } else if (c == 0x15) {                /* Ctrl-U: kill line */
            while (len > 0) {
                fputs("\b \b", stdout);
                len--;
            }
            fflush(stdout);

        } else if (c == 0x0b) {                /* Ctrl-K: same as Ctrl-U here */
            while (len > 0) {
                fputs("\b \b", stdout);
                len--;
            }
            fflush(stdout);

        } else if (c == 0x04 && len == 0) {    /* Ctrl-D: EOF on empty line */
            tcsetattr(ctx->fd, TCSAFLUSH, &saved);
            return NULL;

        } else if (c == 0x09) {                /* Tab: beep for now */
            fputc('\007', stdout);
            fflush(stdout);

        } else if (c == 0x0c) {                /* Ctrl-L: clear screen */
            fputs("\033[2J\033[H", stdout);
            if (prompt && *prompt) fputs(prompt, stdout);
            fwrite(buf, 1, len, stdout);
            fflush(stdout);

        } else if (c == 0x01) {                /* Ctrl-A: go to start (beep - no cursor) */
            fputc('\007', stdout);
            fflush(stdout);

        } else if (c == 0x05) {                /* Ctrl-E: go to end (beep - no cursor) */
            fputc('\007', stdout);
            fflush(stdout);

        } else if (c == 0x1b) {                /* ESC sequence */
            unsigned char seq[3];
            if (read(ctx->fd, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[') continue;
            if (read(ctx->fd, &seq[1], 1) <= 0) continue;

            if (seq[1] == 'A' && h->count > 0) {   /* up arrow */
                /* erase current line */
                while (len > 0) {
                    fputs("\b \b", stdout);
                    len--;
                }
                if (h->pos > 0) h->pos--;
                const char *hp = h->lines[h->pos % MAXHIST];
                if (hp) {
                    strncpy(buf, hp, MAXLINE-1);
                    buf[MAXLINE-1] = '\0';
                    len = strlen(buf);
                    fwrite(buf, 1, len, stdout);
                }
                fflush(stdout);

            } else if (seq[1] == 'B') {            /* down arrow */
                while (len > 0) {
                    fputs("\b \b", stdout);
                    len--;
                }
                if (h->pos < h->count) h->pos++;
                if (h->pos < h->count) {
                    const char *hp = h->lines[h->pos % MAXHIST];
                    if (hp) {
                        strncpy(buf, hp, MAXLINE-1);
                        buf[MAXLINE-1] = '\0';
                        len = strlen(buf);
                        fwrite(buf, 1, len, stdout);
                    }
                } else {
                    memset(buf, 0, sizeof(buf));
                    len = 0;
                }
                fflush(stdout);

            } else if (seq[1] == '3') {            /* DEL key: consume ~ */
                read(ctx->fd, &seq[2], 1);
            }
            /* ignore left/right/other sequences */

        } else if (c >= 0x20 && len < MAXLINE-1) { /* printable */
            buf[len++] = c;
            fputc(c, stdout);
            fflush(stdout);
        }
    }

    tcsetattr(ctx->fd, TCSAFLUSH, &saved);
    buf[len] = '\0';
    hist_add(h, buf);
    return strdup(buf);
}

static void
osdReadlineEnd(struct readlineContext *rc)
{
    struct osdContext *ctx = (struct osdContext *)rc->osd;
    if (!ctx) return;
    for (int i = 0; i < MAXHIST; i++)
        free(ctx->hist.lines[i]);
    free(ctx);
    rc->osd = NULL;
}
