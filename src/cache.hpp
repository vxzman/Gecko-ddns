#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace cache {

struct IPHistoryEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string                           ip;
};

struct CacheFileData {
    std::string                 last_ip;
    std::vector<IPHistoryEntry> history;
};

/// Parse cache file (format: "ISO8601_ip" per line, last_ip derived from last entry)
CacheFileData parse_cache_file(const std::string& path);

/// Write cache data to file
bool write_cache_file(const std::string& path, const CacheFileData& data);

/// Legacy: read last IP (derived from last history entry)
std::string read_last_ip(const std::string& path);

/// Legacy: append IP with current timestamp
bool write_last_ip(const std::string& path, const std::string& ip);

} // namespace cache
