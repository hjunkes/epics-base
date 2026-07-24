/*************************************************************************\
* Copyright (c) 2026 Heinz Junkes
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

/*
 * BeagleBone Black boot-config bridge.
 *
 * Unlike mvme6100/MOTLoad, BBB has no NVRAM: it boots over DHCP on
 * cpsw0, with the startup script path and NFS export delivered via a
 * custom DHCP option (129, "rtems_cmdline"), and the NTP server via the
 * standard "ntp-servers" option. epicsRtemsInit_net.cpp's own handler
 * only waits for link-up, not for a DHCP lease, and the rest of the new
 * init framework (epicsRtemsInit_nfs.cpp, _ntp.cpp, _ioc.cpp) expects
 * plain env vars (RTEMS_NFS_MOUNT_PATH, RTEMS_NET_NTP_IP,
 * RTEMS_BOOT_CMD_LINE) to already be set. This file bridges the two,
 * reusing the dhcpcd hook mechanism the legacy posix/rtems_init.c relied
 * on -- it still fires the same way when dhcpcd is started indirectly
 * via epicsRtemsInit_net.cpp's rtems_bsd_run_etc_rc_conf() call.
 *
 * Registration is wired in from epicsRtemsInit_net.cpp's own
 * epicRtemsInit_net(), guarded by BSP_beagleboneblack, rather than via
 * static-constructor self-registration: this translation unit lives in
 * a static library (librtemsCom.a) and nothing else references its
 * symbols, so with --gc-sections its object file (and any global
 * constructors in it) would simply never be pulled into the final IOC
 * link.
 */

#if defined(BSP_beagleboneblack)

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <epicsEvent.h>
#include <epicsRtemsInit.h>

#include <rtems/dhcpcd.h>

namespace {

epicsEventId bbbDhcpDone;

char bbbInterface[16]       = "?";
char bbbReason[16]          = "?";
char bbbHostName[32]        = "RTEMShost";
char bbbNtpServerIp[16]     = "";
char bbbTftpServerName[128] = "";
char bbbBootCmdLine[128]    = "";

void bbbDhcpcdHookHandler(rtems_dhcpcd_hook *hook, char *const *env) {
    (void) hook;
    bool bound = false;

    struct dhcp_var {
        const char* name;
        char* var;
        size_t varsize;
    } vars[] = {
        { "interface",            bbbInterface,       sizeof(bbbInterface) },
        { "reason",                bbbReason,          sizeof(bbbReason) },
        { "new_host_name",         bbbHostName,        sizeof(bbbHostName) },
        { "new_ntp_servers",       bbbNtpServerIp,     sizeof(bbbNtpServerIp) },
        { "new_tftp_server_name",  bbbTftpServerName,  sizeof(bbbTftpServerName) },
        { "new_rtems_cmdline",     bbbBootCmdLine,     sizeof(bbbBootCmdLine) },
        { nullptr, nullptr, 0 }
    };

    for (; *env != nullptr; ++env) {
        for (auto* v = vars; v->name != nullptr; ++v) {
            size_t namelen = std::strlen(v->name);
            if (std::strncmp(*env, v->name, namelen) != 0 ||
                (*env)[namelen] != '=') {
                continue;
            }
            const char* value = *env + namelen + 1;
            size_t valuelen = std::strlen(value);
            /* Ignore everything but interface/reason until BOUND/REBIND */
            if (v->var != bbbReason && v->var != bbbInterface && !bound) {
                /* skip */
            } else if (valuelen >= v->varsize) {
                std::cout << "error: bbb net: value too long for "
                          << v->name << std::endl;
            } else {
                std::memcpy(v->var, value, valuelen);
                v->var[valuelen] = '\0';
                if (v->var == bbbReason &&
                    (std::strcmp(bbbReason, "BOUND") == 0 ||
                     std::strcmp(bbbReason, "REBIND") == 0)) {
                    bound = true;
                }
            }
            break;
        }
    }

    if (bound) {
        sethostname(bbbHostName, std::strlen(bbbHostName));
        epicsEventSignal(bbbDhcpDone);
    }
}

rtems_dhcpcd_hook bbbDhcpcdHook;

/*
 * dhcpcd needs a custom option 129 definition to expose the EPICS
 * startup-script path ("rtems_cmdline") in the hook's environment --
 * without it dhcpcd has no name for that option and won't report it.
 */
void writeDhcpcdConf() {
    struct stat st;
    if (stat("/etc/dhcpcd.conf", &st) == 0) {
        return; /* already present, assume it's ours */
    }
    std::ofstream conf("/etc/dhcpcd.conf", std::ios::binary);
    conf << "clientid EPICS boot\n"
            "define 129 string rtems_cmdline\n"
            "nodhcp6\n"
            "ipv4only\n"
            "leasetime 86400\n"
            "option ntp-servers\n"
            "option rtems_cmdline\n"
            "option tftp-server-name\n"
            "option bootfile-name\n";
    /*
     * Deliberately no "timeout" directive here: dhcpcd is launched by
     * epicsRtemsInit_net.cpp's rtems_bsd_rc_conf_from_env()/
     * rtems_bsd_run_etc_rc_conf() with "--nobackground --timeout N" on
     * its command line (N from that call's dhcp_timeout argument). A
     * "timeout 0" in this file (meaning "never give up, stay in the
     * foreground") previously combined with --nobackground to block
     * indefinitely if the lease didn't arrive in time -- let the
     * command-line timeout be the only one in effect.
     */
}

/*
 * Build the "server:export:path" form epicsRtemsInit_nfs.cpp expects.
 *
 * On this network the DHCP "tftp-server-name" option already embeds
 * the NFS export path (e.g. "141.14.131.192:/Volumes/Epics"), matching
 * the convention the legacy posix/rtems_init.c code used -- it is not
 * a bare server name/IP. cmdline is the full absolute startup-script
 * path and is expected to start with that same export path as a
 * prefix (e.g. "/Volumes/Epics/BEAGLEBONE/.../st.cmd"); the remainder
 * becomes the mount-relative path component.
 *
 * "server:/export/path" + "/export/path/rest/of/cmdline" ->
 * "server:/export/path:rest/of/cmdline"
 */
std::string buildNfsMountPath(
    const std::string& tftpServerName, const std::string& cmdline) {
    auto colon = tftpServerName.find(':');
    if (colon == std::string::npos) {
        return {};
    }
    std::string server = tftpServerName.substr(0, colon);
    std::string nfsExport = tftpServerName.substr(colon + 1);

    std::string rest = cmdline;
    if (rest.compare(0, nfsExport.size(), nfsExport) == 0) {
        rest.erase(0, nfsExport.size());
    }
    if (!rest.empty() && rest.front() == '/') {
        rest.erase(0, 1);
    }
    return server + ':' + nfsExport + ':' + rest;
}

} // namespace

int rtemsBbbNetPreInitialize() {
    if (getenv("RTEMS_NET_IFACE_1") == nullptr) {
        setenv("RTEMS_NET_IFACE_1", "cpsw0", 1);
    }
    writeDhcpcdConf();
    bbbDhcpDone = epicsEventMustCreate(epicsEventEmpty);
    bbbDhcpcdHook.name = "ioc boot";
    bbbDhcpcdHook.handler = bbbDhcpcdHookHandler;
    rtems_dhcpcd_add_hook(&bbbDhcpcdHook);
    return 0;
}

int rtemsBbbNetPostInitialize() {
    epicsEventWaitStatus stat = epicsEventWaitWithTimeout(bbbDhcpDone, 600.0);
    if (stat == epicsEventWaitTimeout) {
        std::cout << "warning: bbb net: DHCP bind timed out" << std::endl;
        return 0; /* let boot continue; interface may still be usable */
    }

    if (bbbNtpServerIp[0] != '\0') {
        setenv("RTEMS_NET_NTP_IP", bbbNtpServerIp, 1);
    }
    if (bbbTftpServerName[0] != '\0' && bbbBootCmdLine[0] != '\0') {
        setenv("RTEMS_BOOT_CMD_LINE", bbbBootCmdLine, 1);
        auto mountPath = buildNfsMountPath(bbbTftpServerName, bbbBootCmdLine);
        if (!mountPath.empty()) {
            setenv("RTEMS_NFS_MOUNT_PATH", mountPath.c_str(), 1);
        }
        std::cout << "bbb net: DHCP bound, boot script "
                  << bbbBootCmdLine << std::endl;
    } else {
        std::cout << "warning: bbb net: no tftp-server-name/rtems_cmdline "
                     "DHCP options received -- NFS mount and boot script "
                     "path not set" << std::endl;
    }
    return 0;
}

#endif /* BSP_beagleboneblack */
