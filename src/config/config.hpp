#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace alasia::config {

// ─── Configuration Structures ────────────────────────────────────────────────

/// IP acquisition source configuration
struct IPSource {
    std::string              interface_name; ///< Network interface name (optional)
    std::vector<std::string> urls;           ///< Fallback HTTP API URLs
};

/// General configuration
struct GeneralConfig {
    IPSource    get_ip;
    std::string proxy;       ///< Global proxy URL (optional)
};

/// Cloudflare-specific record configuration
struct CloudflareRecord {
    std::string api_token;
    std::string zone_id;     ///< Auto-fetched if empty
    bool        proxied = false;
    int         ttl     = 0; ///< 0 = use parent record's ttl
};

/// Aliyun-specific record configuration
struct AliyunRecord {
    std::string access_key_id;
    std::string access_key_secret;
    int         ttl = 0;
};

/// DNS record configuration
struct RecordConfig {
    std::string                      provider;
    std::string                      zone;
    std::string                      record;
    int                              ttl       = 0;
    bool                             proxied   = false;
    bool                             use_proxy = false;
    std::optional<CloudflareRecord>  cloudflare;
    std::optional<AliyunRecord>      aliyun;
};

/// Complete application configuration
struct Config {
    std::map<std::string, std::string> environment;  ///< Environment variables
    GeneralConfig                      general;
    std::vector<RecordConfig>          records;
};

// ─── Configuration Loading ───────────────────────────────────────────────────

/// Load and validate configuration from JSON file
/// @return Config on success, error message on failure
Result<Config> load_config(const std::string& path);

/// Save configuration to JSON file
bool save_config(const std::string& path, const Config& cfg);

// ─── Record Helpers ──────────────────────────────────────────────────────────

/// Get effective proxy URL for a record (empty = none)
std::string get_record_proxy(const Config& cfg, const RecordConfig& record);

/// Get effective TTL for a record
int get_record_ttl(const RecordConfig& record);

} // namespace alasia::config
