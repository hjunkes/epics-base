/*************************************************************************\
* Copyright (c) 2002 The University of Saskatchewan
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

/* trigger sys/syslog.h to emit prioritynames[] */
#define SYSLOG_NAMES

#include <iostream>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/syslog.h>

#include <epicsRtemsInit.h>

#include <rtems.h>
#include <rtems/libcsupport.h>
#include <rtems/shell.h>
#include <rtems/telnetd.h>

#ifndef RTEMS_LEGACY_STACK
#include <rtems/bsd/bsd.h>
#endif

#include "errlog.h"
#include "iocsh.h"

/*
 * This code came from posix/rtems_init.c
 */

static const iocshArg rtshellArg0 = { "cmd", iocshArgString };
static const iocshArg rtshellArg1 = { "args", iocshArgArgv };
static const iocshArg * rtshellArgs[2] = { &rtshellArg0, &rtshellArg1 };
static const iocshFuncDef rtshellFuncDef = { "rt", 2, rtshellArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
                                            , "run rtems shell command"
#endif
                                           };

static void rtshellCallFunc(const iocshArgBuf *args) {
    rtems_shell_cmd_t *cmd = rtems_shell_lookup_cmd(args[0].sval);
    int ret;

    if (!cmd) {
        fprintf(stderr, "ERR: No such command\n");
        iocshSetError(-1);
    } else {
        fflush(stdout);
        fflush(stderr);
        ret = (*cmd->command)(args[1].aval.ac,args[1].aval.av);
        fflush(stdout);
        fflush(stderr);
        iocshSetError(ret);
        if(ret)
            fprintf(stderr, "ERR: %d\n",ret);
    }
}

/*
 * RTEMS status
 */
static void
rtems_netstat (unsigned int level)
{
#ifndef RTEMS_LEGACY_STACK

#else
    rtems_bsdnet_show_if_stats ();
    rtems_bsdnet_show_mbuf_stats ();
    if (level >= 1) {
        rtems_bsdnet_show_inet_routes ();
    }
    if (level >= 2) {
        rtems_bsdnet_show_ip_stats ();
        rtems_bsdnet_show_icmp_stats ();
        rtems_bsdnet_show_udp_stats ();
        rtems_bsdnet_show_tcp_stats ();
    }
#endif
}

static const iocshArg netStatArg0 = { "level",iocshArgInt };
static const iocshArg * const netStatArgs[1] = { &netStatArg0 };
static const iocshFuncDef netStatFuncDef = {"netstat", 1, netStatArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
                                            , "show network status"
#endif
                                           };
static void netStatCallFunc(const iocshArgBuf *args) {
    rtems_netstat(args[0].ival);
}

static const iocshFuncDef heapSpaceFuncDef = { "heapSpace",0,NULL
#ifdef IOCSHFUNCDEF_HAS_USAGE
                                              , "show malloc statistic"
#endif
                                             };
static void heapSpaceCallFunc(const iocshArgBuf *args) {
    Heap_Information_block info;
    malloc_info(&info);
    double x = info.Stats.size - (unsigned long)
        (info.Stats.lifetime_allocated - info.Stats.lifetime_freed);
    if (x >= 1024 * 1024)
        printf("Heap space: %.1f MB\n", x / (1024 * 1024));
    else
        printf("Heap space: %.1f kB\n", x / 1024);
}

int zoneset(const char *zone) {
    int ret;
    if (zone) {
        if ((ret = setenv("TZ", zone, 1)) < 0) {
            return ret;
        }
    } else if ((ret = unsetenv("TZ")) < 0) {
        return ret;
    }
    tzset();
    return 0;
}

static const iocshArg zonesetArg0 = { "zone string", iocshArgString };
static const iocshArg * const zonesetArgs[1] = { &zonesetArg0 };
static const iocshFuncDef zonesetFuncDef = { "zoneset", 1, zonesetArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
                                           , "set timezone (obsolete?)"
#endif
                                           };
static void zonesetCallFunc(const iocshArgBuf *args) {
    iocshSetError(zoneset(args[0].sval));
}

static void setlogmaskCallFunc(const iocshArgBuf *args) {
    const char* name = args[0].sval;
    const CODE* cur;
    if (!name) {
        printf("Usage: setlogmask <level>\n"
               "\n"
               "  Level names:\n");
        for (cur = prioritynames; cur->c_name; cur++) {
            printf("    %s\n", cur->c_name);
        }
    } else {
        for (cur = prioritynames; cur->c_name; cur++) {
            if (strcmp(name, cur->c_name) != 0) {
                continue;
            }
            (void) setlogmask(LOG_MASK(cur->c_val));
#ifndef RTEMS_LEGACY_STACK
            rtems_bsd_setlogpriority(name);
#endif
            return;
        }
        printf("Error: unknown log level.\n");
        iocshSetError(-1);
    }
}
static const iocshArg setlogmaskArg0 = {"level name", iocshArgString };
static const iocshArg * const setlogmaskArgs[1] = { &setlogmaskArg0 };
static const iocshFuncDef setlogmaskFuncDef = { "setlogmask", 1, setlogmaskArgs,
                                                "Set syslog() threshold level" };

/*
 * telnetd's shell entry point: a minimal iocsh session over the telnet
 * pty. Ported from posix/rtems_init.c; the reset sequence below was
 * confirmed against an independently-converged equivalent fix on the
 * mvme6100 port of this same init framework, so it's cross-checked
 * against two boards' worth of testing, not just this one's.
 *
 * - rtems-net-services' telnetd_session_task() fopen()s stdin/stdout/
 *   stderr against the pty device path exactly ONCE, when the session
 *   task is created, then reuses those same FILE* objects across every
 *   subsequent connection to that pty slot for the task's entire
 *   lifetime (see telnetd.c: fopen() happens before the `while (true)`
 *   accept loop, not inside it). A connection that ends any way other
 *   than us cleanly returning here (e.g. the client resets/closes the
 *   TCP connection abruptly instead of typing "bye") can leave EOF/
 *   error indicators, and a stale buffered-read pointer, set on these
 *   streams. The next client to land on that same pty slot then
 *   inherits a stream newlib's stdio won't read from, which looks like
 *   "connects, prints the prompt, then the connection just closes with
 *   no chance to type anything." freopen() was tried and rejected (in
 *   the mvme6100 testing this was ported from): it closes the PTY fd,
 *   which triggers the socket-close path and hands the *next* session
 *   a stale EOF too. What actually proved necessary: a short delay to
 *   let the freshly (re)bound pty socket settle before touching stream
 *   state at all, a termios flush both before and after, and resetting
 *   each stream's buffered-read pointer/count and EOF/error flags, not
 *   just clearing the flags. This pairs with the companion fix in
 *   rtems-net-services' telnetd/pty.c, which resets the pty driver's
 *   own termios cindex/ccount EOF-signalling state on each new
 *   connection -- that one's at the driver layer, this one's at the
 *   libc stdio layer above it.
 * - The pty driver signals EOF by handing back a literal VEOF byte
 *   (0x04, checked directly here) rather than always making fgets()
 *   return NULL, so check for it explicitly; "bye" still works too.
 * - No dup2() of the global fd 1/2 onto the session's stdout/stderr:
 *   telnetd allows up to client_maximum (default 5) concurrent
 *   sessions, and dup2()ing the *process-wide* fd 1/2 would let
 *   concurrent sessions stomp on each other's output. Each session's
 *   stdout/stderr are already correctly bound to its own pty by
 *   telnetd_session_task()'s fopen(), so it isn't needed.
 */
#define LINE_SIZE 256
static void telnet_pseudoIocsh(char *name, void *) {
    char line[LINE_SIZE];

    /* Let the freshly (re)bound pty socket settle before touching
     * stream state. */
    rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(100));
    tcflush(fileno(stdin), TCIFLUSH);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    stdin->_r = 0;
    stdin->_p = stdin->_bf._base;

    /*
     * Plain stdio here, not std::cout: telnetd_session_task() rebinds
     * the C library's per-task stdin/stdout/stderr globals to this
     * session's pty (via rtems_libio_set_private_env() + fopen()), and
     * fprintf(stdout, ...)/fputs(..., stdout) correctly follow that
     * per-task binding. std::cout does not -- it stays bound to
     * whatever stream was current when the C++ runtime first
     * constructed it (the console), so using it here would leak these
     * messages onto the console instead of the telnet client.
     */
    fprintf(stdout, "info: pty dev name = %s\r\n", name);
    fputs(" To leave please type 'bye'\r\n", stdout);
    fflush(stdout);

    /* Second settle + flush after the banner. */
    rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(500));
    tcflush(fileno(stdin), TCIFLUSH);
    clearerr(stdin);

    const char *prompt = "tIocSh> ";

    while (1) {
        fputs(prompt, stdout);
        fflush(stdout);
        if (fgets(line, LINE_SIZE, stdin) == NULL) {
            break;
        }
        if (line[0] == '\004') {
            /* VEOF from the pty driver's EOF-signalling hack */
            break;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0) {
            continue;
        }
        if (!strncmp(line, "bye", 3)) {
            break;
        }
        iocshCmd(line);
    }
    fputs("\r\ntelnet: session closed\r\n", stdout);
    fflush(stdout);
}

/* extern, not static: rtems_telnetd_initialize() looks this symbol up
 * by name (declared in <rtems/telnetd.h>). */
rtems_telnetd_config_table rtems_telnetd_config;

static int rtemsCmdsInitialize() {
    iocshRegister(&netStatFuncDef, netStatCallFunc);
    iocshRegister(&heapSpaceFuncDef, heapSpaceCallFunc);
    iocshRegister(&zonesetFuncDef, &zonesetCallFunc);
    iocshRegister(&rtshellFuncDef, &rtshellCallFunc);
    iocshRegister(&setlogmaskFuncDef, &setlogmaskCallFunc);
    rtems_shell_init_environment();
    std::cout << "RTEMS Commands registered" << std::endl;

    /* RTEMS_TELNETD_PORT=0 disables telnetd entirely */
    const char* telnetdPort = getenv("RTEMS_TELNETD_PORT");
    if (telnetdPort != nullptr && strcmp(telnetdPort, "0") == 0) {
        std::cout << "telnetd disabled (RTEMS_TELNETD_PORT=0)" << std::endl;
        return 0;
    }

    rtems_telnetd_config.command = telnet_pseudoIocsh;
    rtems_telnetd_config.arg = NULL;
    rtems_telnetd_config.priority = 0;
    rtems_telnetd_config.stack_size = 0;
    rtems_telnetd_config.client_maximum = 0;
    rtems_telnetd_config.login_check = NULL;
    rtems_telnetd_config.keep_stdio = false;

    int r = rtems_telnetd_initialize();
    if (r != 0) {
        std::cout << "error: cmds: telnetd initialize: " << r << std::endl;
        return r;
    }
    std::cout << "telnetd initialized" << std::endl;
    return 0;
}

void epicRtemsInit_cmds() {
    epicsRtemsInitRegisterHandler(
        "system", "cmds", rtemsInit_Order_commands, true, rtemsCmdsInitialize);
}
