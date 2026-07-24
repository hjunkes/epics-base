/*************************************************************************\
* Copyright (c) 2026 Chris Johns
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <iostream>

#include <string.h>
#include <time.h>
#include <errno.h>

#include <epicsRtemsInit.h>
#include <epicsNtp.h>
#include <osdTime.h>

char rtemsInit_NTP_server_ip[16] = "";

static int rtemsNTPInitialize() {
    auto envp = getenv("RTEMS_NET_NTP_IP");
    if (envp != nullptr) {
        ::strlcpy(
            rtemsInit_NTP_server_ip, envp, sizeof(rtemsInit_NTP_server_ip));
        std::cout << "NTP IP address: " << rtemsInit_NTP_server_ip
                  << std::endl;
    }

    /*
     * One-shot SNTP to set the OS clock immediately, before iocInit.
     * Without this the clock stays unsynchronised until the ntpd-based
     * osdTimeRegister() hook (run at initHookAtBeginning, i.e. inside
     * iocInit) eventually converges, which is far too slow. Calling
     * osdTimeRegister() here as well is safe: NTPTime_Init/ClockTime_Init
     * use epicsThreadOnce, so the later call made by the initHookAtBeginning
     * hook becomes a no-op.
     */
    if (rtemsInit_NTP_server_ip[0] != '\0') {
        struct timespec now;
        std::cout << "One-shot NTP time set from " << rtemsInit_NTP_server_ip
                  << std::endl;
        if (epicsNtpGetTime(rtemsInit_NTP_server_ip, &now) == 0) {
            if (clock_settime(CLOCK_REALTIME, &now) == 0) {
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "%Y/%m/%d %H:%M:%S",
                         gmtime(&now.tv_sec));
                std::cout << "Clock set to " << tbuf << " UTC" << std::endl;
            } else {
                std::cout << "WARNING: clock_settime failed: "
                          << strerror(errno) << std::endl;
            }
        } else {
            std::cout << "WARNING: epicsNtpGetTime failed, clock not set"
                      << std::endl;
        }
        std::cout << "Initialising EPICS time provider..." << std::endl;
        osdTimeRegister();
    }
    return 0;
}

void epicRtemsInit_ntp() {
    epicsRtemsInitRegisterHandler(
        "system", "ntp.ip", rtemsInit_Order_post_net_services + 50,
        true, rtemsNTPInitialize);
}
