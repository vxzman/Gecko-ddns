#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gecko::config {

// ─── Configuration Structures ────────────────────────────────────────────────

/// IP acquisition source configuration
struct IPSource {
    std::string              interface_name; ///< Network interface name (optional)
    std::vector<std::string> fallback_urls;  ///< Fallback HTTP API URLs
};

/// DNS record configuration (flat — validated at runtime by provider)
struct RecordConfig {
    std::string provider;
    std::string zone;
    std::string name;         ///< record name / subdomain
    std::string type;         ///< "AAAA" (default) or "A"
    int         ttl       = 0;
    bool        proxied   = false; ///< Cloudflare CDN proxy only
    bool        use_proxy = false;

    // Provider-specific fields (flat)
    std::string api_token;          ///< Cloudflare
    std::string zone_id;            ///< Cloudflare (auto-fetched if empty)
    std::string access_key_id;      ///< Aliyun
    std::string access_key_secret;  ///< Aliyun
};

/// Complete application configuration
struct Config {
    std::map<std::string, std::string> environment; ///< Environment variables (from "env")
    IPSource                           ip_source;
    std::string                        proxy;       ///< Global proxy for DNS API
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

} // namespace gecko::config
