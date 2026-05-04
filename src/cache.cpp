#include "cache.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace cache {

namespace {

/// Trim whitespace from both ends
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Parse an ISO 8601 / RFC 3339 timestamp string (e.g. "2026-05-01T10:30:00Z")
static std::chrono::system_clock::time_point parse_iso8601(const std::string& ts) {
    std::tm tm{};
    std::istringstream ss(ts);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) {
        return std::chrono::system_clock::time_point{};
    }
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

/// Format a time_point as "YYYY-MM-DDTHH:MM:SSZ"
static std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::gmtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace

CacheFileData parse_cache_file(const std::string& path) {
    CacheFileData data;

    std::ifstream f(path);
    if (!f.is_open()) return data;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Parse "YYYY-MM-DDTHH:MM:SSZ ip"
        auto space_pos = line.find(' ');
        if (space_pos == std::string::npos) continue;

        std::string ts_str = trim(line.substr(0, space_pos));
        std::string ip = trim(line.substr(space_pos + 1));
        if (ip.empty()) continue;

        auto tp = parse_iso8601(ts_str);
        if (tp == std::chrono::system_clock::time_point{}) continue;

        data.history.push_back({tp, ip});
    }

    // last_ip derived from last history entry
    if (!data.history.empty()) {
        data.last_ip = data.history.back().ip;
    }

    return data;
}

bool write_cache_file(const std::string& path, const CacheFileData& data) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    for (const auto& entry : data.history) {
        f << format_iso8601(entry.timestamp) << ' ' << entry.ip << '\n';
    }

    if (!f.good()) return false;
    f.close();

    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

std::string read_last_ip(const std::string& path) {
    return parse_cache_file(path).last_ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    CacheFileData data = parse_cache_file(path);
    data.history.push_back({
        std::chrono::system_clock::now(),
        ip
    });
    return write_cache_file(path, data);
}

} // namespace cache
