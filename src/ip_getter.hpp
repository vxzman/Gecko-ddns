#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <expected>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// FreeBSD clang++ with C++23 hides AF_INET6 via _STRICT_STDC_
#ifndef AF_INET6
#define AF_INET6 10
#endif

namespace ip_getter {

/// IPv6Info contains information about an IPv6 address
/// (matching Go version's ifaddr.IPv6Info)
struct IPv6Info {
    std::string ip;
    std::string scope;           ///< "Link Local", "Unique Local (ULA)", "Global Unicast"
    std::string address_state;   ///< "Expired", "Deprecated", "Preferred/Dynamic", "Preferred/Static"
    long        preferred_lft = 0; ///< seconds
    long        valid_lft     = 0;
    bool        is_deprecated = false;
    bool        is_unique_local = false;
    bool        is_candidate  = false; ///< Whether it is a DDNS candidate
};

// ─── Inline utilities (shared across ip_getter.cpp and ip_getter_bsd.cpp) ──

inline bool is_link_local(const uint8_t* addr16) {
    return addr16[0] == 0xfe && (addr16[1] & 0xc0) == 0x80;
}

inline bool is_loopback(const uint8_t* addr16) {
    for (int i = 0; i < 15; ++i) if (addr16[i] != 0) return false;
    return addr16[15] == 1;
}

inline bool is_ula(const uint8_t* addr16) {
    return (addr16[0] == 0xfc || addr16[0] == 0xfd);
}

inline void populate_info(IPv6Info* info) {
    if (info->ip.empty()) return;

    uint8_t addr[16];
    if (inet_pton(AF_INET6, info->ip.c_str(), addr) != 1) return;

    info->is_unique_local = is_ula(addr);

    if (is_link_local(addr)) {
        info->scope = "Link Local";
    } else if (info->is_unique_local) {
        info->scope = "Unique Local (ULA)";
    } else {
        info->scope = "Global Unicast";
    }

    info->is_deprecated = (info->preferred_lft <= 0 && info->valid_lft > 0);

    if (info->valid_lft == 0) {
        info->address_state = "Expired";
    } else if (info->is_deprecated) {
        info->address_state = "Deprecated";
    } else if (info->preferred_lft < info->valid_lft) {
        info->address_state = "Preferred/Dynamic";
    } else {
        info->address_state = "Preferred/Static";
    }

    info->is_candidate = (info->scope == "Global Unicast" &&
                          !info->is_deprecated &&
                          !info->is_unique_local &&
                          info->valid_lft > 0);
}

/// Get IPv6 addresses from interface (Linux netlink / FreeBSD ioctl).
/// Returns error string on failure.
std::expected<std::vector<IPv6Info>, std::string> get_from_interface(std::string_view iface_name);

/// Query multiple HTTP APIs (tries each, returns first success).
std::expected<std::vector<IPv6Info>, std::string> get_from_apis(const std::vector<std::string>& urls);

/// Select the best (longest PreferredLft) global unicast candidate.
/// Returns error string on failure.
std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos);

} // namespace ip_getter
