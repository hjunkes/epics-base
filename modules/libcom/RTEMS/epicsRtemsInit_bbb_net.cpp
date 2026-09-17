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
 * cpsw0, with the NFS export and the startup script path delivered by
 * the DHCP server and the NTP server via the standard "ntp-servers"
 * option. epicsRtemsInit_net.cpp's own handler only waits for link-up,
 * not for a DHCP lease, and the rest of the new init framework
 * (epicsRtemsInit_nfs.cpp, _ntp.cpp, _ioc.cpp) expects plain env vars
 * (RTEMS_NFS_MOUNT_PATH, RTEMS_NET_NTP_IP, RTEMS_BOOT_CMD_LINE) to
 * already be set. This file bridges the two, reusing the dhcpcd hook
 * mechanism the legacy posix/rtems_init.c relied on -- it still fires
 * the same way when dhcpcd is started indirectly via
 * epicsRtemsInit_net.cpp's rtems_bsd_run_etc_rc_conf() call.
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <epicsEvent.h>
#include <epicsRtemsInit.h>

#include <rtems/dhcpcd.h>

namespace {

/*
 * Dump every dhcpcd environment variable as it arrives. Useful when
 * commissioning a DHCP server -- it shows exactly which of 129 / 77 /
 * 67 / 17 / 66 / the BOOTP file field that server actually delivers,
 * which is how the executable-in-option-67 trap below was found.
 *
 * Off by default; turn on when commissioning a DHCP server.
 */
static constexpr bool bbb_net_verbose = false;

epicsEventId bbbDhcpDone;

char bbbInterface[16]       = "?";
char bbbReason[16]          = "?";
char bbbHostName[32]        = "RTEMShost";
/*
 * ntp-servers (option 42) is an "array ipaddress" for dhcpcd, i.e. the
 * value is a space-separated list, not a single address -- size the
 * buffer for a list and hand only the first entry to the NTP code,
 * whose rtemsInit_NTP_server_ip is a char[16].
 */
char bbbNtpServers[80]      = "";
/* NFS export, as "server:/export" -- option 66 or, failing that, 17 */
char bbbTftpServerName[128] = "";
char bbbRootPath[128]       = "";
/* st.cmd path -- option 129, else option 77, else option 17 */
char bbbRtemsCmdline[128]   = "";
char bbbUserClass[128]      = "";
/*
 * Captured for the log only. Measured on this network 2026-09-17:
 * bootfile-name (67) carries the IOC *executable* and the BOOTP file
 * field carries u-boot's TFTP image ("beaglebone/beaglePyroIOC.img"),
 * so neither is a startup-script source -- see the note above the
 * candidate list in rtemsBbbNetPostInitialize().
 */
char bbbBootfileName[128]   = "";
char bbbFilename[128]       = "";

void bbbDhcpcdHookHandler(rtems_dhcpcd_hook *hook, char *const *env) {
    (void) hook;
    bool bound = false;

    struct dhcp_var {
        const char* name;
        char* var;
        size_t varsize;
    } vars[] = {
        { "interface",             bbbInterface,       sizeof(bbbInterface) },
        { "reason",                bbbReason,          sizeof(bbbReason) },
        { "new_host_name",         bbbHostName,        sizeof(bbbHostName) },
        { "new_ntp_servers",       bbbNtpServers,      sizeof(bbbNtpServers) },
        { "new_tftp_server_name",  bbbTftpServerName,  sizeof(bbbTftpServerName) },
        { "new_root_path",         bbbRootPath,        sizeof(bbbRootPath) },
        { "new_rtems_cmdline",     bbbRtemsCmdline,    sizeof(bbbRtemsCmdline) },
        { "new_user_class",        bbbUserClass,       sizeof(bbbUserClass) },
        { "new_bootfile_name",     bbbBootfileName,    sizeof(bbbBootfileName) },
        { "new_filename",          bbbFilename,        sizeof(bbbFilename) },
        { nullptr, nullptr, 0 }
    };

    for (; *env != nullptr; ++env) {
        if (bbb_net_verbose) {
            std::cout << "bbb net: dhcpcd ---> '" << *env << "'" << std::endl;
        }
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
 * Write the dhcpcd configuration the IOC needs.
 *
 * A DHCP server only returns an option if the client asked for it in
 * the parameter request list (option 55), so every non-default option
 * this file consumes has to appear in an "option" line here.
 *
 * The catch: rtems-libbsd bundles dhcpcd 6.2.1, and in that version a
 * custom option "define"d in /etc/dhcpcd.conf can never make it into
 * the request list. read_config() parses dhcpcd's *embedded*
 * definitions first and moves them into the global dhcp_opts[], then
 * parses this file into a fresh per-interface ifo->dhcp_override[].
 * "option <name>" resolves through set_option_space()/
 * make_option_mask(), which search dhcp_opts[] only, and the request
 * list in dhcp.c's make_message() is built by walking dhcp_opts[] as
 * well. So "option rtems_cmdline" would fail with "unknown option",
 * and even a numeric "option 129" has nothing to match.
 *
 * Decoding, on the other hand, does consult ifo->dhcp_override[] (see
 * dhcp_env()), so the "define" below is still worth keeping: a server
 * that sends option 129 unsolicited will have it turned into
 * new_rtems_cmdline for the hook above. It just cannot be *requested*.
 *
 * Hence the st.cmd path is taken from whichever of these arrives:
 *   1. option 129 (rtems_cmdline) -- only if the server pushes it,
 *   2. option 67  (bootfile-name) -- requested below,
 *   3. the BOOTP 'file' header field, which dhcpcd always exports as
 *      new_filename with no request needed (ISC dhcpd's "filename"
 *      statement). This is the one that works against a stock server.
 *
 * Note also: option names are spelled with underscores here. dhcpcd
 * matches them with strcmp() against its own table (ntp_servers,
 * tftp_server_name, ...); a hyphenated spelling silently fails to
 * parse and the option is then never requested.
 *
 * Deliberately no "clientid" directive: with none, dhcpcd sends no
 * option 61 at all and the server matches on the chaddr, so per-board
 * "hardware ethernet" host declarations work. A literal shared client
 * id would make every BBB look like the same client.
 *
 * Deliberately no "timeout" directive either: dhcpcd is launched by
 * epicsRtemsInit_net.cpp's rtems_bsd_rc_conf_from_env()/
 * rtems_bsd_run_etc_rc_conf() with "--nobackground --timeout N" on its
 * command line (N from that call's dhcp_timeout argument). A
 * "timeout 0" in this file (meaning "never give up, stay in the
 * foreground") previously combined with --nobackground to block
 * indefinitely if the lease didn't arrive in time -- let the
 * command-line timeout be the only one in effect.
 */
void writeDhcpcdConf() {
    struct stat st;
    if (stat("/etc/dhcpcd.conf", &st) == 0) {
        return; /* already present, assume it's ours */
    }
    std::ofstream conf("/etc/dhcpcd.conf", std::ios::binary);
    conf << "nodhcp6\n"
            "ipv4only\n"
            "leasetime 86400\n"
            "define 129 string rtems_cmdline\n"
            "option host_name\n"
            "option ntp_servers\n"
            "option tftp_server_name\n"
            "option root_path\n"
            "option user_class\n"
            "require dhcp_server_identifier\n";
}

/*
 * The fallback options below all have other legitimate users, so a value
 * is only accepted once its shape says it is the thing we are looking
 * for. This matters most for the BOOTP 'file' header field: u-boot's
 * "dhcp" command autoloads it over TFTP (see the board's uEnv.txt), so
 * on this hardware it normally holds the RTEMS image path, e.g.
 * "bbb/rtems.img" -- taking that as the startup script would be wrong
 * and confusing. A DHCP server may of course hand out a different file
 * field per vendor-class-identifier (u-boot sends "U-Boot.armv7...",
 * dhcpcd sends "dhcpcd-<version>"), in which case the field really is
 * ours and the check below passes it through.
 */

/* An NFS export spec is "server:/export" */
bool looksLikeExport(const char* s) {
    return std::strchr(s, ':') != nullptr;
}

/*
 * A startup script path is absolute and, because epicsRtemsInit_nfs.cpp
 * mounts the export and then chdirs below it, has to sit under the
 * export we were given.
 */
bool looksLikeCmdline(const char* s, const std::string& nfsExport) {
    if (s[0] != '/') {
        return false;
    }
    if (nfsExport.empty()) {
        return true;
    }
    return std::strncmp(s, nfsExport.c_str(), nfsExport.size()) == 0;
}

const char* firstMatching(
    const char* const* candidates, bool (*pred)(const char*)) {
    for (; *candidates != nullptr; ++candidates) {
        if ((*candidates)[0] != '\0' && pred(*candidates)) {
            return *candidates;
        }
    }
    return "";
}

/* ntp-servers is a space-separated list; the NTP code wants one address */
std::string firstAddress(const char* list) {
    std::string s = list;
    auto sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

/*
 * Build the "server:export:path" form epicsRtemsInit_nfs.cpp expects.
 *
 * On this network the DHCP "tftp-server-name" option already embeds
 * the NFS export path (e.g. "141.14.131.192:/Volumes/Epics"), matching
 * the convention the legacy posix/rtems_init.c code used -- it is not
 * a bare server name/IP. "root-path" (option 17) is accepted in the
 * same form as a more standards-flavoured alternative. cmdline is the
 * full absolute startup-script path and is expected to start with that
 * same export path as a prefix (e.g.
 * "/Volumes/Epics/BEAGLEBONE/.../st.cmd"); the remainder becomes the
 * mount-relative path component.
 *
 * "server:/export/path" + "/export/path/rest/of/cmdline" ->
 * "server:/export/path:rest/of/cmdline"
 */
std::string buildNfsMountPath(
    const std::string& nfsBase, const std::string& cmdline) {
    auto colon = nfsBase.find(':');
    if (colon == std::string::npos) {
        return {};
    }
    std::string server = nfsBase.substr(0, colon);
    std::string nfsExport = nfsBase.substr(colon + 1);

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
    /* dhcpcd keeps its lease files in DBDIR ("/var/db", see its
     * config.h); without the directory every lease write fails with a
     * syslog error. Not fatal, but noisy and it loses the lease across
     * a reboot. */
    std::error_code ec;
    std::filesystem::create_directories("/var/db", ec);
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

    if (bbbNtpServers[0] != '\0') {
        auto ntp = firstAddress(bbbNtpServers);
        setenv("RTEMS_NET_NTP_IP", ntp.c_str(), 1);
    }

    /*
     * root-path (17) can plausibly carry either the export or the script
     * path depending on how the server is set up, so both lists include
     * it and the shape decides which role it takes.
     */
    const char* exportCandidates[] = {
        bbbTftpServerName, bbbRootPath, nullptr };
    const char* nfsBase = firstMatching(exportCandidates, looksLikeExport);

    std::string nfsExport;
    if (nfsBase[0] != '\0') {
        nfsExport = std::strchr(nfsBase, ':') + 1;
    }

    /*
     * Option 129 is ours alone, so it is taken as given; the rest have
     * to look like a script path below the export.
     *
     * Only user-class (77) and root-path (17) are candidates. Two
     * plausible-looking options are deliberately NOT here, as measured
     * against this site's server on 2026-09-17:
     *
     *   - bootfile-name (67) held the IOC executable
     *     (".../bin/RTEMS-beagleboneblack/beaglePyroIOC"), which is
     *     absolute and below the export and would therefore have passed
     *     looksLikeCmdline() while being the wrong file entirely.
     *   - the BOOTP file field held "beaglebone/beaglePyroIOC.img",
     *     u-boot's TFTP image. Relative, so the guard rejects it, but
     *     it is not ours to consume in the first place.
     *
     * 77 is the only one of these that a stock dhcpcd can actually put
     * in the request list, so it is what makes this work against a
     * server that answers only what was asked for.
     */
    const char* cmdline = bbbRtemsCmdline;
    if (cmdline[0] == '\0') {
        const char* cmdlineCandidates[] = {
            bbbUserClass, bbbRootPath, nullptr };
        for (auto** c = cmdlineCandidates; *c != nullptr; ++c) {
            if ((*c)[0] != '\0' && looksLikeCmdline(*c, nfsExport)) {
                cmdline = *c;
                break;
            }
        }
    }

    if (nfsBase[0] != '\0' && cmdline[0] != '\0') {
        setenv("RTEMS_BOOT_CMD_LINE", cmdline, 1);
        auto mountPath = buildNfsMountPath(nfsBase, cmdline);
        if (!mountPath.empty()) {
            setenv("RTEMS_NFS_MOUNT_PATH", mountPath.c_str(), 1);
        }
        std::cout << "bbb net: DHCP bound, boot script "
                  << cmdline << std::endl;
    } else {
        std::cout << "warning: bbb net: no NFS export "
                     "(tftp-server-name/root-path) and/or boot script "
                     "(rtems_cmdline/bootfile-name/filename) delivered by "
                     "DHCP -- NFS mount and boot script path not set"
                  << std::endl;
    }
    return 0;
}

#endif /* BSP_beagleboneblack */
