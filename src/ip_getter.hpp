#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef AF_INET6
#define AF_INET6 10
#endif

namespace ip_getter {

/// IPv6Info contains information about an IPv6 address.
struct IPv6Info {
    std::string ip{};
    std::string scope{};             ///< "Link Local", "Unique Local (ULA)", "Global Unicast"
    std::string address_state{};     ///< "Expired", "Deprecated", "Preferred/Dynamic", "Preferred/Static"
    long preferred_lft = 0;        ///< seconds
    long valid_lft = 0;
    bool is_deprecated = false;
    bool is_unique_local = false;
    bool is_candidate = false;     ///< Whether it is a DDNS candidate
};

[[nodiscard]] inline bool is_link_local(std::span<const uint8_t> addr) {
    return addr.size() >= 2 && addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80;
}

[[nodiscard]] inline bool is_loopback(std::span<const uint8_t> addr) {
    if (addr.size() < 16) return false;
    for (std::size_t i = 0; i < 15; ++i) {
        if (addr[i] != 0) return false;
    }
    return addr[15] == 1;
}

[[nodiscard]] inline bool is_ula(std::span<const uint8_t> addr) {
    return !addr.empty() && (addr[0] == 0xfc || addr[0] == 0xfd);
}

inline void populate_info(IPv6Info* info) {
    if (info->ip.empty()) return;

    std::array<uint8_t, 16> addr{};
    if (inet_pton(AF_INET6, info->ip.c_str(), addr.data()) != 1) return;
    const std::span<const uint8_t> bytes{addr};

    info->is_unique_local = is_ula(bytes);
    if (is_link_local(bytes)) {
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

/// Get IPv6 addresses from an interface (Linux/FreeBSD netlink).
[[nodiscard]] std::expected<std::vector<IPv6Info>, std::string> get_from_interface(std::string_view iface_name);

/// Query HTTP echo APIs. Returns the first success. timeout_sec 0 means no overall deadline.
/// Stop and shutdown requests abort the in-flight requests; unfinished work is joined, never detached.
[[nodiscard]] std::expected<std::vector<IPv6Info>, std::string>
get_from_apis(const std::vector<std::string>& urls, int timeout_sec = 0);

/// Longest preferred lifetime among candidates. Filter rules are fixed; do not loosen them.
[[nodiscard]] std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos);

} // namespace ip_getter
