/*************************************************************************\
* Copyright (c) 2002 The University of Saskatchewan
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <epicsRtemsInit.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syslog.h>

#include <rtems/bsd/bsd.h>

#include "errlog.h"

/*
 * Stop libbsd log messages being shredded across the console.
 *
 * default_putchar() in rtemsbsd/rtems/rtems-kernel-vprintf.c emits each
 * log line one character at a time with fputc(). Every task shares one
 * stdout FILE here (this newlib uses _REENT_THREAD_LOCAL with a global
 * __sf[3], and _tls_stdout points at __sf[1]), so another task writing
 * concurrently lands its own characters between ours. The result is
 * lines like "info: mve0: link state changed to UP" appearing one
 * letter per line, threaded through ifconfig's output.
 *
 * Format the message once and hand it to stdio as a single fwrite() so
 * it is copied into the stream in one operation instead of forty.
 * Using fwrite() rather than write(2) keeps ordering with respect to
 * other buffered output on the same stream.
 *
 * kvprintf() supports conversions vsnprintf() does not (%b, %D, %r, %y).
 * Those would both print wrongly and desynchronise the remaining
 * arguments, so such messages are passed to the original handler
 * untouched; they are rare and merely stay as ugly as before.
 */
/*
 * FreeBSD's pseudo-priority meaning "this came from printf(), do not
 * prefix it with a level name". It is declared under _KERNEL only, so it
 * is not visible to application space.
 */
#ifndef LOG_PRINTF
#define LOG_PRINTF (-1)
#endif

static rtems_bsd_vprintf_handler previousVprintfHandler;

static const char *const bsdLogLevelName[] = {
    "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
};

static bool usesKvprintfOnlyConversion(const char *fmt) {
    for (const char *p = fmt; (p = strchr(p, '%')) != NULL; ) {
        ++p;
        while (*p != '\0' && strchr("#0- +'", *p) != NULL) ++p;
        while (*p != '\0' && (isdigit((unsigned char) *p) || *p == '.' || *p == '*')) ++p;
        while (*p != '\0' && strchr("hljztLq", *p) != NULL) ++p;
        if (*p == '\0') break;
        if (strchr("bDry", *p) != NULL) return true;
        ++p;
    }
    return false;
}

/*
 * kvprintf() understands syslog's %m (strerror of the current errno);
 * vsnprintf() does not, and would print a bare "m". Expand it here so
 * messages like "ipv4_addroute: %m" keep their reason text. "%%m" is
 * left alone.
 */
static void expandErrnoConversion(const char *fmt, char *out, size_t outlen) {
    const char *err = strerror(errno);
    size_t o = 0;
    for (const char *p = fmt; *p != '\0' && o + 1 < outlen; ++p) {
        if (p[0] == '%' && p[1] == '%') {
            if (o + 2 >= outlen) break;
            out[o++] = *p++;
            out[o++] = *p;
        } else if (p[0] == '%' && p[1] == 'm') {
            for (const char *e = err; *e != '\0' && o + 1 < outlen; ++e) {
                out[o++] = *e;
            }
            ++p;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static int atomicVprintfHandler(int level, const char *fmt, va_list ap) {
    if (previousVprintfHandler == NULL) {
        return 0;
    }
    if (usesKvprintfOnlyConversion(fmt)) {
        return previousVprintfHandler(level, fmt, ap);
    }

    char fmtbuf[256];
    if (strstr(fmt, "%m") != NULL) {
        expandErrnoConversion(fmt, fmtbuf, sizeof(fmtbuf));
        fmt = fmtbuf;
    }

    char buf[512];
    int n = 0;
    if (level != LOG_PRINTF) {
        int pri = LOG_PRI(level);
        const char *name = (pri >= 0 && pri < (int)(sizeof(bsdLogLevelName) /
                                                   sizeof(bsdLogLevelName[0])))
                           ? bsdLogLevelName[pri] : "log";
        n = snprintf(buf, sizeof(buf), "%s: ", name);
    }
    int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    if (m < 0) {
        return 0;
    }
    n += (m < (int)(sizeof(buf) - n)) ? m : (int)(sizeof(buf) - n - 1);
    if (level != LOG_PRINTF && (n == 0 || buf[n - 1] != '\n') &&
        n < (int)(sizeof(buf) - 1)) {
        buf[n++] = '\n';
    }
    fwrite(buf, 1, (size_t) n, stdout);
    return n;
}

static int rtemsLogResetInitialize() {
    previousVprintfHandler = rtems_bsd_set_vprintf_handler(atomicVprintfHandler);

    /*
     * Put errlog's console on stdout.
     *
     * By default errlog writes to stderr while iocsh echoes to
     * epicsGetStdout(). Those are two different FILE objects with
     * independent buffers, so their output collides at the console
     * device and lines come out interleaved character by character.
     * Sharing one stream is the precondition for them not to.
     */
    errlogSetConsole(stdout);

    void rtems_bsp_reset_cause(char *buf, size_t capacity) __attribute__((weak));
    void (*reset_cause_p)(char *buf, size_t capacity) = rtems_bsp_reset_cause;

    if (reset_cause_p) {
        char buf[80];
        reset_cause_p(buf, sizeof(buf));
        errlogPrintf("Startup after %s\n", buf);
    }
    else {
        errlogPrintf("Startup\n");
    }
    errlogFlush();
    return 0;
}

static int logFlushErrorLog() {
    errlogFlush();
    return 0;
}

void epicRtemsInit_log() {
    epicsRtemsInitRegisterHandler(
        "system", "log.reset", rtemsInit_Order_pre_net_services + 100,
        true, rtemsLogResetInitialize);
    epicsRtemsInitRegisterHandler(
        "system", "log.err.flush", rtemsInit_Order_ioc - 1, true, logFlushErrorLog);
}
