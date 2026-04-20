#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace config {

// C++23: constexpr constants for configuration defaults
inline constexpr std::string_view DEFAULT_LOG_OUTPUT = "shell";
inline constexpr std::string_view CACHE_FILENAME = "cache.lastip";
inline constexpr std::string_view ZONEID_CACHE_FILENAME = "cache.zoneid.json";
inline constexpr int DEFAULT_CLOUDFLARE_TTL = 180;
inline constexpr int DEFAULT_ALIYUN_TTL = 600;
inline constexpr int MAX_API_RETRIES = 2;
inline constexpr int API_TIMEOUT_SECONDS = 15;

struct IPSource {
    std::string              interface_name; ///< network interface (optional)
    std::vector<std::string> urls;           ///< fallback HTTP API URLs
};

struct GeneralConfig {
    IPSource    get_ip;
    std::string log_output;  ///< "shell" or file path (relative to base_dir if not absolute)
    std::string proxy;       ///< global proxy (optional)
};

struct CloudflareRecord {
    std::string api_token;
    std::string zone_id;  ///< auto-fetched if empty
    bool        proxied = false;
    int         ttl     = 0; ///< 0 = use parent record's ttl
};

struct AliyunRecord {
    std::string access_key_id;
    std::string access_key_secret;
    int         ttl = 0;
};

struct RecordConfig {
    std::string                      provider;
    std::string                      zone;
    std::string                      record;
    int                              ttl       = 0;
    bool                             proxied   = false;
    bool                             use_proxy = false;
    std::optional<CloudflareRecord>  cloudflare;
    std::optional<AliyunRecord>      aliyun;

    // Raw values before environment variable expansion (for security validation)
    std::string _raw_cloudflare_api_token;
    std::string _raw_cloudflare_zone_id;
    std::string _raw_aliyun_access_key_id;
    std::string _raw_aliyun_access_key_secret;
};

struct Config {
    std::map<std::string, std::string> environment;  ///< Environment variables
    GeneralConfig                      general;
    std::vector<RecordConfig>          records;
};

/// Read and validate config JSON. Returns nullopt on any error.
std::optional<Config> read_config(const std::string& path);

/// Write config to file (used to persist auto-fetched zone_id, etc.)
bool write_config(const std::string& path, const Config& cfg);

/// Returns the cache file path next to the config file (or in work_dir)
std::string get_cache_file_path(const std::string& config_abs_path,
                                 const std::string& work_dir);

/// Read last IP from cache file (empty string on miss)
std::string read_last_ip(const std::string& path);

/// Write IP to cache file
bool write_last_ip(const std::string& path, const std::string& ip);

/// Effective proxy URL for a record (empty = none)
std::string get_record_proxy(const Config& cfg, const RecordConfig& record);

/// Effective TTL for a record
int get_record_ttl(const RecordConfig& record);

/// Read ZoneID cache (map of zone -> zone_id)
std::map<std::string, std::string> read_zone_id_cache(const std::string& path);

/// Update ZoneID cache for a specific zone
bool update_zone_id_cache(const std::string& path, const std::string& zone, const std::string& zone_id);

/// Get ZoneID cache file path
std::string get_zone_id_cache_path(const std::string& config_abs_path);

} // namespace config
