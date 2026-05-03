#pragma once

#include <string>
#include <cstdint>

namespace alasia {

// Version information
#ifndef APP_VERSION
#  define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#  define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#  define APP_BUILD_DATE ""
#endif

// Configuration defaults
inline constexpr std::string_view CACHE_FILENAME = "cache.lastip";
inline constexpr std::string_view ZONEID_CACHE_FILENAME = "cache.zoneid.json";
inline constexpr int DEFAULT_CLOUDFLARE_TTL = 180;
inline constexpr int DEFAULT_ALIYUN_TTL = 600;
inline constexpr int MAX_API_RETRIES = 2;
inline constexpr int API_TIMEOUT_SECONDS = 15;

// HTTP client defaults
inline constexpr int HTTP_TIMEOUT_SECONDS = 15;
inline constexpr int HTTP_MAX_RETRIES = 3;
inline constexpr int DEFAULT_TIMEOUT_SECONDS = 300;

// IPv6 constants
inline constexpr uint32_t ND6_INFINITE_LIFETIME = 0xFFFFFFFFU;
inline constexpr long INFINITE_LIFETIME_SECONDS = 1000000000000L;

/// IPv6 address information
struct IPv6Info {
    std::string ip;
    std::string scope;           ///< "Link Local", "Unique Local (ULA)", "Global Unicast"
    std::string address_state;   ///< "Expired", "Deprecated", "Preferred/Dynamic", "Preferred/Static"
    long        preferred_lft = 0;
    long        valid_lft     = 0;
    bool        is_deprecated   = false;
    bool        is_unique_local = false;
    bool        is_candidate    = false;

    bool operator==(const IPv6Info&) const = default;
};

/// DNS record update result
struct UpdateResult {
    std::string record_name;
    bool        success = false;
    std::string error;
};

} // namespace alasia
