/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <sys/socket.h>

#include <functional>

#define LOG_TAG "InterfaceController"
#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <cutils/log.h>
#include <logwrap/logwrap.h>
#include <netutils/ifc.h>

#include "InterfaceController.h"
#include "RouteController.h"

using android::base::StringPrintf;
using android::base::ReadFileToString;
using android::base::WriteStringToFile;
using android::net::RouteController;

namespace {

const char ipv6_proc_path[] = "/proc/sys/net/ipv6/conf";

const char ipv4_neigh_conf_dir[] = "/proc/sys/net/ipv4/neigh";

const char ipv6_neigh_conf_dir[] = "/proc/sys/net/ipv6/neigh";

const char proc_net_path[] = "/proc/sys/net";
const char sys_net_path[] = "/sys/class/net";

const char wl_util_path[] = "/vendor/xbin/wlutil";

constexpr int kRouteInfoMinPrefixLen = 48;

// RFC 7421 prefix length.
constexpr int kRouteInfoMaxPrefixLen = 64;

inline bool isNormalPathComponent(const char *component) {
    return (strcmp(component, ".") != 0) &&
           (strcmp(component, "..") != 0) &&
           (strchr(component, '/') == nullptr);
}

inline bool isAddressFamilyPathComponent(const char *component) {
    return strcmp(component, "ipv4") == 0 || strcmp(component, "ipv6") == 0;
}

inline bool isInterfaceName(const char *name) {
    return isNormalPathComponent(name) &&
           (strcmp(name, "default") != 0) &&
           (strcmp(name, "all") != 0);
}

int writeValueToPath(
        const char* dirname, const char* subdirname, const char* basename,
        const char* value) {
    std::string path(StringPrintf("%s/%s/%s", dirname, subdirname, basename));
    return WriteStringToFile(value, path) ? 0 : -1;
}

// Run @fn on each interface as well as 'default' in the path @dirname.
void forEachInterface(const std::string& dirname,
                      std::function<void(const std::string& path, const std::string& iface)> fn) {
    // Run on default, which controls the behavior of any interfaces that are created in the future.
    fn(dirname, "default");
    DIR* dir = opendir(dirname.c_str());
    if (!dir) {
        ALOGE("Can't list %s: %s", dirname.c_str(), strerror(errno));
        return;
    }
    while (true) {
        const dirent *ent = readdir(dir);
        if (!ent) {
            break;
        }
        if ((ent->d_type != DT_DIR) || !isInterfaceName(ent->d_name)) {
            continue;
        }
        fn(dirname, ent->d_name);
    }
    closedir(dir);
}

void setOnAllInterfaces(const char* dirname, const char* basename, const char* value) {
    auto fn = [basename, value](const std::string& path, const std::string& iface) {
        writeValueToPath(path.c_str(), iface.c_str(), basename, value);
    };
    forEachInterface(dirname, fn);
}

void setIPv6UseOutgoingInterfaceAddrsOnly(const char *value) {
    setOnAllInterfaces(ipv6_proc_path, "use_oif_addrs_only", value);
}

std::string getParameterPathname(
        const char *family, const char *which, const char *interface, const char *parameter) {
    if (!isAddressFamilyPathComponent(family)) {
        errno = EAFNOSUPPORT;
        return "";
    } else if (!isNormalPathComponent(which) ||
               !isInterfaceName(interface) ||
               !isNormalPathComponent(parameter)) {
        errno = EINVAL;
        return "";
    }

    return StringPrintf("%s/%s/%s/%s/%s", proc_net_path, family, which, interface, parameter);
}

void setAcceptIPv6RIO(int min, int max) {
    auto fn = [min, max](const std::string& prefix, const std::string& iface) {
        int rv = writeValueToPath(prefix.c_str(), iface.c_str(), "accept_ra_rt_info_min_plen",
                                  std::to_string(min).c_str());
        if (rv != 0) {
            // Only update max_plen if the write to min_plen succeeded. This ordering will prevent
            // RIOs from being accepted unless both min and max are written successfully.
            return;
        }
        writeValueToPath(prefix.c_str(), iface.c_str(), "accept_ra_rt_info_max_plen",
                         std::to_string(max).c_str());
    };
    forEachInterface(ipv6_proc_path, fn);
}

}  // namespace

void InterfaceController::initializeAll() {
    // Initial IPv6 settings.
    // By default, accept_ra is set to 1 (accept RAs unless forwarding is on) on all interfaces.
    // This causes RAs to work or not work based on whether forwarding is on, and causes routes
    // learned from RAs to go away when forwarding is turned on. Make this behaviour predictable
    // by always setting accept_ra to 2.
    setAcceptRA("2");

    // Accept RIOs with prefix length in the closed interval [48, 64].
    setAcceptIPv6RIO(kRouteInfoMinPrefixLen, kRouteInfoMaxPrefixLen);

    setAcceptRARouteTable(-RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX);

    // Enable optimistic DAD for IPv6 addresses on all interfaces.
    setIPv6OptimisticMode("1");

    // Reduce the ARP/ND base reachable time from the default (30sec) to 15sec.
    setBaseReachableTimeMs(15 * 1000);

    // When sending traffic via a given interface use only addresses configured
       // on that interface as possible source addresses.
    setIPv6UseOutgoingInterfaceAddrsOnly("1");
}

int InterfaceController::setEnableIPv6(const char *interface, const int on) {
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    // When disable_ipv6 changes from 1 to 0, the kernel starts autoconf.
    // When disable_ipv6 changes from 0 to 1, the kernel clears all autoconf
    // addresses and routes and disables IPv6 on the interface.
    const char *disable_ipv6 = on ? "0" : "1";
    return writeValueToPath(ipv6_proc_path, interface, "disable_ipv6", disable_ipv6);
}

int InterfaceController::setAcceptIPv6Ra(const char *interface, const int on) {
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    // Because forwarding can be enabled even when tethering is off, we always
    // use mode "2" (accept RAs, even if forwarding is enabled).
    const char *accept_ra = on ? "2" : "0";
    return writeValueToPath(ipv6_proc_path, interface, "accept_ra", accept_ra);
}

int InterfaceController::setAcceptIPv6Dad(const char *interface, const int on) {
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    const char *accept_dad = on ? "1" : "0";
    return writeValueToPath(ipv6_proc_path, interface, "accept_dad", accept_dad);
}

int InterfaceController::setIPv6DadTransmits(const char *interface, const char *value) {
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    return writeValueToPath(ipv6_proc_path, interface, "dad_transmits", value);
}

int InterfaceController::setIPv6PrivacyExtensions(const char *interface, const int on) {
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    // 0: disable IPv6 privacy addresses
    // 0: enable IPv6 privacy addresses and prefer them over non-privacy ones.
    return writeValueToPath(ipv6_proc_path, interface, "use_tempaddr", on ? "2" : "0");
}

// Enables or disables IPv6 ND offload. This is useful for 464xlat on wifi, IPv6 tethering, and
// generally implementing IPv6 neighbour discovery and duplicate address detection properly.
// TODO: This should be implemented in wpa_supplicant via driver commands instead.
int InterfaceController::setIPv6NdOffload(char* interface, const int on) {
    // Only supported on Broadcom chipsets via wlutil for now.
    if (access(wl_util_path, X_OK) == 0) {
        const char *argv[] = {
            wl_util_path,
            "-a",
            interface,
            "ndoe",
            on ? "1" : "0"
        };
        int ret = android_fork_execvp(ARRAY_SIZE(argv), const_cast<char**>(argv), NULL,
                                      false, false);
        ALOGD("%s ND offload on %s: %d (%s)",
              (on ? "enabling" : "disabling"), interface, ret, strerror(errno));
        return ret;
    } else {
        return 0;
    }
}

void InterfaceController::setAcceptRA(const char *value) {
    setOnAllInterfaces(ipv6_proc_path, "accept_ra", value);
}

// |tableOrOffset| is interpreted as:
//     If == 0: default. Routes go into RT6_TABLE_MAIN.
//     If > 0: user set. Routes go into the specified table.
//     If < 0: automatic. The absolute value is intepreted as an offset and added to the interface
//             ID to get the table. If it's set to -1000, routes from interface ID 5 will go into
//             table 1005, etc.
void InterfaceController::setAcceptRARouteTable(int tableOrOffset) {
    std::string value(StringPrintf("%d", tableOrOffset));
    setOnAllInterfaces(ipv6_proc_path, "accept_ra_rt_table", value.c_str());
}

int InterfaceController::setMtu(const char *interface, const char *mtu)
{
    if (!isIfaceName(interface)) {
        errno = ENOENT;
        return -1;
    }
    return writeValueToPath(sys_net_path, interface, "mtu", mtu);
}

int InterfaceController::addAddress(const char *interface,
        const char *addrString, int prefixLength) {
    return ifc_add_address(interface, addrString, prefixLength);
}

int InterfaceController::delAddress(const char *interface,
        const char *addrString, int prefixLength) {
    return ifc_del_address(interface, addrString, prefixLength);
}

int InterfaceController::getParameter(
        const char *family, const char *which, const char *interface, const char *parameter,
        std::string *value) {
    const std::string path(getParameterPathname(family, which, interface, parameter));
    if (path.empty()) {
        return -errno;
    }
    return ReadFileToString(path, value) ? 0 : -errno;
}

int InterfaceController::setParameter(
        const char *family, const char *which, const char *interface, const char *parameter,
        const char *value) {
    const std::string path(getParameterPathname(family, which, interface, parameter));
    if (path.empty()) {
        return -errno;
    }
    return WriteStringToFile(value, path) ? 0 : -errno;
}

void InterfaceController::setBaseReachableTimeMs(unsigned int millis) {
    std::string value(StringPrintf("%u", millis));
    setOnAllInterfaces(ipv4_neigh_conf_dir, "base_reachable_time_ms", value.c_str());
    setOnAllInterfaces(ipv6_neigh_conf_dir, "base_reachable_time_ms", value.c_str());
}

void InterfaceController::setIPv6OptimisticMode(const char *value) {
    setOnAllInterfaces(ipv6_proc_path, "optimistic_dad", value);
    setOnAllInterfaces(ipv6_proc_path, "use_optimistic", value);
}
