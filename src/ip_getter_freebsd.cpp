// FreeBSD/OpenBSD ioctl implementation for IPv6 address retrieval
// Reference: goddns/internal/platform/ifaddr/freebsd_ioctl.go

#include "ip_getter.hpp"
#include "log.hpp"

#if defined(__FreeBSD__) || defined(__OpenBSD__)

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#if defined(__FreeBSD__)
#include <netinet6/in6_var.h>
#endif

#if defined(__OpenBSD__)
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#endif

namespace ip_getter {

namespace {

constexpr uint32_t ND6_INFINITE_LIFETIME = 0xFFFFFFFFU;

/// Get IPv6 address lifetime information using ioctl
static int get_ipv6_lifetime(const std::string& ifname,
                              const struct sockaddr_in6& sin6,
                              uint32_t& pltime_out,
                              uint32_t& vltime_out) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == -1) {
        return -1;
    }

#if defined(__FreeBSD__)
    struct in6_ifreq ifr6;
    memset(&ifr6, 0, sizeof(ifr6));
    strncpy(ifr6.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    ifr6.ifr_addr = sin6;

    if (ioctl(s, SIOCGIFALIFETIME_IN6, &ifr6) == -1) {
        close(s);
        return -1;
    }

    struct in6_addrlifetime lt = ifr6.ifr_ifru.ifru_lifetime;
    time_t now = time(nullptr);

    pltime_out = (lt.ia6t_preferred != (time_t)-1)
                     ? (uint32_t)(lt.ia6t_preferred - now)
                     : ND6_INFINITE_LIFETIME;
    vltime_out = (lt.ia6t_expire != (time_t)-1)
                     ? (uint32_t)(lt.ia6t_expire - now)
                     : ND6_INFINITE_LIFETIME;

#elif defined(__OpenBSD__)
    // OpenBSD uses different ioctl interface
    struct in6_ifreq ifr6;
    memset(&ifr6, 0, sizeof(ifr6));
    strncpy(ifr6.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    ifr6.ifr_addr = sin6;

    // Try to get address lifetime
    // Note: OpenBSD may not support SIOCGIFALIFETIME_IN6
    // Fall back to infinite lifetime if not available
    pltime_out = ND6_INFINITE_LIFETIME;
    vltime_out = ND6_INFINITE_LIFETIME;
#endif

    close(s);
    return 0;
}

} // anonymous namespace

std::vector<IPv6Info> get_from_interface(const std::string& iface_name,
                                          std::string&       error_out) {
    // Check if interface exists
    if (if_nametoindex(iface_name.c_str()) == 0) {
        error_out = "Interface not found: " + iface_name;
        return {};
    }

    // Use getifaddrs to enumerate addresses
    struct ifaddrs *ifap = nullptr, *ifa;
    if (getifaddrs(&ifap) == -1) {
        error_out = std::string("getifaddrs() failed: ") + strerror(errno);
        return {};
    }

    std::vector<IPv6Info> result;

    for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        // Skip if no address
        if (ifa->ifa_addr == nullptr) continue;

        // Skip if not matching interface name
        if (strcmp(ifa->ifa_name, iface_name.c_str()) != 0) continue;

        // Skip if not IPv6
        if (ifa->ifa_addr->sa_family != AF_INET6) continue;

        auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);

        // Skip loopback and link-local
        if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
        if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;

        // Convert address to string
        char addr_str[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str)) == nullptr) {
            continue;
        }

        // Get lifetime information
        uint32_t pltime = ND6_INFINITE_LIFETIME;
        uint32_t vltime = ND6_INFINITE_LIFETIME;
        get_ipv6_lifetime(iface_name, *sin6, pltime, vltime);

        // Create IPv6Info
        IPv6Info info;
        info.ip = addr_str;
        info.preferred_lft = (pltime == ND6_INFINITE_LIFETIME)
                                 ? (long)1e12
                                 : (long)pltime;
        info.valid_lft = (vltime == ND6_INFINITE_LIFETIME)
                             ? (long)1e12
                             : (long)vltime;
        info.is_deprecated = (pltime == 0 && vltime > 0);
        populate_info(&info);

        if (info.is_candidate) {
            result.push_back(info);
        }
    }

    freeifaddrs(ifap);

    if (result.empty()) {
        error_out = "No suitable IPv6 address on interface " + iface_name;
    }

    return result;
}

} // namespace ip_getter

#endif // __FreeBSD__ || __OpenBSD__
