/*************************************************************************\
* Copyright (c) 2026 Chris Johns
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/socket.h>

#include <epicsRtemsInit.h>

#include <rtems.h>

#include <rtems/bsd/bsd.h>
#include <machine/rtems-bsd-rc-conf.h>
#include <machine/rtems-bsd-rc-conf-env.h>

static constexpr bool net_verbose = false;

/*
 * Block until the named interface reports link up via an RTM_IFINFO routing
 * message, or until timeout_secs elapses.
 *
 * route_sock must already be open (opened before the interface was configured
 * so that no RTM_IFINFO event can be missed).
 *
 * This function was modeled after the RTM_IFINFO handling in
 * rtems-libbsd/dhcpcd/if-bsd.c (manage_link).  There's a
 * simpler implementation in a test that uses a sleep loop in
 * rtems-libbsd/testsuite/include/rtems/bsd/test/default-init.h, but
 * we chose this more responsive event based implementation.
 *
 * Returns 0 if link came up, -1 on timeout or an error code.
 */
/* Current link state of an interface, without waiting for anything. */
static bool link_is_up(const char *ifname) {
    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        return false;
    }
    bool up = false;
    for (struct ifaddrs *ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_LINK ||
            ifa->ifa_data == nullptr || strcmp(ifa->ifa_name, ifname) != 0) {
            continue;
        }
        auto* data = (struct if_data*) ifa->ifa_data;
        up = data->ifi_link_state == LINK_STATE_UP;
        break;
    }
    freeifaddrs(ifap);
    return up;
}

/*
 * Wait until the named interface reports link up.
 *
 * Order matters here. The routing socket is opened first, so that any
 * transition from this point on is queued on it; only then is the
 * current state examined. Checking first and opening afterwards would
 * leave a window in which the link comes up unseen and the subsequent
 * wait has nothing left to wait for.
 *
 * The check itself is needed because this runs after
 * rtems_bsd_run_etc_rc_conf() has already configured the interfaces, so
 * on most boots the link is up before we are called and no further
 * RTM_IFINFO transition is coming -- a purely edge-triggered wait then
 * burns its entire timeout on every boot.
 */
static int wait_for_link_up(const char *ifname, int timeout_secs) {
    int sock = socket(PF_ROUTE, SOCK_RAW, 0);
    if (sock < 0) {
        std::cout << "error: net: route sock open: " << std::strerror(errno)
                  << std::endl;
        return 3;
    }

    if (link_is_up(ifname)) {
        std::cout << ifname << ": link already up" << std::endl;
        close(sock);
        return 0;
    }

    std::cout << ifname << ": waiting for link (timeout " << timeout_secs << "s)... "
              << std::flush;

    struct timeval tv = { .tv_sec = timeout_secs, .tv_usec = 0 };
    int r = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (r == 0) {
        while (true) {
            char buf[sizeof(struct if_msghdr) + sizeof(struct sockaddr_dl)];
            auto n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            struct rt_msghdr* rtm = reinterpret_cast<struct rt_msghdr*>(buf);
            if (rtm->rtm_type == RTM_IFINFO) {
                struct if_msghdr* ifm = reinterpret_cast<struct if_msghdr*>(buf);
                char name[IFNAMSIZ];
                if (if_indextoname(ifm->ifm_index, name) != nullptr &&
                    strcmp(name, ifname) == 0 &&
                    ifm->ifm_data.ifi_link_state == LINK_STATE_UP) {
                    std::cout << "up" << std::endl;
                    close(sock);
                    return 0;
                }
            }
        }
    }

    /* recv returned <= 0: SO_RCVTIMEO expired (EAGAIN) or socket error */
    std::cout << "timeout" << std::endl;
    close(sock);
    return -1;
}

static void rtemsNetMakeHosts() {
    std::ofstream hosts("/etc/hosts", std::ios::binary);
    hosts << "127.0.0.1       localhost" << std::endl;
}

static int rtemsNetInitialize() {
    rtems_bsd_rc_conf_from_env(true, 210, 20, net_verbose);
    rtems_bsd_resolv_conf_from_env(net_verbose);
    rtemsNetMakeHosts();
    auto sc = rtems_bsd_initialize();
    if (sc != RTEMS_SUCCESSFUL) {
        std::cout << "error: net: initialize networking: " << rtems_status_text(sc)
                  << std::endl;
        return 1;
    }
    auto r = rtems_bsd_run_etc_rc_conf(30, net_verbose);
    if (r < 0) {
        std::cout << "error: net: start networking: " << std::strerror(errno)
                  << std::endl;
        return 2;
    }
    for (int unit = 1; unit <= 4; ++unit) {
        std::ostringstream oss;
        oss << "RTEMS_NET_IFACE_" << unit;
        auto env = getenv(oss.str().c_str());
        if (env != nullptr && env[0] != '\0') {
            wait_for_link_up(env, 20);
        }
    }
    return 0;
}

#if defined(BSP_beagleboneblack)
/* Implemented in epicsRtemsInit_bbb_net.cpp: bridges DHCP-delivered
 * boot config (no NVRAM on this board) into the env vars the rest of
 * this init framework expects. */
extern int rtemsBbbNetPreInitialize();
extern int rtemsBbbNetPostInitialize();
#endif

void epicRtemsInit_net() {
    epicsRtemsInitRegisterHandler(
        "system", "net", rtemsInit_Order_net, true, rtemsNetInitialize);
#if defined(BSP_beagleboneblack)
    epicsRtemsInitRegisterHandler(
        "bbb", "net.dhcp_pre", rtemsInit_Order_net - 10, true,
        rtemsBbbNetPreInitialize);
    epicsRtemsInitRegisterHandler(
        "bbb", "net.dhcp_post", rtemsInit_Order_net + 10, true,
        rtemsBbbNetPostInitialize);
#endif
}

#if defined(HAVE_MOTLOAD) || defined(HAVE_PPCBUG) || defined(__mcf528x__)
extern "C" int setNetConfigEnvFromNVRAM(char*, size_t);

static int rtemsNetNVRAM() {
    static char ntp_server_ip[16];
    (void) setNetConfigEnvFromNVRAM(ntp_server_ip, sizeof(ntp_server_ip));
    /*
     * Applied whether or not static configuration was found: the NTP server
     * address comes from NVRAM (epics-ntpserver, else the boot server) and is
     * equally valid when the interface is configured by DHCP.
     */
    if (ntp_server_ip[0] != '\0') {
        setenv("EPICS_TS_NTP_INET", ntp_server_ip, 0);
        setenv("RTEMS_NET_NTP_IP", ntp_server_ip, 0);
    }
    return 0;
}

static epicsRtemsInitRegister rtemsNetNVRAM_reg(
    "system", "net.nvram", 450, true, rtemsNetNVRAM);
#endif
