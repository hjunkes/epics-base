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
 * Query the interface's current link state directly, instead of only
 * watching for a future RTM_IFINFO transition. rtems_bsd_run_etc_rc_conf()
 * already brings interfaces up (and, on this hardware, cpsw0's link
 * flaps up/down/up during that process) before this is ever called, so
 * waiting solely on a routing-socket event risks watching for a
 * transition that already happened and will not repeat -- which just
 * burns the full timeout every boot.
 */
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
static int wait_for_link_up(const char *ifname, int timeout_secs) {
    if (link_is_up(ifname)) {
        std::cout << ifname << ": link already up" << std::endl;
        return 0;
    }

    int sock = socket(PF_ROUTE, SOCK_RAW, 0);
    if (sock < 0) {
        std::cout << "error: net: route sock open: " << std::strerror(errno)
                  << std::endl;
        return 3;
    }

    std::cout << ifname << ": waiting for link (timeout " << timeout_secs << "s)... "
              << std::flush;

    struct timeval tv = { .tv_sec = timeout_secs, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        char buf[sizeof(struct if_msghdr) + sizeof(struct sockaddr_dl)];
        auto n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0) {
            break;
        }
        struct rt_msghdr* rtm = (struct rt_msghdr*)(void*)buf;
        if (rtm->rtm_type == RTM_IFINFO) {
            struct if_msghdr* ifm = (struct if_msghdr*)(void*)buf;
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
        if (env != nullptr && std::strlen(env) != 0) {
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
