#include "config/config.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace gecko::config {

namespace {

// ─── JSON Helpers ─────────────────────────────────────────────────────────────

std::string jstr(const json& j, const char* key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return def;
}

bool jbool(const json& j, const char* key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return def;
}

int jint(const json& j, const char* key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) {
        return j[key].get<int>();
    }
    return def;
}

// ─── Environment Variable Resolution ──────────────────────────────────────────

/// Resolve $name reference from cfg.environment
/// Only supports $name syntax (e.g., $cloudflare_token)
std::string resolve_env_var(const std::string& expr, const std::map<std::string, std::string>& environment) {
    if (expr.empty() || expr[0] != '$') {
        return expr;
    }

    std::string name = expr.substr(1);
    if (name.empty()) {
        return expr;
    }

    auto it = environment.find(name);
    if (it != environment.end()) {
        return it->second;
    }
    return "";
}

// ─── Validation ───────────────────────────────────────────────────────────────

bool validate_proxy_url(const std::string& proxy) {
    if (proxy.empty()) return true;
    std::string lower = proxy;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.rfind("http://", 0) == 0 ||
           lower.rfind("https://", 0) == 0 ||
           lower.rfind("socks5://", 0) == 0 ||
           lower.rfind("socks5h://", 0) == 0;
}

bool validate_record(const RecordConfig& r, size_t index, const std::string& global_proxy) {
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
        logger::error("Config: record[{}]: use_proxy=true but no global 'proxy' set", index);
        return false;
    }

    if (r.provider == "cloudflare") {
        if (r.api_token.empty()) {
            logger::error("Config: record[{}]: cloudflare 'api_token' is not set", index);
            return false;
        }
    } else if (r.provider == "aliyun") {
        if (r.access_key_id.empty()) {
            logger::error("Config: record[{}]: aliyun 'access_key_id' is not set", index);
            return false;
        }
        if (r.access_key_secret.empty()) {
            logger::error("Config: record[{}]: aliyun 'access_key_secret' is not set", index);
            return false;
        }
    } else {
        logger::error("Config: record[{}]: unsupported provider '{}'", index, r.provider);
        return false;
    }
    return true;
}

bool validate_all_records(const std::vector<RecordConfig>& records, const std::string& global_proxy) {
    for (size_t i = 0; i < records.size(); ++i) {
        if (!validate_record(records[i], i, global_proxy)) {
            return false;
        }
    }
    return true;
}

bool validate_config(const Config& cfg) {
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }

    if (!cfg.proxy.empty() && !validate_proxy_url(cfg.proxy)) {
        logger::error("Config: invalid global 'proxy' format '{}'", cfg.proxy);
        return false;
    }

    return validate_all_records(cfg.records, cfg.proxy);
}

// ─── Parsing ──────────────────────────────────────────────────────────────────

RecordConfig parse_record_config(const json& rj, const std::map<std::string, std::string>& environment) {
    RecordConfig r;
    r.provider  = jstr(rj, "provider");
    r.zone      = jstr(rj, "zone");
    r.name      = jstr(rj, "name");
    r.type      = jstr(rj, "type", "AAAA");
    r.ttl       = jint(rj, "ttl");
    r.proxied   = jbool(rj, "proxied");
    r.use_proxy = jbool(rj, "use_proxy");

    // Flat provider fields (resolve $name references)
    r.api_token         = resolve_env_var(jstr(rj, "api_token"), environment);
    r.zone_id           = resolve_env_var(jstr(rj, "zone_id"), environment);
    r.access_key_id     = resolve_env_var(jstr(rj, "access_key_id"), environment);
    r.access_key_secret = resolve_env_var(jstr(rj, "access_key_secret"), environment);

    return r;
}

} // anonymous namespace

// ─── Public API ───────────────────────────────────────────────────────────────

Result<Config> load_config(const std::string& path) {
    std::error_code ec;
    fs::path abs_path = fs::absolute(path, ec);
    if (ec) {
        return Result<Config>::error(std::format("Failed to resolve config path '{}': {}", path, ec.message()));
    }

    std::ifstream f(abs_path);
    if (!f.is_open()) {
        return Result<Config>::error(std::format("Failed to open config file: {}", abs_path.string()));
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        return Result<Config>::error(std::format("配置文件 JSON 格式错误：{}", e.what()));
    }

    Config cfg;

    // env
    if (root.contains("env") && root["env"].is_object()) {
        for (const auto& [key, value] : root["env"].items()) {
            if (value.is_string()) {
                cfg.environment[key] = value.get<std::string>();
            }
        }
    }

    // ip_source
    if (root.contains("ip_source")) {
        const auto& is = root["ip_source"];
        cfg.ip_source.interface_name = jstr(is, "interface");
        if (is.contains("fallback_urls") && is["fallback_urls"].is_array()) {
            for (const auto& u : is["fallback_urls"]) {
                if (u.is_string()) {
                    cfg.ip_source.fallback_urls.push_back(u.get<std::string>());
                }
            }
        }
    }

    // proxy (top-level)
    cfg.proxy = jstr(root, "proxy");

    // records
    if (root.contains("records") && root["records"].is_array()) {
        for (const auto& rj : root["records"]) {
            cfg.records.push_back(parse_record_config(rj, cfg.environment));
        }
    }

    if (!validate_config(cfg)) {
        return Result<Config>::error("Configuration validation failed");
    }

    return Result<Config>(std::move(cfg));
}

bool save_config(const std::string& path, const Config& cfg) {
    json root;

    // env
    root["env"] = json::object();
    for (const auto& [key, value] : cfg.environment) {
        root["env"][key] = value;
    }

    // ip_source
    root["ip_source"]["interface"]     = cfg.ip_source.interface_name;
    root["ip_source"]["fallback_urls"] = cfg.ip_source.fallback_urls;

    // proxy (top-level)
    root["proxy"] = cfg.proxy;

    // records
    root["records"] = json::array();
    for (const auto& r : cfg.records) {
        json rj;
        rj["provider"]  = r.provider;
        rj["zone"]      = r.zone;
        rj["name"]      = r.name;
        rj["type"]      = r.type;
        rj["ttl"]       = r.ttl;
        rj["proxied"]   = r.proxied;
        rj["use_proxy"] = r.use_proxy;

        if (!r.api_token.empty())         rj["api_token"]         = r.api_token;
        if (!r.zone_id.empty())           rj["zone_id"]           = r.zone_id;
        if (!r.access_key_id.empty())     rj["access_key_id"]     = r.access_key_id;
        if (!r.access_key_secret.empty()) rj["access_key_secret"] = r.access_key_secret;

        root["records"].push_back(std::move(rj));
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << root.dump(4) << "\n";
    return f.good();
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

} // namespace gecko::config
