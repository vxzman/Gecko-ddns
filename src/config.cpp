#include "config.hpp"
#include "cache.hpp"
#include "log.hpp"

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace config {

namespace {

static std::string trim_copy(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Two labels become the zone apex ("@" + the name). Three or more labels:
/// the leftmost label is the RR and the rest is the zone. A public suffix
/// with more than one label, such as example.com.cn, cannot be told apart
/// from a host under example.com.cn.
static std::optional<std::pair<std::string, std::string>>
split_fqdn(const std::string& raw, std::string& err) {
    std::string s = lower_ascii(trim_copy(raw));
    if (s.empty()) {
        err = "name is empty";
        return std::nullopt;
    }
    if (s.find_first_of(" \t") != std::string::npos) {
        err = "name must not contain whitespace";
        return std::nullopt;
    }
    while (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty()) {
        err = "name is empty";
        return std::nullopt;
    }

    std::vector<std::string> labels;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            if (cur.empty()) {
                err = "name has an empty label";
                return std::nullopt;
            }
            labels.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (cur.empty()) {
        err = "name has an empty label";
        return std::nullopt;
    }
    labels.push_back(std::move(cur));

    if (labels.size() < 2) {
        err = "name must contain at least two labels, for example example.com";
        return std::nullopt;
    }
    if (labels.size() == 2) {
        return std::make_pair(std::string("@"), s);
    }
    return std::make_pair(labels[0], s.substr(labels[0].size() + 1));
}

static bool allowed_keys(const json& j,
                         std::initializer_list<std::string_view> allowed,
                         const std::string& where) {
    if (!j.is_object()) {
        logger::error("Config: {} must be an object", where);
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string key = it.key();
        bool ok = false;
        for (const auto allowed_key : allowed) {
            if (key == allowed_key) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            logger::error("Config: {}: unknown field '{}'", where, key);
            return false;
        }
    }
    return true;
}

static bool validate_proxy_url(const std::string& proxy) {
    if (proxy.empty()) return true;
    const std::string lower = lower_ascii(proxy);
    return lower.rfind("http://", 0) == 0 ||
           lower.rfind("https://", 0) == 0 ||
           lower.rfind("socks5://", 0) == 0 ||
           lower.rfind("socks5h://", 0) == 0;
}

static bool read_ttl(const json& rj, int& ttl, const std::string& where) {
    ttl = 0;
    if (!rj.contains("ttl")) return true;
    if (!rj["ttl"].is_number_integer()) {
        logger::error("Config: {}: ttl must be an integer", where);
        return false;
    }
    const auto value = rj["ttl"].get<std::int64_t>();
    if (value < 0 || value > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        logger::error("Config: {}: ttl is out of range", where);
        return false;
    }
    ttl = static_cast<int>(value);
    return true;
}

static bool read_bool(const json& rj, const char* key, bool& out, const std::string& where) {
    out = false;
    if (!rj.contains(key)) return true;
    if (!rj[key].is_boolean()) {
        logger::error("Config: {}: {} must be a boolean", where, key);
        return false;
    }
    out = rj[key].get<bool>();
    return true;
}

static bool fill_name(RecordConfig& record, const json& rj, const std::string& where) {
    if (!rj.contains("name") || !rj["name"].is_string()) {
        logger::error("Config: {}: name is required", where);
        return false;
    }
    std::string err;
    auto parts = split_fqdn(rj["name"].get<std::string>(), err);
    if (!parts) {
        logger::error("Config: {}: {}", where, err);
        return false;
    }
    record.name = parts->first;
    record.zone = parts->second;
    record.type = "AAAA";
    return true;
}

static bool read_zone_id(RecordConfig& record, const json& rj, const std::string& where) {
    if (!rj.contains("zone_id")) return true;
    if (!rj["zone_id"].is_string()) {
        logger::error("Config: {}: zone_id must be a string", where);
        return false;
    }
    record.zone_id = trim_copy(rj["zone_id"].get<std::string>());
    return true;
}

static bool validate_record(const RecordConfig& r, size_t index, const std::string& global_proxy) {
    if (r.provider.empty()) {
        logger::error("Config: record[{}]: provider is required", index);
        return false;
    }
    if (r.zone.empty()) {
        logger::error("Config: record[{}]: zone is required", index);
        return false;
    }
    if (r.name.empty()) {
        logger::error("Config: record[{}]: record name is required", index);
        return false;
    }
    if (r.use_proxy && global_proxy.empty()) {
        logger::error("Config: record[{}]: use_proxy is true but no global proxy configured", index);
        return false;
    }

    if (r.provider == "cloudflare") {
        if (r.api_token.empty()) {
            logger::error("Config: record[{}]: cloudflare token is not set", index);
            return false;
        }
    } else if (r.provider == "aliyun") {
        if (r.access_key_id.empty()) {
            logger::error("Config: record[{}]: aliyun access_key_id is not set", index);
            return false;
        }
        if (r.access_key_secret.empty()) {
            logger::error("Config: record[{}]: aliyun access_key_secret is not set", index);
            return false;
        }
    } else {
        logger::error("Config: record[{}]: unsupported provider '{}'", index, r.provider);
        return false;
    }
    return true;
}

static bool validate_config(const Config& cfg) {
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }
    if (cfg.ip_source.interface_name.empty() && cfg.ip_source.fallback_urls.empty()) {
        logger::error("Config: either 'interface' or 'fallback_urls' must be configured");
        return false;
    }
    if (!cfg.proxy.empty() && !validate_proxy_url(cfg.proxy)) {
        logger::error("Config: invalid proxy '{}'", cfg.proxy);
        return false;
    }
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        if (!validate_record(cfg.records[i], i, cfg.proxy)) return false;
    }
    return true;
}

static bool append_cloudflare(Config& cfg, const json& cf) {
    if (!allowed_keys(cf, {"token", "records"}, "cloudflare")) return false;
    if (!cf.contains("token") || !cf["token"].is_string() ||
        trim_copy(cf["token"].get<std::string>()).empty()) {
        logger::error("Config: cloudflare.token is required");
        return false;
    }
    if (!cf.contains("records") || !cf["records"].is_array()) {
        logger::error("Config: cloudflare.records must be an array");
        return false;
    }
    if (cf["records"].empty()) {
        logger::error("Config: cloudflare.records must contain at least one record");
        return false;
    }

    const std::string token = trim_copy(cf["token"].get<std::string>());
    for (size_t i = 0; i < cf["records"].size(); ++i) {
        const auto& rj = cf["records"][i];
        const std::string where = "cloudflare.records[" + std::to_string(i) + "]";
        if (!allowed_keys(rj, {"name", "zone_id", "ttl", "proxied", "use_proxy"}, where)) return false;

        RecordConfig record;
        record.provider = "cloudflare";
        record.api_token = token;
        if (!fill_name(record, rj, where)) return false;
        if (!read_zone_id(record, rj, where)) return false;
        if (!read_ttl(rj, record.ttl, where)) return false;
        if (!read_bool(rj, "proxied", record.proxied, where)) return false;
        if (!read_bool(rj, "use_proxy", record.use_proxy, where)) return false;
        cfg.records.push_back(std::move(record));
    }
    return true;
}

static bool append_aliyun(Config& cfg, const json& ali) {
    if (!allowed_keys(ali, {"access_key_id", "access_key_secret", "records"}, "aliyun")) return false;
    if (!ali.contains("access_key_id") || !ali["access_key_id"].is_string() ||
        trim_copy(ali["access_key_id"].get<std::string>()).empty()) {
        logger::error("Config: aliyun.access_key_id is required");
        return false;
    }
    if (!ali.contains("access_key_secret") || !ali["access_key_secret"].is_string() ||
        trim_copy(ali["access_key_secret"].get<std::string>()).empty()) {
        logger::error("Config: aliyun.access_key_secret is required");
        return false;
    }
    if (!ali.contains("records") || !ali["records"].is_array()) {
        logger::error("Config: aliyun.records must be an array");
        return false;
    }
    if (ali["records"].empty()) {
        logger::error("Config: aliyun.records must contain at least one record");
        return false;
    }

    const std::string key_id = trim_copy(ali["access_key_id"].get<std::string>());
    const std::string key_secret = trim_copy(ali["access_key_secret"].get<std::string>());
    for (size_t i = 0; i < ali["records"].size(); ++i) {
        const auto& rj = ali["records"][i];
        const std::string where = "aliyun.records[" + std::to_string(i) + "]";
        if (!allowed_keys(rj, {"name", "ttl", "use_proxy"}, where)) return false;

        RecordConfig record;
        record.provider = "aliyun";
        record.access_key_id = key_id;
        record.access_key_secret = key_secret;
        if (!fill_name(record, rj, where)) return false;
        if (!read_ttl(rj, record.ttl, where)) return false;
        if (!read_bool(rj, "use_proxy", record.use_proxy, where)) return false;
        cfg.records.push_back(std::move(record));
    }
    return true;
}

} // namespace

std::optional<Config> read_config(const std::string& path) {
    std::error_code ec;
    fs::path abs_path = fs::absolute(path, ec);
    if (ec) {
        logger::error("Failed to resolve config path '{}': {}", path, ec.message());
        return std::nullopt;
    }

    std::ifstream f(abs_path);
    if (!f.is_open()) {
        logger::error("Failed to open config file: {}", abs_path.string());
        return std::nullopt;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        logger::error("配置文件 JSON 格式错误：{}", e.what());
        return std::nullopt;
    }

    if (!root.is_object()) {
        logger::error("Config: root must be a JSON object");
        return std::nullopt;
    }
    if (!allowed_keys(root, {"interface", "fallback_urls", "proxy", "cloudflare", "aliyun"}, "config")) {
        return std::nullopt;
    }

    Config cfg;

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
    if (root.contains("cloudflare") && !append_cloudflare(cfg, root["cloudflare"])) {
        return std::nullopt;
    }
    if (root.contains("aliyun") && !append_aliyun(cfg, root["aliyun"])) {
        return std::nullopt;
    }
    if (!validate_config(cfg)) return std::nullopt;

    return cfg;
}

std::string get_cache_file_path(const std::string& config_abs_path,
                                 const std::string& base_dir) {
    if (!base_dir.empty()) {
        std::error_code ec;
        fs::create_directories(base_dir, ec);
        if (!ec) {
            return (fs::path(base_dir) / std::string(CACHE_FILENAME)).string();
        }
        logger::error("Failed to create base_dir '{}': {}", base_dir, ec.message());
    }
    return (fs::path(config_abs_path).parent_path() / std::string(CACHE_FILENAME)).string();
}

std::string read_last_ip(const std::string& path) {
    return ::cache::parse_cache_file(path).last_ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
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

std::string get_zone_id_cache_path(const std::string& cache_file_path) {
    return (fs::path(cache_file_path).parent_path() / std::string(ZONEID_CACHE_FILENAME)).string();
}

std::map<std::string, std::string> read_zone_id_cache(const std::string& path) {
    std::ifstream f(path);
    std::map<std::string, std::string> zone_ids;
    if (!f.is_open()) return zone_ids;

    try {
        json j;
        f >> j;
        if (j.is_object()) {
            for (const auto& [zone, id] : j.items()) {
                if (id.is_string()) {
                    zone_ids[zone] = id.get<std::string>();
                }
            }
        }
    } catch (const json::parse_error&) {
        return zone_ids;
    }
    return zone_ids;
}

bool update_zone_id_cache(const std::string& path, const std::string& zone, const std::string& zone_id) {
    std::map<std::string, std::string> zone_ids;

    std::ifstream f_in(path);
    if (f_in.is_open()) {
        try {
            json j;
            f_in >> j;
            if (j.is_object()) {
                for (const auto& [z, id] : j.items()) {
                    if (id.is_string()) {
                        zone_ids[z] = id.get<std::string>();
                    }
                }
            }
        } catch (const json::parse_error&) {
        }
    }

    zone_ids[zone] = zone_id;

    json j = json::object();
    for (const auto& [z, id] : zone_ids) {
        j[z] = id;
    }

    std::ofstream f_out(path);
    if (!f_out.is_open()) return false;
    f_out << j.dump(4) << "\n";
    if (!f_out.good()) return false;
    f_out.close();

    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

} // namespace config
