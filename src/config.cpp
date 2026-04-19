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

// ─── Environment variable expansion ──────────────────────────────────────────

/// Expand a single environment variable reference with optional default value
/// Supports: ${VAR}, ${VAR:-default}, ${VAR-default}
static std::string expand_env_var(const std::string& expr) {
    // expr is the content inside ${...}
    std::string key       = expr;
    std::string default_val;
    bool        has_default = false;

    // Check for :- syntax (use default if unset or empty)
    if (auto pos = key.find(":-"); pos != std::string::npos) {
        key = key.substr(0, pos);
        if (pos + 2 <= expr.size()) {
            default_val = expr.substr(pos + 2);
        }
        has_default = true;
    }
    // Check for - syntax (use default if unset)
    else if (auto pos = key.find('-'); pos != std::string::npos) {
        key = key.substr(0, pos);
        if (pos + 1 <= expr.size()) {
            default_val = expr.substr(pos + 1);
        }
        has_default = true;
    }

    const char* env_val = std::getenv(key.c_str());
    if (env_val == nullptr || (has_default && std::string(env_val).empty())) {
        return has_default ? default_val : "";
    }
    return env_val;
}

/// Expand all environment variables in a string
/// Looks for ${...} patterns and replaces them with environment values
static std::string expand_env(const std::string& input) {
    std::string result;
    result.reserve(input.size());

    std::size_t i = 0;
    while (i < input.size()) {
        // Look for ${
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            // Find closing }
            std::size_t end = input.find('}', i + 2);
            if (end != std::string::npos) {
                std::string expr = input.substr(i + 2, end - i - 2);
                result += expand_env_var(expr);
                i = end + 1;
                continue;
            }
        }
        result += input[i];
        ++i;
    }
    return result;
}

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

// ─── Validation helpers ───────────────────────────────────────────────────────

/// Check if a string looks like an environment variable reference ${...}
static bool is_env_var_reference(const std::string& s) {
    if (s.size() < 4) return false;  // Minimum: ${X}
    if (s[0] != '$' || s[1] != '{') return false;
    if (s.back() != '}') return false;
    return true;
}

/// Validate that sensitive values are not stored in plaintext
static bool validate_no_plaintext_secret(const std::string& value, const std::string& field_name) {
    if (value.empty()) {
        logger::error("Config: {} is empty", field_name);
        return false;
    }
    if (!is_env_var_reference(value)) {
        logger::error("Config: {} must use environment variable reference (e.g., ${{VAR_NAME}}), "
                      "plaintext secrets are not allowed for security reasons", field_name);
        return false;
    }
    return true;
}

static bool validate_proxy_url(const std::string& proxy) {
    if (proxy.empty()) return true;
    std::string lower = proxy;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.rfind("http://", 0) == 0) return true;
    if (lower.rfind("https://", 0) == 0) return true;
    if (lower.rfind("socks5://", 0) == 0) return true;
    if (lower.rfind("socks5h://", 0) == 0) return true;
    return false;
}

static bool validate_config(const Config& cfg) {
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }

    bool has_iface = !cfg.general.get_ip.interface_name.empty();
    bool has_urls  = !cfg.general.get_ip.urls.empty();
    if (!has_iface && !has_urls) {
        logger::error("Config: either 'get_ip.interface' or 'get_ip.urls' must be set");
        return false;
    }

    if (!cfg.general.proxy.empty() && !validate_proxy_url(cfg.general.proxy)) {
        logger::error("Config: invalid global proxy format '{}'", cfg.general.proxy);
        return false;
    }

    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const auto& r = cfg.records[i];
        if (r.provider.empty()) {
            logger::error("Config: record[{}]: provider is required", i);
            return false;
        }
        if (r.zone.empty()) {
            logger::error("Config: record[{}]: zone is required", i);
            return false;
        }
        if (r.record.empty()) {
            logger::error("Config: record[{}]: record name is required", i);
            return false;
        }
        if (r.use_proxy && cfg.general.proxy.empty()) {
            logger::error("Config: record[{}]: use_proxy=true but no global proxy set", i);
            return false;
        }

        if (r.provider == "cloudflare") {
            if (!r.cloudflare) {
                logger::error("Config: record[{}]: cloudflare configuration is missing", i);
                return false;
            }
            // Validate raw value (before env expansion) for security
            if (!validate_no_plaintext_secret(r._raw_cloudflare_api_token, 
                    "record[" + std::to_string(i) + "].cloudflare.api_token")) {
                return false;
            }
            // Check if expanded value is empty (env var not set)
            if (r.cloudflare->api_token.empty()) {
                logger::error("Config: record[{}]: cloudflare.api_token environment variable is not set or empty", i);
                return false;
            }
        } else if (r.provider == "aliyun") {
            if (!r.aliyun) {
                logger::error("Config: record[{}]: aliyun configuration is missing", i);
                return false;
            }
            // Validate raw values (before env expansion) for security
            if (!validate_no_plaintext_secret(r._raw_aliyun_access_key_id, 
                    "record[" + std::to_string(i) + "].aliyun.access_key_id")) {
                return false;
            }
            if (!validate_no_plaintext_secret(r._raw_aliyun_access_key_secret, 
                    "record[" + std::to_string(i) + "].aliyun.access_key_secret")) {
                return false;
            }
            // Check if expanded values are empty (env vars not set)
            if (r.aliyun->access_key_id.empty()) {
                logger::error("Config: record[{}]: aliyun.access_key_id environment variable is not set or empty", i);
                return false;
            }
            if (r.aliyun->access_key_secret.empty()) {
                logger::error("Config: record[{}]: aliyun.access_key_secret environment variable is not set or empty", i);
                return false;
            }
        } else {
            logger::error("Config: record[{}]: unsupported provider '{}'", i, r.provider);
            return false;
        }
    }
    return true;
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
        logger::error("配置文件 JSON 格式错误:");
        logger::error("  JSON 解析失败: {}", e.what());
        logger::error("  错误位置: byte offset {}", e.byte);
        logger::error("");
        logger::error("常见错误原因:");
        logger::error("  1. 缺少逗号 (,) - 在对象或数组的元素之间需要逗号分隔");
        logger::error("  2. 多余逗号 - 最后一个元素后不应有逗号");
        logger::error("  3. 引号问题 - 键和字符串值必须使用双引号 \"\"，不能使用单引号");
        logger::error("  4. 括号不匹配 - 检查 {{}} 和 [] 是否正确配对");
        logger::error("  5. 使用了注释 - JSON 标准不支持注释");
        logger::error("  6. 特殊字符未转义 - 如字符串中的引号需要转义");
        logger::error("");
        logger::error("推荐验证工具:");
        logger::error("  - 在线工具: https://jsonlint.com/");
        logger::error("  - 命令行: python -m json.tool config.json");
        logger::error("  - 命令行: cat config.json | jq .");
        return std::nullopt;
    }

    Config cfg;

    // general
    if (root.contains("general")) {
        const auto& g = root["general"];
        cfg.general.log_output = jstr(g, "log_output", std::string(config::DEFAULT_LOG_OUTPUT));
        cfg.general.work_dir   = jstr(g, "work_dir");
        cfg.general.proxy      = expand_env(jstr(g, "proxy"));

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
                // Store raw value for security validation
                std::string raw_api_token = jstr(cj, "api_token");
                std::string raw_zone_id   = jstr(cj, "zone_id");
                // Expand environment variables
                cr.api_token = expand_env(raw_api_token);
                cr.zone_id   = expand_env(raw_zone_id);
                cr.proxied   = jbool(cj, "proxied");
                cr.ttl       = jint(cj, "ttl");
                // Store raw values for validation (use zone_id field temporarily)
                // We'll validate before expansion in validate_config
                r._raw_cloudflare_api_token = raw_api_token;
                r._raw_cloudflare_zone_id   = raw_zone_id;
                r.cloudflare = cr;
            }

            if (rj.contains("aliyun") && rj["aliyun"].is_object()) {
                const auto& aj = rj["aliyun"];
                AliyunRecord ar;
                // Store raw values for security validation
                std::string raw_access_key_id     = jstr(aj, "access_key_id");
                std::string raw_access_key_secret = jstr(aj, "access_key_secret");
                // Expand environment variables
                ar.access_key_id     = expand_env(raw_access_key_id);
                ar.access_key_secret = expand_env(raw_access_key_secret);
                ar.ttl               = jint(aj, "ttl");
                // Store raw values for validation
                r._raw_aliyun_access_key_id     = raw_access_key_id;
                r._raw_aliyun_access_key_secret = raw_access_key_secret;
                r.aliyun = ar;
            }

            cfg.records.push_back(std::move(r));
        }
    }

    // Expand environment variables in proxy
    if (!cfg.general.proxy.empty()) {
        cfg.general.proxy = expand_env(cfg.general.proxy);
    }

    if (!validate_config(cfg)) return std::nullopt;

    return cfg;
}

// ─── Write ────────────────────────────────────────────────────────────────────

bool write_config(const std::string& path, const Config& cfg) {
    json root;
    root["general"]["log_output"]       = cfg.general.log_output;
    root["general"]["work_dir"]         = cfg.general.work_dir;
    root["general"]["proxy"]            = cfg.general.proxy;
    root["general"]["get_ip"]["interface"] = cfg.general.get_ip.interface_name;
    root["general"]["get_ip"]["urls"]   = cfg.general.get_ip.urls;

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
            // Use raw values to avoid writing plaintext secrets
            // Only write environment variable references (e.g., ${VAR})
            rj["cloudflare"]["api_token"] = r._raw_cloudflare_api_token.empty() 
                ? r.cloudflare->api_token 
                : r._raw_cloudflare_api_token;
            rj["cloudflare"]["zone_id"]   = r._raw_cloudflare_zone_id.empty()
                ? r.cloudflare->zone_id
                : r._raw_cloudflare_zone_id;
            rj["cloudflare"]["proxied"]   = r.cloudflare->proxied;
            rj["cloudflare"]["ttl"]       = r.cloudflare->ttl;
        }
        if (r.aliyun) {
            // Use raw values to avoid writing plaintext secrets
            rj["aliyun"]["access_key_id"]     = r._raw_aliyun_access_key_id.empty()
                ? r.aliyun->access_key_id
                : r._raw_aliyun_access_key_id;
            rj["aliyun"]["access_key_secret"] = r._raw_aliyun_access_key_secret.empty()
                ? r.aliyun->access_key_secret
                : r._raw_aliyun_access_key_secret;
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
                                 const std::string& work_dir) {
    if (!work_dir.empty()) {
        std::error_code ec;
        fs::create_directories(work_dir, ec);
        if (!ec) {
            return (fs::path(work_dir) / config::CACHE_FILENAME).string();
        }
        logger::error("Failed to create work_dir '{}': {}", work_dir, ec.message());
    }
    return (fs::path(config_abs_path).parent_path() / config::CACHE_FILENAME).string();
}

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    // trim whitespace
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
        // Ignore parse errors, return empty map
    }
    return zone_ids;
}

bool update_zone_id_cache(const std::string& path, const std::string& zone, const std::string& zone_id) {
    std::map<std::string, std::string> zone_ids;

    // Read existing cache
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
            // Ignore parse errors, start fresh
        }
    }

    // Update
    zone_ids[zone] = zone_id;

    // Write back
    json j = json::object();
    for (const auto& [z, id] : zone_ids) {
        j[z] = id;
    }

    std::ofstream f_out(path);
    if (!f_out.is_open()) return false;
    f_out << j.dump(4) << "\n";
    if (!f_out.good()) return false;
    f_out.close();
    
    // Set file permissions to 0600 (owner read/write only)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

} // namespace config
