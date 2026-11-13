#include "cache.hpp"

#include "text.hpp"

#include <fstream>
#include <string>
#include <sys/stat.h>

namespace cache {

CacheFileData parse_cache_file(const std::filesystem::path& path) {
    CacheFileData data;
    std::ifstream file{path};
    if (!file.is_open()) return data;

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed{text::trim(line)};
        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::string ip;
        const auto space_pos = trimmed.find(' ');
        if (space_pos == std::string::npos) {
            ip = trimmed;
        } else {
            ip = std::string{text::trim(std::string_view{trimmed}.substr(space_pos + 1))};
        }
        if (!ip.empty()) data.last_ip = std::move(ip);
    }
    return data;
}

std::string read_last_ip(const std::filesystem::path& path) {
    return parse_cache_file(path).last_ip;
}

bool write_last_ip(const std::filesystem::path& path, std::string_view ip) {
    std::ofstream file{path, std::ios::trunc};
    if (!file.is_open()) return false;
    file << ip << '\n';
    if (!file.good()) return false;
    file.close();
    return chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
}

} // namespace cache
