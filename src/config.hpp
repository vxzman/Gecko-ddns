#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace config {

inline constexpr std::string_view CACHE_FILENAME = "cache.lastip";
inline constexpr std::string_view ZONEID_CACHE_FILENAME = "cache.zoneid.json";
inline constexpr int DEFAULT_CLOUDFLARE_TTL = 180;
inline constexpr int DEFAULT_ALIYUN_TTL = 600;
inline constexpr int MAX_API_RETRIES = 2;
inline constexpr int API_TIMEOUT_SECONDS = 15;
inline constexpr int HTTP_TIMEOUT_SECONDS = 15;
inline constexpr int HTTP_MAX_RETRIES = 3;
inline constexpr int DEFAULT_TIMEOUT_SECONDS = 300;
inline constexpr uint32_t ND6_INFINITE_LIFETIME = 0xFFFFFFFFU;
inline constexpr long INFINITE_LIFETIME_SECONDS = 1000000000000L;

struct IPSource {
    std::string interface_name;
    std::vector<std::string> fallback_urls;
};

struct RecordConfig {
    std::string provider{};
    std::string zone{};
    std::string name{};          ///< RR label; "@" is the zone apex
    std::string type = "AAAA";
    int ttl = 0;
    bool proxied = false;
    bool use_proxy = false;
    std::string api_token{};
    std::string zone_id{};
    std::string access_key_id{};
    std::string access_key_secret{};
};

struct Config {
    IPSource ip_source;
    std::string proxy;
    std::string netns;         ///< empty: discover on the host. Linux name or /run/netns path.
    std::vector<RecordConfig> records;
};

/// Display name. Apex records are the zone itself, not "@.zone".
[[nodiscard]] inline std::string record_fqdn(const RecordConfig& record) {
    if (record.name == "@") return record.zone;
    return std::format("{}.{}", record.name, record.zone);
}

[[nodiscard]] std::optional<Config> read_config(const std::filesystem::path& path);

/// Replace netns when the CLI passed a non-empty value, then validate it.
[[nodiscard]] bool apply_netns_override(Config& cfg, std::string_view netns_name);

[[nodiscard]] std::filesystem::path get_cache_file_path(const std::filesystem::path& config_abs_path,
                                                        const std::filesystem::path& work_dir);

[[nodiscard]] std::string read_last_ip(const std::filesystem::path& path);
[[nodiscard]] bool write_last_ip(const std::filesystem::path& path, std::string_view ip);

[[nodiscard]] std::string get_record_proxy(const Config& cfg, const RecordConfig& record);
[[nodiscard]] int get_record_ttl(const RecordConfig& record);

[[nodiscard]] std::map<std::string, std::string> read_zone_id_cache(const std::filesystem::path& path);
[[nodiscard]] bool update_zone_id_cache(const std::filesystem::path& path,
                                        std::string_view zone,
                                        std::string_view zone_id);
[[nodiscard]] std::filesystem::path get_zone_id_cache_path(const std::filesystem::path& cache_file_path);

} // namespace config
