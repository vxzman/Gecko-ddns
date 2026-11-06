#pragma once
#include <string>

namespace cache {

struct CacheFileData {
    std::string last_ip;
};

/// Last non-empty line wins.
/// A line with a space is an old "RFC3339 ip" history entry; the IP is the second field.
/// A line without a space is the IP itself.
CacheFileData parse_cache_file(const std::string& path);

/// Read last IP (empty string on miss).
std::string read_last_ip(const std::string& path);

/// Overwrite the cache with a single IP line.
bool write_last_ip(const std::string& path, const std::string& ip);

} // namespace cache
