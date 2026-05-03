#include "config.hpp"
#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace config {

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static std::string jstr(const json& j, const char* key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static bool jbool(const json& j, const char* key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

static int jint(const json& j, const char* key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    return def;
}

// ─── Environment variable resolution ──────────────────────────────────────────

/// Resolve $name reference from cfg.environment
/// Only supports $name syntax (e.g., $cloudflare_token)
/// Returns empty string if variable not found
static std::string resolve_env_var(const std::string& expr, const std::map<std::string, std::string>& environment) {
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

// ─── Validation helpers ───────────────────────────────────────────────────────

static bool validate_proxy_url(const std::string& proxy) {
    if (proxy.empty()) return true;
    std::string lower = proxy;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.rfind("http://", 0) == 0 ||
           lower.rfind("https://", 0) == 0 ||
           lower.rfind("socks5://", 0) == 0 ||
           lower.rfind("socks5h://", 0) == 0;
}

static bool validate_general_config(const GeneralConfig& general) {
    bool has_iface = !general.get_ip.interface_name.empty();
    bool has_urls  = !general.get_ip.urls.empty();
    
    if (!has_iface && !has_urls) {
        logger::error("Config: either 'get_ip.interface' or 'get_ip.urls' must be set");
        return false;
    }

    if (!general.proxy.empty() && !validate_proxy_url(general.proxy)) {
        logger::error("Config: invalid global proxy format '{}'", general.proxy);
        return false;
    }

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
    if (r.record.empty()) {
        logger::error("Config: record[{}]: record name is required", index);
        return false;
    }
    if (r.use_proxy && global_proxy.empty()) {
        logger::error("Config: record[{}]: use_proxy=true but no global proxy set", index);
        return false;
    }

    if (r.provider == "cloudflare") {
        if (!r.cloudflare) {
            logger::error("Config: record[{}]: cloudflare configuration is missing", index);
            return false;
        }
        if (r.cloudflare->api_token.empty()) {
            logger::error("Config: record[{}]: cloudflare.api_token is not set or empty", index);
            return false;
        }
    } else if (r.provider == "aliyun") {
        if (!r.aliyun) {
            logger::error("Config: record[{}]: aliyun configuration is missing", index);
            return false;
        }
        if (r.aliyun->access_key_id.empty()) {
            logger::error("Config: record[{}]: aliyun.access_key_id is not set or empty", index);
            return false;
        }
        if (r.aliyun->access_key_secret.empty()) {
            logger::error("Config: record[{}]: aliyun.access_key_secret is not set or empty", index);
            return false;
        }
    } else {
        logger::error("Config: record[{}]: unsupported provider '{}'", index, r.provider);
        return false;
    }
    return true;
}

static bool validate_all_records(const std::vector<RecordConfig>& records, const std::string& global_proxy) {
    for (size_t i = 0; i < records.size(); ++i) {
        if (!validate_record(records[i], i, global_proxy)) {
            return false;
        }
    }
    return true;
}

static bool validate_config(const Config& cfg) {
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }

    return validate_general_config(cfg.general) &&
           validate_all_records(cfg.records, cfg.general.proxy);
}

// ─── Parse ────────────────────────────────────────────────────────────────────

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

    Config cfg;

    // environment
    if (root.contains("environment") && root["environment"].is_object()) {
        for (const auto& [key, value] : root["environment"].items()) {
            if (value.is_string()) {
                cfg.environment[key] = value.get<std::string>();
            }
        }
    }

    // general
    if (root.contains("general")) {
        const auto& g = root["general"];
        cfg.general.proxy      = jstr(g, "proxy");

        if (g.contains("get_ip")) {
            const auto& gi = g["get_ip"];
            cfg.general.get_ip.interface_name = jstr(gi, "interface");
            if (gi.contains("urls") && gi["urls"].is_array()) {
                for (const auto& u : gi["urls"]) {
                    if (u.is_string()) cfg.general.get_ip.urls.push_back(u.get<std::string>());
                }
            }
        }
    }

    // records
    if (root.contains("records") && root["records"].is_array()) {
        for (const auto& rj : root["records"]) {
            RecordConfig r;
            r.provider  = jstr(rj, "provider");
            r.zone      = jstr(rj, "zone");
            r.record    = jstr(rj, "record");
            r.ttl       = jint(rj, "ttl");
            r.proxied   = jbool(rj, "proxied");
            r.use_proxy = jbool(rj, "use_proxy");

            if (rj.contains("cloudflare") && rj["cloudflare"].is_object()) {
                const auto& cj = rj["cloudflare"];
                CloudflareRecord cr;
                std::string api_token = jstr(cj, "api_token");
                std::string zone_id   = jstr(cj, "zone_id");
                // Resolve $name references
                cr.api_token = resolve_env_var(api_token, cfg.environment);
                cr.zone_id   = resolve_env_var(zone_id, cfg.environment);
                cr.proxied   = jbool(cj, "proxied");
                cr.ttl       = jint(cj, "ttl");
                r.cloudflare = cr;
            }

            if (rj.contains("aliyun") && rj["aliyun"].is_object()) {
                const auto& aj = rj["aliyun"];
                AliyunRecord ar;
                std::string access_key_id     = jstr(aj, "access_key_id");
                std::string access_key_secret = jstr(aj, "access_key_secret");
                // Resolve $name references
                ar.access_key_id     = resolve_env_var(access_key_id, cfg.environment);
                ar.access_key_secret = resolve_env_var(access_key_secret, cfg.environment);
                ar.ttl               = jint(aj, "ttl");
                r.aliyun = ar;
            }

            cfg.records.push_back(std::move(r));
        }
    }

    if (!validate_config(cfg)) return std::nullopt;

    return cfg;
}

// ─── Write ────────────────────────────────────────────────────────────────────

bool write_config(const std::string& path, const Config& cfg) {
    json root;
    
    // environment
    root["environment"] = json::object();
    for (const auto& [key, value] : cfg.environment) {
        root["environment"][key] = value;
    }
    
    // general
    root["general"]["proxy"]      = cfg.general.proxy;
    root["general"]["get_ip"]["interface"] = cfg.general.get_ip.interface_name;
    root["general"]["get_ip"]["urls"]      = cfg.general.get_ip.urls;

    // records
    root["records"] = json::array();
    for (const auto& r : cfg.records) {
        json rj;
        rj["provider"]  = r.provider;
        rj["zone"]      = r.zone;
        rj["record"]    = r.record;
        rj["ttl"]       = r.ttl;
        rj["proxied"]   = r.proxied;
        rj["use_proxy"] = r.use_proxy;

        if (r.cloudflare) {
            rj["cloudflare"]["api_token"] = r.cloudflare->api_token;
            rj["cloudflare"]["zone_id"]   = r.cloudflare->zone_id;
            rj["cloudflare"]["proxied"]   = r.cloudflare->proxied;
            rj["cloudflare"]["ttl"]       = r.cloudflare->ttl;
        }
        if (r.aliyun) {
            rj["aliyun"]["access_key_id"]     = r.aliyun->access_key_id;
            rj["aliyun"]["access_key_secret"] = r.aliyun->access_key_secret;
            rj["aliyun"]["ttl"]               = r.aliyun->ttl;
        }
        root["records"].push_back(std::move(rj));
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << root.dump(4) << "\n";
    return f.good();
}

// ─── Cache helpers ────────────────────────────────────────────────────────────

std::string get_cache_file_path(const std::string& config_abs_path,
                                 const std::string& base_dir) {
    if (!base_dir.empty()) {
        std::error_code ec;
        fs::create_directories(base_dir, ec);
        if (!ec) {
            return (fs::path(base_dir) / config::CACHE_FILENAME).string();
        }
        logger::error("Failed to create base_dir '{}': {}", base_dir, ec.message());
    }
    return (fs::path(config_abs_path).parent_path() / config::CACHE_FILENAME).string();
}

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    };
    trim(ip);
    return ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << ip;
    return f.good();
}

// ─── Record helpers ───────────────────────────────────────────────────────────

std::string get_record_proxy(const Config& cfg, const RecordConfig& record) {
    if (!record.use_proxy) return "";
    return cfg.general.proxy;
}

int get_record_ttl(const RecordConfig& record) {
    if (record.ttl > 0) return record.ttl;
    if (record.provider == "cloudflare") return config::DEFAULT_CLOUDFLARE_TTL;
    return config::DEFAULT_ALIYUN_TTL;
}

// ─── ZoneID cache helpers ─────────────────────────────────────────────────────

std::string get_zone_id_cache_path(const std::string& config_abs_path) {
    return (fs::path(config_abs_path).parent_path() / config::ZONEID_CACHE_FILENAME).string();
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
