#include "config.hpp"

#include "cache.hpp"
#include "log.hpp"
#include "netns_probe.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace config {

namespace {

constexpr std::array<std::string_view, 6> kRootKeys{
    "interface", "fallback_urls", "proxy", "cloudflare", "aliyun", "netns",
};
constexpr std::array<std::string_view, 2> kCloudflareKeys{"token", "records"};
constexpr std::array<std::string_view, 5> kCloudflareRecordKeys{
    "name", "zone_id", "ttl", "proxied", "use_proxy",
};
constexpr std::array<std::string_view, 3> kAliyunKeys{
    "access_key_id", "access_key_secret", "records",
};
constexpr std::array<std::string_view, 3> kAliyunRecordKeys{"name", "ttl", "use_proxy"};

std::string trim_copy(std::string_view text) {
    return std::string{text::trim(text)};
}

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// Two labels become the zone apex ("@" + the name). Three or more labels:
/// the leftmost label is the RR and the rest is the zone. A public suffix
/// with more than one label, such as example.com.cn, cannot be told apart
/// from a host under example.com.cn.
std::optional<std::pair<std::string, std::string>> split_fqdn(std::string_view raw, std::string& err) {
    std::string name = lower_ascii(trim_copy(raw));
    if (name.empty()) {
        err = "name is empty";
        return std::nullopt;
    }
    if (name.find_first_of(" \t") != std::string::npos) {
        err = "name must not contain whitespace";
        return std::nullopt;
    }
    while (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty()) {
        err = "name is empty";
        return std::nullopt;
    }

    std::vector<std::string> labels;
    for (auto&& part : name | std::views::split('.')) {
        labels.emplace_back(part.begin(), part.end());
    }
    for (const auto& label : labels) {
        if (label.empty()) {
            err = "name has an empty label";
            return std::nullopt;
        }
    }
    if (labels.size() < 2) {
        err = "name must contain at least two labels, for example example.com";
        return std::nullopt;
    }
    if (labels.size() == 2) return std::make_pair(std::string{"@"}, name);
    return std::make_pair(labels[0], name.substr(labels[0].size() + 1));
}

template <std::size_t N>
[[nodiscard]] bool allowed_keys(const json& object,
                                const std::array<std::string_view, N>& allowed,
                                std::string_view where) {
    if (!object.is_object()) {
        logger::error("Config: {} must be an object", where);
        return false;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string key = it.key();
        if (!std::ranges::contains(allowed, std::string_view{key})) {
            logger::error("Config: {}: unknown field '{}'", where, key);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validate_proxy_url(std::string_view proxy) {
    if (proxy.empty()) return true;
    const std::string lower = lower_ascii(std::string{proxy});
    return lower.starts_with("http://") || lower.starts_with("https://") ||
           lower.starts_with("socks5://") || lower.starts_with("socks5h://");
}

[[nodiscard]] bool read_ttl(const json& record, int& ttl, std::string_view where) {
    ttl = 0;
    if (!record.contains("ttl")) return true;
    if (!record["ttl"].is_number_integer()) {
        logger::error("Config: {}: ttl must be an integer", where);
        return false;
    }
    const auto value = record["ttl"].get<std::int64_t>();
    if (value < 0 || value > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        logger::error("Config: {}: ttl is out of range", where);
        return false;
    }
    ttl = static_cast<int>(value);
    return true;
}

[[nodiscard]] bool read_bool(const json& record, const char* key, bool& out, std::string_view where) {
    out = false;
    if (!record.contains(key)) return true;
    if (!record[key].is_boolean()) {
        logger::error("Config: {}: {} must be a boolean", where, key);
        return false;
    }
    out = record[key].get<bool>();
    return true;
}

[[nodiscard]] bool fill_name(RecordConfig& record, const json& body, std::string_view where) {
    if (!body.contains("name") || !body["name"].is_string()) {
        logger::error("Config: {}: name is required", where);
        return false;
    }
    std::string err;
    const auto parts = split_fqdn(body["name"].get<std::string>(), err);
    if (!parts) {
        logger::error("Config: {}: {}", where, err);
        return false;
    }
    record.name = parts->first;
    record.zone = parts->second;
    record.type = "AAAA";
    return true;
}

[[nodiscard]] bool read_zone_id(RecordConfig& record, const json& body, std::string_view where) {
    if (!body.contains("zone_id")) return true;
    if (!body["zone_id"].is_string()) {
        logger::error("Config: {}: zone_id must be a string", where);
        return false;
    }
    record.zone_id = trim_copy(body["zone_id"].get<std::string>());
    return true;
}

[[nodiscard]] bool validate_record(const RecordConfig& record, std::size_t index, std::string_view global_proxy) {
    if (record.provider.empty()) {
        logger::error("Config: record[{}]: provider is required", index);
        return false;
    }
    if (record.zone.empty()) {
        logger::error("Config: record[{}]: zone is required", index);
        return false;
    }
    if (record.name.empty()) {
        logger::error("Config: record[{}]: record name is required", index);
        return false;
    }
    if (record.use_proxy && global_proxy.empty()) {
        logger::error("Config: record[{}]: use_proxy is true but no global proxy configured", index);
        return false;
    }
    if (record.provider == "cloudflare") {
        if (record.api_token.empty()) {
            logger::error("Config: record[{}]: cloudflare token is not set", index);
            return false;
        }
    } else if (record.provider == "aliyun") {
        if (record.access_key_id.empty()) {
            logger::error("Config: record[{}]: aliyun access_key_id is not set", index);
            return false;
        }
        if (record.access_key_secret.empty()) {
            logger::error("Config: record[{}]: aliyun access_key_secret is not set", index);
            return false;
        }
    } else {
        logger::error("Config: record[{}]: unsupported provider '{}'", index, record.provider);
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_netns(const Config& cfg) {
    if (cfg.netns.empty()) return true;
#if !defined(__linux__)
    logger::error("Config: netns is only supported on Linux");
    return false;
#else
    if (cfg.ip_source.interface_name.empty()) {
        logger::error("Config: interface is required when netns is set");
        return false;
    }
    const auto resolved = netns::resolve_netns_path(cfg.netns);
    if (!resolved) {
        logger::error("Config: {}", resolved.error());
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool validate_config(const Config& cfg) {
    if (!validate_netns(cfg)) return false;
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }
    if (cfg.netns.empty() && cfg.ip_source.interface_name.empty() && cfg.ip_source.fallback_urls.empty()) {
        logger::error("Config: either 'interface' or 'fallback_urls' must be configured");
        return false;
    }
    if (!cfg.proxy.empty() && !validate_proxy_url(cfg.proxy)) {
        logger::error("Config: invalid proxy '{}'", cfg.proxy);
        return false;
    }
    for (std::size_t i = 0; i < cfg.records.size(); ++i) {
        if (!validate_record(cfg.records[i], i, cfg.proxy)) return false;
    }
    return true;
}

[[nodiscard]] bool append_cloudflare(Config& cfg, const json& cloudflare) {
    if (!allowed_keys(cloudflare, kCloudflareKeys, "cloudflare")) return false;
    if (!cloudflare.contains("token") || !cloudflare["token"].is_string() ||
        trim_copy(cloudflare["token"].get<std::string>()).empty()) {
        logger::error("Config: cloudflare.token is required");
        return false;
    }
    if (!cloudflare.contains("records") || !cloudflare["records"].is_array()) {
        logger::error("Config: cloudflare.records must be an array");
        return false;
    }
    if (cloudflare["records"].empty()) {
        logger::error("Config: cloudflare.records must contain at least one record");
        return false;
    }

    const std::string token = trim_copy(cloudflare["token"].get<std::string>());
    for (std::size_t i = 0; i < cloudflare["records"].size(); ++i) {
        const auto& body = cloudflare["records"][i];
        const std::string where = std::format("cloudflare.records[{}]", i);
        if (!allowed_keys(body, kCloudflareRecordKeys, where)) return false;

        RecordConfig record{
            .provider = "cloudflare",
            .api_token = token,
        };
        if (!fill_name(record, body, where)) return false;
        if (!read_zone_id(record, body, where)) return false;
        if (!read_ttl(body, record.ttl, where)) return false;
        if (!read_bool(body, "proxied", record.proxied, where)) return false;
        if (!read_bool(body, "use_proxy", record.use_proxy, where)) return false;
        cfg.records.push_back(std::move(record));
    }
    return true;
}

[[nodiscard]] bool append_aliyun(Config& cfg, const json& aliyun) {
    if (!allowed_keys(aliyun, kAliyunKeys, "aliyun")) return false;
    if (!aliyun.contains("access_key_id") || !aliyun["access_key_id"].is_string() ||
        trim_copy(aliyun["access_key_id"].get<std::string>()).empty()) {
        logger::error("Config: aliyun.access_key_id is required");
        return false;
    }
    if (!aliyun.contains("access_key_secret") || !aliyun["access_key_secret"].is_string() ||
        trim_copy(aliyun["access_key_secret"].get<std::string>()).empty()) {
        logger::error("Config: aliyun.access_key_secret is required");
        return false;
    }
    if (!aliyun.contains("records") || !aliyun["records"].is_array()) {
        logger::error("Config: aliyun.records must be an array");
        return false;
    }
    if (aliyun["records"].empty()) {
        logger::error("Config: aliyun.records must contain at least one record");
        return false;
    }

    const std::string key_id = trim_copy(aliyun["access_key_id"].get<std::string>());
    const std::string key_secret = trim_copy(aliyun["access_key_secret"].get<std::string>());
    for (std::size_t i = 0; i < aliyun["records"].size(); ++i) {
        const auto& body = aliyun["records"][i];
        const std::string where = std::format("aliyun.records[{}]", i);
        if (!allowed_keys(body, kAliyunRecordKeys, where)) return false;

        RecordConfig record{
            .provider = "aliyun",
            .access_key_id = key_id,
            .access_key_secret = key_secret,
        };
        if (!fill_name(record, body, where)) return false;
        if (!read_ttl(body, record.ttl, where)) return false;
        if (!read_bool(body, "use_proxy", record.use_proxy, where)) return false;
        cfg.records.push_back(std::move(record));
    }
    return true;
}

} // namespace

std::optional<Config> read_config(const fs::path& path) {
    std::error_code ec;
    const fs::path abs_path = fs::absolute(path, ec);
    if (ec) {
        logger::error("Failed to resolve config path '{}': {}", path.string(), ec.message());
        return std::nullopt;
    }

    std::ifstream file{abs_path};
    if (!file.is_open()) {
        logger::error("Failed to open config file: {}", abs_path.string());
        return std::nullopt;
    }

    json root;
    try {
        file >> root;
    } catch (const json::parse_error& ex) {
        logger::error("配置文件 JSON 格式错误：{}", ex.what());
        return std::nullopt;
    }
    if (!root.is_object()) {
        logger::error("Config: root must be a JSON object");
        return std::nullopt;
    }
    if (!allowed_keys(root, kRootKeys, "config")) return std::nullopt;

    Config cfg{
        .ip_source = {},
        .proxy = {},
        .netns = {},
        .records = {},
    };
    if (root.contains("interface")) {
        if (!root["interface"].is_string()) {
            logger::error("Config: interface must be a string");
            return std::nullopt;
        }
        cfg.ip_source.interface_name = trim_copy(root["interface"].get<std::string>());
    }
    if (root.contains("fallback_urls")) {
        if (!root["fallback_urls"].is_array()) {
            logger::error("Config: fallback_urls must be an array");
            return std::nullopt;
        }
        for (const auto& url : root["fallback_urls"]) {
            if (!url.is_string() || trim_copy(url.get<std::string>()).empty()) {
                logger::error("Config: fallback_urls entries must be non-empty strings");
                return std::nullopt;
            }
            cfg.ip_source.fallback_urls.push_back(trim_copy(url.get<std::string>()));
        }
    }
    if (root.contains("proxy")) {
        if (!root["proxy"].is_string()) {
            logger::error("Config: proxy must be a string");
            return std::nullopt;
        }
        cfg.proxy = trim_copy(root["proxy"].get<std::string>());
    }
    if (root.contains("netns")) {
        if (!root["netns"].is_string()) {
            logger::error("Config: netns must be a string");
            return std::nullopt;
        }
        cfg.netns = trim_copy(root["netns"].get<std::string>());
    }
    if (root.contains("cloudflare") && !append_cloudflare(cfg, root["cloudflare"])) return std::nullopt;
    if (root.contains("aliyun") && !append_aliyun(cfg, root["aliyun"])) return std::nullopt;
    if (!validate_config(cfg)) return std::nullopt;
    return cfg;
}

bool apply_netns_override(Config& cfg, std::string_view netns_name) {
    const std::string trimmed = trim_copy(netns_name);
    if (trimmed.empty()) return true;
    cfg.netns = trimmed;
    return validate_netns(cfg);
}

fs::path get_cache_file_path(const fs::path& config_abs_path, const fs::path& base_dir) {
    if (!base_dir.empty()) {
        std::error_code ec;
        fs::create_directories(base_dir, ec);
        if (!ec) return base_dir / CACHE_FILENAME;
        logger::error("Failed to create base_dir '{}': {}", base_dir.string(), ec.message());
    }
    return config_abs_path.parent_path() / CACHE_FILENAME;
}

std::string read_last_ip(const fs::path& path) {
    return ::cache::parse_cache_file(path).last_ip;
}

bool write_last_ip(const fs::path& path, std::string_view ip) {
    return ::cache::write_last_ip(path, ip);
}

std::string get_record_proxy(const Config& cfg, const RecordConfig& record) {
    if (!record.use_proxy) return "";
    return cfg.proxy;
}

int get_record_ttl(const RecordConfig& record) {
    if (record.ttl > 0) return record.ttl;
    if (record.provider == "cloudflare") return DEFAULT_CLOUDFLARE_TTL;
    return DEFAULT_ALIYUN_TTL;
}

fs::path get_zone_id_cache_path(const fs::path& cache_file_path) {
    return cache_file_path.parent_path() / ZONEID_CACHE_FILENAME;
}

std::map<std::string, std::string> read_zone_id_cache(const fs::path& path) {
    std::ifstream file{path};
    std::map<std::string, std::string> zone_ids;
    if (!file.is_open()) return zone_ids;
    try {
        json parsed;
        file >> parsed;
        if (parsed.is_object()) {
            for (const auto& [zone, id] : parsed.items()) {
                if (id.is_string()) zone_ids[zone] = id.get<std::string>();
            }
        }
    } catch (const json::parse_error&) {
        return zone_ids;
    }
    return zone_ids;
}

bool update_zone_id_cache(const fs::path& path, std::string_view zone, std::string_view zone_id) {
    std::map<std::string, std::string> zone_ids;
    std::ifstream input{path};
    if (input.is_open()) {
        try {
            json parsed;
            input >> parsed;
            if (parsed.is_object()) {
                for (const auto& [name, id] : parsed.items()) {
                    if (id.is_string()) zone_ids[name] = id.get<std::string>();
                }
            }
        } catch (const json::parse_error&) {
        }
    }

    zone_ids[std::string{zone}] = std::string{zone_id};
    json body = json::object();
    for (const auto& [name, id] : zone_ids) body[name] = id;

    std::ofstream output{path};
    if (!output.is_open()) return false;
    output << body.dump(4) << '\n';
    if (!output.good()) return false;
    output.close();
    return chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
}

} // namespace config
