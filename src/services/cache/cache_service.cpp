#include "services/cache/cache_service.hpp"
#include "core/types.hpp"
#include "core/logger.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace alasia::cache {

// ─── FileCacheService Implementation ─────────────────────────────────────────

FileCacheService::FileCacheService(std::string base_dir)
    : base_dir_(std::move(base_dir)) {
    std::error_code ec;
    fs::create_directories(base_dir_, ec);
    if (ec) {
        logger::warning("Failed to create cache directory '{}': {}", base_dir_, ec.message());
    }
}

std::string FileCacheService::get_cache_path(const std::string& key) const {
    return (fs::path(base_dir_) / key).string();
}

std::string FileCacheService::read(const std::string& key) {
    std::string path = get_cache_path(key);
    std::ifstream f(path);
    if (!f.is_open()) {
        return "";
    }
    
    std::string value;
    std::getline(f, value);
    
    // Trim whitespace
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    
    return value;
}

bool FileCacheService::write(const std::string& key, const std::string& value) {
    std::string path = get_cache_path(key);
    std::ofstream f(path);
    if (!f.is_open()) {
        return false;
    }
    f << value;
    f.close();
    
    // Set file permissions to 0600 (owner read/write only)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

void FileCacheService::clear() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(base_dir_, ec)) {
        fs::remove(entry.path(), ec);
    }
}

// ─── Legacy API (for backward compatibility during refactoring) ──────────────

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    };
    trim(ip);
    return ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << ip;
    f.close();
    
    // Set file permissions to 0600 (owner read/write only)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return f.good();
}

std::string get_cache_file_path(const std::string& config_abs_path, const std::string& base_dir) {
    if (!base_dir.empty()) {
        std::error_code ec;
        fs::create_directories(base_dir, ec);
        if (!ec) {
            return (fs::path(base_dir) / CACHE_FILENAME).string();
        }
        logger::error("Failed to create base_dir '{}': {}", base_dir, ec.message());
    }
    return (fs::path(config_abs_path).parent_path() / CACHE_FILENAME).string();
}

std::string get_zone_id_cache_path(const std::string& config_abs_path) {
    return (fs::path(config_abs_path).parent_path() / ZONEID_CACHE_FILENAME).string();
}

std::map<std::string, std::string> read_zone_id_cache(const std::string& path) {
    std::map<std::string, std::string> zone_ids;
    
    std::ifstream f(path);
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

    // Read existing
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

    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

} // namespace alasia::cache
