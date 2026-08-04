/*************************************************************************\
* Copyright (c) 2026 Chris Johns
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <iostream>

#include <string.h>

#include <epicsRtemsInit.h>
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
     * Starting the time provider early (rather than waiting for the
     * initHookAtBeginning hook inside iocInit) is safe: NTPTime_Init/
     * ClockTime_Init use epicsThreadOnce, so the later hook-driven call
     * becomes a no-op. ntpd itself (started here) performs a rough
     * step-sync as soon as it selects a system peer -- see
     * osdTimeRegister()/osdNTP_Run() in osdTime.cpp.
     */
    if (rtemsInit_NTP_server_ip[0] != '\0') {
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
