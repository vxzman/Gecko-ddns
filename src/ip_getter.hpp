#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <expected>

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

/// Get IPv6 addresses from interface using Linux netlink.
/// Returns error string on failure.
std::expected<std::vector<IPv6Info>, std::string> get_from_interface(std::string_view iface_name);

/// Query multiple HTTP APIs (tries each, returns first success).
std::expected<std::vector<IPv6Info>, std::string> get_from_apis(const std::vector<std::string>& urls);

/// Select the best (longest PreferredLft) global unicast candidate.
/// Returns error string on failure.
std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos);

} // namespace ip_getter
