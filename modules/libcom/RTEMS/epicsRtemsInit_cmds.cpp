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
#include <sys/syslog.h>

#include <epicsRtemsInit.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <rtems/telnetd.h>
#include "iocsh.h"

#include <rtems.h>
#include <rtems/libcsupport.h>
#include <rtems/shell.h>

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
    if (!cmd) {
        fprintf(stderr, "ERR: No such command\n");
        iocshSetError(-1);
    } else {
        fflush(stdout);
        fflush(stderr);
        int ret = (*cmd->command)(args[1].aval.ac,args[1].aval.av);
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

#define TELNET_LINE_SIZE 256
static void
telnet_pseudoIocsh(char *name, void *)
{
    char line[TELNET_LINE_SIZE];
    /*
     * The stdio FILE buffer may contain a stale VEOF (0x04) from
     * the previous session. Purge the FILE input buffer and reset
     * the error/EOF flags before starting a new session.
     * Note: stdin FILE* is reused across sessions by telnetd.
     */
    /*
     * Do NOT freopen() - that closes the PTY fd which triggers
     * ptyShutdown() closing the socket, causing the next read to
     * return VEOF. Instead reset the FILE* internal buffer state
     * by directly clearing the glibc/newlib FILE struct fields.
     * tcflush resets the termios cindex/ccount EOF hack in the PTY.
     */
    {
        rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(100));
        int fd = fileno(stdin);
        tcflush(fd, TCIFLUSH);
        /* Reset newlib FILE buffer: clear read buffer and error flags */
        stdin->_r  = 0;   /* no buffered data to read */
        stdin->_p  = stdin->_bf._base;  /* reset read pointer */
        stdin->_flags &= ~(__SEOF | __SERR);  /* clear EOF/error */
    }
    fflush(stdout);
    fprintf(stdout, "EPICS IOC shell - type 'bye' to exit\r\n");
    fflush(stdout);
    /* Delay to let socket settle, then flush again */
    rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(500));
    tcflush(fileno(stdin), TCIFLUSH);
    clearerr(stdin);
    while (1) {
        fputs("tIocSh> ", stdout);
        fflush(stdout);
        if (fgets(line, TELNET_LINE_SIZE, stdin) == NULL) {
                    fflush(stdout);
            break;
        }
            fflush(stdout);
        if (line[0] == '\004') break;  /* VEOF: connection closed */
        size_t len = strlen(line);
        while (len > 0 &&
               (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (len == 0) continue;
        if (strncmp(line, "bye", 3) == 0) {
            fprintf(stdout, "bye\r\n");
            fflush(stdout);
            break;
        }
        iocshCmd(line);
    }
    fprintf(stdout, "\r\ntelnet: session closed\r\n");
    fflush(stdout);
}

rtems_telnetd_config_table rtems_telnetd_config = {
    .command        = telnet_pseudoIocsh,
    .arg            = NULL,
    .priority       = 0,
    .stack_size     = 0,
    .login_check    = NULL,
    .keep_stdio     = false,
    .client_maximum = 0,
    .port           = 23
};

static int rtemsCmdsInitialize() {
    iocshRegister(&netStatFuncDef, netStatCallFunc);
    iocshRegister(&heapSpaceFuncDef, heapSpaceCallFunc);
    iocshRegister(&zonesetFuncDef, &zonesetCallFunc);
    iocshRegister(&rtshellFuncDef, &rtshellCallFunc);
    iocshRegister(&setlogmaskFuncDef, &setlogmaskCallFunc);
    rtems_shell_init_environment();
    std::cout << "RTEMS Commands registered" << std::endl;
    /* Start telnetd for remote IOC shell access */
    {
        auto envp = getenv("RTEMS_TELNETD_PORT");
        if (envp == nullptr || std::string(envp) != "0") {
            int r = (int)rtems_telnetd_initialize();
            if (r != 0)
                std::cout << "error: telnetd: initialize failed: " << r << std::endl;
            else
                std::cout << "telnetd: started" << std::endl;
        }
    }
    return 0;
}

void epicRtemsInit_cmds() {
    epicsRtemsInitRegisterHandler(
        "system", "cmds", rtemsInit_Order_commands, true, rtemsCmdsInitialize);
}
