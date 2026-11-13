#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cache {

struct CacheFileData {
    std::string last_ip;
};

/// Last non-empty line wins.
/// A line with a space is an old "RFC3339 ip" history entry; the IP is the second field.
/// A line without a space is the IP itself.
[[nodiscard]] CacheFileData parse_cache_file(const std::filesystem::path& path);

[[nodiscard]] std::string read_last_ip(const std::filesystem::path& path);

/// Overwrite the cache with a single IP line.
[[nodiscard]] bool write_last_ip(const std::filesystem::path& path, std::string_view ip);

} // namespace cache
